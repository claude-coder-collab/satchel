// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/restore.hpp"

#include "codecs/flac/parallel_decode.hpp"

#include "codecs/flac/flac_codec.hpp"
#include "crypto/hash.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <thread>

namespace zp::flac
{

namespace
{

// A read-only file made of byte ranges and runs of zeros, used to re-run the container scan
// on the foreign records without the audio.
class VirtualFile final : public IChunkedStream
{
public:
    struct Part
    {
        std::span<const std::uint8_t> bytes;
        std::uint64_t zeros = 0;
        [[nodiscard]] std::uint64_t size() const { return bytes.empty() ? zeros : bytes.size(); }
    };

    explicit VirtualFile(std::vector<Part> parts) :
        parts_(std::move(parts))
    {
        for (const auto& p : parts_)
            size_ += p.size();
    }

    Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override
    {
        std::size_t done = 0;
        std::uint64_t base = 0;
        for (const auto& p : parts_)
        {
            const auto end = base + p.size();
            if (pos_ < end && done < len)
            {
                const auto off = pos_ - base;
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(len - done, end - pos_));
                if (p.bytes.empty())
                    std::memset(buf + done, 0, n);
                else
                    std::memcpy(buf + done, p.bytes.data() + off, n);
                done += n;
                pos_ += n;
            }
            base = end;
        }
        return done;
    }
    [[nodiscard]] bool seekable() const override { return true; }
    VoidResult seek(std::uint64_t pos) override
    {
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override { return size_; }

private:
    std::vector<Part> parts_;
    std::uint64_t size_ = 0;
    std::uint64_t pos_ = 0;
};

bool is_audio_record(const ForeignRecord& r)
{
    static constexpr std::array<std::uint8_t, 16> w64_data = { 'd', 'a', 't', 'a', 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A };
    const auto& b = r.bytes;
    if (b.size() >= 16 && std::memcmp(b.data(), w64_data.data(), 16) == 0)
        return true;
    if (b.size() >= 4 && (std::memcmp(b.data(), "data", 4) == 0 || std::memcmp(b.data(), "SSND", 4) == 0))
        return true;
    return false;
}

}

Result<std::vector<std::size_t>> order_members(const std::vector<Header>& headers)
{
    if (headers.empty())
        return fail(Status::InvalidArgument, "no FLAC stream");
    const auto& only = headers[0].project;
    if (headers.size() == 1 && (!only.has_value() || only->layout != Layout::MultiMonoMember))
        return std::vector<std::size_t>{ 0 };
    const auto& first = headers[0].project;
    if (!first)
        return fail(Status::IncompleteGroup, "multi-mono member without a project block");
    const auto count = first->channel_count;
    if (headers.size() != count)
        return fail(Status::IncompleteGroup, std::format("{} of {} channels present", headers.size(), count));
    std::vector<std::size_t> order(count, headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        const auto& p = headers[i].project;
        if (!p || p->layout != Layout::MultiMonoMember || p->group_id != first->group_id || p->channel_count != count || p->sha256 != first->sha256)
            return fail(Status::IncompleteGroup, "multi-mono members do not belong together");
        if (p->channel_index < 1 || p->channel_index > count || order[p->channel_index - 1] != headers.size())
            return fail(Status::IncompleteGroup, "duplicate or invalid channel index");
        if (headers[i].stream_info.channels != 1 || headers[i].stream_info.total_samples != headers[0].stream_info.total_samples
            || headers[i].stream_info.bits_per_sample != headers[0].stream_info.bits_per_sample)
            return fail(Status::IncompleteGroup, "multi-mono members have different formats");
        order[p->channel_index - 1] = i;
    }
    return order;
}

Result<PcmLayout> layout_from_records(const std::vector<ForeignRecord>& records, std::uint32_t channels, std::uint32_t bytes_per_sample, std::uint64_t frames, std::uint64_t trailing_size)
{
    const auto audio = std::ranges::find_if(records, is_audio_record);
    if (audio == records.end())
        return fail(Status::CorruptArchive, "foreign metadata has no audio chunk header");
    const auto audio_bytes = frames * channels * bytes_per_sample;
    for (std::uint64_t pad = 0; pad < 8; ++pad)
    {
        std::vector<VirtualFile::Part> parts;
        for (auto it = records.begin(); it != records.end(); ++it)
        {
            parts.push_back({ it->bytes, 0 });
            if (it == audio)
            {
                if (audio_bytes > 0)
                    parts.push_back({ {}, audio_bytes });
                if (pad > 0)
                    parts.push_back({ {}, pad });
            }
        }
        if (trailing_size > 0)
            parts.push_back({ {}, trailing_size });
        VirtualFile vf(std::move(parts));
        auto scan = scan_pcm(vf);
        if (!scan)
            return std::unexpected(scan.error());
        if (scan->layout && scan->layout->audio_padding == pad && scan->layout->blocks.size() == records.size() && scan->layout->audio_size == audio_bytes
            && scan->layout->channels == channels && scan->layout->bytes_per_sample == bytes_per_sample && scan->layout->trailing_size == trailing_size)
            return *scan->layout;
    }
    return fail(Status::CorruptArchive, "foreign metadata does not describe the FLAC audio");
}

VoidResult restore(const std::vector<Header>& headers, const MemberOpener& open, const RestoreSink& sink, std::size_t threads)
{
    auto order = order_members(headers);
    if (!order)
        return std::unexpected(order.error());
    const auto& lead = headers[(*order)[0]];
    const auto& info = lead.stream_info;
    if (info.bits_per_sample % 8 != 0)
        return fail(Status::UnsupportedMethod, std::format("{}-bit FLAC cannot be mapped to the original container", info.bits_per_sample));
    const bool mono = order->size() > 1 || (lead.project && lead.project->layout == Layout::MultiMonoMember);
    const std::uint32_t channels = mono ? static_cast<std::uint32_t>(order->size()) : info.channels;
    const std::uint32_t width = info.bits_per_sample / 8;

    std::vector<ForeignRecord> records;
    std::vector<std::uint8_t> trailing;
    if (lead.project && lead.project->layout != Layout::Standard)
    {
        auto unpacked = unpack_private(lead.project->private_data);
        if (!unpacked)
            return std::unexpected(unpacked.error());
        records = std::move(*unpacked);
    }
    else
        records = lead.foreign;
    if (lead.project)
        trailing = lead.project->trailing;
    if (records.empty())
        return fail(Status::CorruptArchive, "FLAC file carries no foreign metadata");

    auto layout = layout_from_records(records, channels, width, info.total_samples, trailing.size());
    if (!layout)
        return std::unexpected(layout.error());

    std::vector<std::unique_ptr<IChunkedStream>> streams;
    std::vector<std::unique_ptr<Decoder>> decoders;
    const bool parallel = !mono && threads > 1 && lead.project.has_value();
    if (parallel)
    {
        auto s = open((*order)[0]);
        if (!s)
            return std::unexpected(s.error());
        std::vector<std::uint8_t> skip(64u << 10);
        for (std::uint64_t left = lead.metadata_size; left > 0;)
        {
            auto n = (*s)->read(skip.data(), static_cast<std::size_t>(std::min<std::uint64_t>(left, skip.size())));
            if (!n)
                return std::unexpected(n.error());
            if (*n == 0)
                return fail(Status::CorruptArchive, "FLAC file ends inside its metadata");
            left -= *n;
        }
        streams.push_back(std::move(*s));
    }
    for (const auto m : *order)
    {
        if (parallel)
            break;
        auto s = open(m);
        if (!s)
            return std::unexpected(s.error());
        auto d = Decoder::open(**s, !lead.project);
        if (!d)
            return std::unexpected(d.error());
        if ((*d)->bits_per_sample() != info.bits_per_sample || (*d)->channels() != (mono ? 1 : channels))
            return fail(Status::CorruptArchive, "FLAC stream format does not match its metadata");
        streams.push_back(std::move(*s));
        decoders.push_back(std::move(*d));
    }

    Sha256 sha;
    const auto emit = [&](std::span<const std::uint8_t> bytes) -> VoidResult {
        sha.update(bytes);
        return sink(bytes);
    };

    std::vector<std::int32_t> interleaved;
    std::vector<std::uint8_t> out;
    const auto write_audio = [&]() -> VoidResult {
        if (parallel)
        {
            const auto convert = [&](std::span<const std::int32_t> samples, std::vector<std::uint8_t>& bytes) {
                bytes.resize(samples.size() * width);
                encode_samples(*layout, samples, bytes);
            };
            auto decoded = decode_parallel(*streams[0], info, { .threads = threads }, convert, emit);
            if (!decoded)
                return std::unexpected(decoded.error());
            if (*decoded != info.total_samples)
                return fail(Status::CorruptArchive, "FLAC stream has a different length than its STREAMINFO");
            return {};
        }
        std::uint64_t written = 0;
        if (mono && threads > 1)
        {
            constexpr std::size_t round_samples = 32 * 4096;
            const std::size_t members = decoders.size();
            std::vector<std::vector<std::int32_t>> pending(members);
            std::vector<char> ended(members, 0);
            std::vector<std::optional<Error>> errors(members);
            while (true)
            {
                const auto worker = [&](std::size_t first) {
                    for (std::size_t c = first; c < members; c += threads)
                    {
                        while (!ended[c] && pending[c].size() < round_samples)
                        {
                            auto block = decoders[c]->next();
                            if (!block)
                            {
                                errors[c] = block.error();
                                ended[c] = 1;
                                break;
                            }
                            if (block->empty())
                                ended[c] = 1;
                            pending[c].insert(pending[c].end(), block->begin(), block->end());
                        }
                    }
                };
                std::vector<std::thread> pool;
                pool.reserve(std::min(threads, members));
                for (std::size_t t = 0; t < std::min(threads, members); ++t)
                    pool.emplace_back(worker, t);
                for (auto& t : pool)
                    t.join();
                for (const auto& e : errors)
                {
                    if (e)
                        return std::unexpected(*e);
                }
                const auto frames = std::ranges::min(pending, {}, &std::vector<std::int32_t>::size).size();
                if (frames == 0)
                {
                    if (std::ranges::any_of(pending, [](const auto& p) { return !p.empty(); }))
                        return fail(Status::IncompleteGroup, "multi-mono members are not aligned");
                    break;
                }
                interleaved.resize(frames * channels);
                for (std::size_t c = 0; c < members; ++c)
                {
                    for (std::size_t f = 0; f < frames; ++f)
                        interleaved[f * channels + c] = pending[c][f];
                    pending[c].erase(pending[c].begin(), pending[c].begin() + static_cast<std::ptrdiff_t>(frames));
                }
                out.resize(frames * channels * width);
                encode_samples(*layout, interleaved, out);
                if (auto r = emit(out); !r)
                    return r;
                written += frames;
            }
            if (written != info.total_samples)
                return fail(Status::CorruptArchive, "FLAC stream has a different length than its STREAMINFO");
            for (auto& d : decoders)
            {
                if (auto r = d->finish(); !r)
                    return r;
            }
            return {};
        }
        while (true)
        {
            std::size_t frames = 0;
            if (!mono)
            {
                auto block = decoders[0]->next();
                if (!block)
                    return std::unexpected(block.error());
                frames = block->size() / channels;
                interleaved.assign(block->begin(), block->end());
            }
            else
            {
                for (std::size_t c = 0; c < decoders.size(); ++c)
                {
                    auto block = decoders[c]->next();
                    if (!block)
                        return std::unexpected(block.error());
                    if (c == 0)
                    {
                        frames = block->size();
                        interleaved.resize(frames * channels);
                    }
                    else if (block->size() != frames)
                        return fail(Status::IncompleteGroup, "multi-mono members are not aligned");
                    for (std::size_t f = 0; f < frames; ++f)
                        interleaved[f * channels + c] = (*block)[f];
                }
            }
            if (frames == 0)
                break;
            out.resize(frames * channels * width);
            encode_samples(*layout, interleaved, out);
            if (auto r = emit(out); !r)
                return r;
            written += frames;
        }
        if (written != info.total_samples)
            return fail(Status::CorruptArchive, "FLAC stream has a different length than its STREAMINFO");
        for (auto& d : decoders)
        {
            if (auto r = d->finish(); !r)
                return r;
        }
        return {};
    };

    for (std::size_t i = 0; i < records.size(); ++i)
    {
        if (auto r = emit(records[i].bytes); !r)
            return r;
        if (layout->blocks[i].audio_header)
        {
            if (auto r = write_audio(); !r)
                return r;
            if (layout->audio_padding > 0)
            {
                const std::vector<std::uint8_t> pad(static_cast<std::size_t>(layout->audio_padding), 0);
                if (auto r = emit(pad); !r)
                    return r;
            }
        }
    }
    if (!trailing.empty())
    {
        if (auto r = emit(trailing); !r)
            return r;
    }
    if (lead.project && sha.finish() != lead.project->sha256)
        return fail(Status::HashMismatch, "restored file does not match its SHA-256");
    return {};
}

}
