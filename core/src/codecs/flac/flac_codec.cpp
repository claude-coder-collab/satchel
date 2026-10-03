// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/flac_codec.hpp"

#include <FLAC/format.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <algorithm>
#include <format>

namespace zp::flac
{

namespace
{

struct EncoderHandle
{
    FLAC__StreamEncoder* e = FLAC__stream_encoder_new();
    EncoderHandle() = default;
    EncoderHandle(const EncoderHandle&) = delete;
    EncoderHandle& operator=(const EncoderHandle&) = delete;
    EncoderHandle(EncoderHandle&&) = delete;
    EncoderHandle& operator=(EncoderHandle&&) = delete;
    ~EncoderHandle()
    {
        if (e)
            FLAC__stream_encoder_delete(e);
    }
};

struct Collector
{
    std::vector<Frame> frames;
};

FLAC__StreamEncoderWriteStatus write_frames(const FLAC__StreamEncoder*, const FLAC__byte* buffer, size_t bytes, uint32_t samples, uint32_t, void* user)
{
    if (samples == 0)
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    auto* c = static_cast<Collector*>(user);
    c->frames.push_back({ std::vector<std::uint8_t>(buffer, buffer + bytes) });
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

bool configure(FLAC__StreamEncoder* e, const EncoderSettings& s, bool subset)
{
    return FLAC__stream_encoder_set_channels(e, s.channels) && FLAC__stream_encoder_set_bits_per_sample(e, s.bits_per_sample)
        && FLAC__stream_encoder_set_sample_rate(e, s.sample_rate) && FLAC__stream_encoder_set_compression_level(e, static_cast<uint32_t>(s.level))
        && FLAC__stream_encoder_set_blocksize(e, block_size) && FLAC__stream_encoder_set_streamable_subset(e, subset)
        && FLAC__stream_encoder_set_verify(e, false);
}

}

std::string vendor_string()
{
    return FLAC__VENDOR_STRING;
}

Result<EncodedSegment> encode_segment(const EncoderSettings& settings, std::span<const std::int32_t> interleaved, std::uint64_t first_frame_number)
{
    Collector collector;
    bool initialized = false;
    std::unique_ptr<EncoderHandle> enc;
    for (const bool subset : { true, false })
    {
        enc = std::make_unique<EncoderHandle>();
        if (!enc->e || !configure(enc->e, settings, subset))
            return fail(Status::Internal, "cannot configure the FLAC encoder");
        const auto st = FLAC__stream_encoder_init_stream(enc->e, write_frames, nullptr, nullptr, nullptr, &collector);
        if (st == FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        {
            initialized = true;
            break;
        }
        if (st != FLAC__STREAM_ENCODER_INIT_STATUS_NOT_STREAMABLE)
            return fail(Status::Internal, std::format("FLAC encoder init failed: {}", FLAC__StreamEncoderInitStatusString[st]));
    }
    if (!initialized)
        return fail(Status::Internal, "FLAC encoder init failed");

    const auto frames = static_cast<uint32_t>(interleaved.size() / settings.channels);
    if (frames > 0 && !FLAC__stream_encoder_process_interleaved(enc->e, interleaved.data(), frames))
        return fail(Status::Internal, std::format("FLAC encoding failed: {}", FLAC__stream_encoder_get_resolved_state_string(enc->e)));
    if (!FLAC__stream_encoder_finish(enc->e))
        return fail(Status::Internal, std::format("FLAC encoding failed: {}", FLAC__stream_encoder_get_resolved_state_string(enc->e)));

    EncodedSegment out;
    out.min_frame_size = 0;
    for (std::size_t i = 0; i < collector.frames.size(); ++i)
    {
        auto renumbered = renumber_frame(collector.frames[i].bytes, first_frame_number + i);
        if (!renumbered)
            return std::unexpected(renumbered.error());
        const auto size = static_cast<std::uint32_t>(renumbered->size());
        out.min_frame_size = out.frame_count == 0 ? size : std::min(out.min_frame_size, size);
        out.max_frame_size = std::max(out.max_frame_size, size);
        ++out.frame_count;
        out.bytes.insert(out.bytes.end(), renumbered->begin(), renumbered->end());
    }
    return out;
}

struct Decoder::Impl
{
    FLAC__StreamDecoder* d = nullptr;
    IChunkedStream* in = nullptr;
    std::vector<std::int32_t> block;
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;
    std::uint64_t total = 0;
    bool got_block = false;
    std::optional<Error> error;
    bool eof = false;
};

struct DecoderCallbacks
{
    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder*, FLAC__byte* buffer, size_t* bytes, void* user)
    {
        auto* d = static_cast<Decoder::Impl*>(user);
        auto n = d->in->read(buffer, *bytes);
        if (!n)
        {
            d->error = n.error();
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        *bytes = *n;
        if (*n == 0)
        {
            d->eof = true;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const* buffer, void* user)
    {
        auto* d = static_cast<Decoder::Impl*>(user);
        const auto n = frame->header.blocksize;
        const auto ch = frame->header.channels;
        d->block.resize(static_cast<std::size_t>(n) * ch);
        for (uint32_t i = 0; i < n; ++i)
        {
            for (uint32_t c = 0; c < ch; ++c)
                d->block[static_cast<std::size_t>(i) * ch + c] = buffer[c][i];
        }
        d->got_block = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void metadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* user)
    {
        if (m->type != FLAC__METADATA_TYPE_STREAMINFO)
            return;
        auto* d = static_cast<Decoder::Impl*>(user);
        d->channels = m->data.stream_info.channels;
        d->bits = m->data.stream_info.bits_per_sample;
        d->total = m->data.stream_info.total_samples;
    }

    static void error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* user)
    {
        auto* d = static_cast<Decoder::Impl*>(user);
        if (!d->error)
            d->error = Error{ Status::CorruptArchive, std::format("FLAC decoding error: {}", FLAC__StreamDecoderErrorStatusString[status]) };
    }
};

Result<std::unique_ptr<Decoder>> Decoder::open(IChunkedStream& in)
{
    std::unique_ptr<Decoder> dec(new Decoder());
    dec->impl_ = std::make_unique<Impl>();
    auto& impl = *dec->impl_;
    impl.in = &in;
    impl.d = FLAC__stream_decoder_new();
    if (!impl.d)
        return fail(Status::Internal, "cannot create a FLAC decoder");
    FLAC__stream_decoder_set_md5_checking(impl.d, true);
    const auto st = FLAC__stream_decoder_init_stream(impl.d, DecoderCallbacks::read, nullptr, nullptr, nullptr, nullptr, DecoderCallbacks::write, DecoderCallbacks::metadata, DecoderCallbacks::error, &impl);
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return fail(Status::Internal, "cannot initialise the FLAC decoder");
    if (!FLAC__stream_decoder_process_until_end_of_metadata(impl.d) || impl.error)
        return std::unexpected(impl.error.value_or(Error{ Status::CorruptArchive, "FLAC metadata could not be read" }));
    dec->channels_ = impl.channels;
    dec->bits_ = impl.bits;
    dec->total_ = impl.total;
    return dec;
}

Decoder::~Decoder()
{
    if (impl_ && impl_->d)
        FLAC__stream_decoder_delete(impl_->d);
}

Result<std::span<const std::int32_t>> Decoder::next()
{
    auto& impl = *impl_;
    impl.got_block = false;
    while (!impl.got_block)
    {
        if (FLAC__stream_decoder_get_state(impl.d) == FLAC__STREAM_DECODER_END_OF_STREAM)
            return std::span<const std::int32_t>{};
        if (!FLAC__stream_decoder_process_single(impl.d) || impl.error)
            return std::unexpected(impl.error.value_or(Error{ Status::CorruptArchive, "FLAC frame could not be decoded" }));
    }
    return std::span<const std::int32_t>(impl.block);
}

VoidResult Decoder::finish()
{
    if (!FLAC__stream_decoder_finish(impl_->d))
        return fail(Status::HashMismatch, "FLAC MD5 signature does not match the decoded audio");
    return {};
}

}
