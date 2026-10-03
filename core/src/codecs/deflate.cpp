// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/deflate.hpp"

#include <format>
#include <zlib-ng.h>

namespace zp
{

namespace
{

class Deflater
{
public:
    explicit Deflater(int level) :
        ok_(zng_deflateInit2(&s_, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK)
    {
    }
    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;
    Deflater(Deflater&&) = delete;
    Deflater& operator=(Deflater&&) = delete;
    ~Deflater()
    {
        if (ok_)
            zng_deflateEnd(&s_);
    }

    [[nodiscard]] bool ok() const { return ok_; }
    zng_stream* get() { return &s_; }

private:
    zng_stream s_{};
    bool ok_ = false;
};

Result<std::vector<std::uint8_t>> run_deflate(std::span<const std::uint8_t> history, std::span<const std::uint8_t> input, int level, int flush)
{
    Deflater d(level);
    if (!d.ok())
        return fail(Status::Internal, "deflateInit2 failed");
    auto* s = d.get();
    if (!history.empty())
    {
        if (zng_deflateSetDictionary(s, history.data(), static_cast<uint32_t>(history.size())) != Z_OK)
            return fail(Status::Internal, "deflateSetDictionary failed");
    }
    std::vector<std::uint8_t> out(zng_deflateBound(s, input.size()) + 16);
    s->next_in = input.data();
    s->avail_in = static_cast<uint32_t>(input.size());
    s->next_out = out.data();
    s->avail_out = static_cast<uint32_t>(out.size());
    while (true)
    {
        const int rc = zng_deflate(s, flush);
        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            return fail(Status::Internal, std::format("deflate failed ({})", rc));
        if (flush != Z_FINISH && s->avail_in == 0 && s->avail_out > 0)
            break;
        if (s->avail_out == 0)
        {
            const auto used = out.size();
            out.resize(out.size() * 2);
            s->next_out = out.data() + used;
            s->avail_out = static_cast<uint32_t>(out.size() - used);
        }
    }
    out.resize(out.size() - s->avail_out);
    return out;
}

}

Result<std::vector<std::uint8_t>> DeflateSegmentEncoder::encode(std::span<const std::uint8_t> history, std::vector<std::uint8_t>&& input, bool last) const
{
    const auto data = std::move(input);
    return run_deflate(history, data, level_, last ? Z_FINISH : Z_SYNC_FLUSH);
}

Result<std::vector<std::uint8_t>> deflate_buffer(std::span<const std::uint8_t> input, int level)
{
    return run_deflate({}, input, level, Z_FINISH);
}

Result<std::vector<std::uint8_t>> inflate_buffer(std::span<const std::uint8_t> input, std::size_t expected_size)
{
    zng_stream s{};
    if (zng_inflateInit2(&s, -15) != Z_OK)
        return fail(Status::Internal, "inflateInit2 failed");
    std::vector<std::uint8_t> out(expected_size + 1);
    s.next_in = input.data();
    s.avail_in = static_cast<uint32_t>(input.size());
    s.next_out = out.data();
    s.avail_out = static_cast<uint32_t>(out.size());
    const int rc = zng_inflate(&s, Z_FINISH);
    const auto produced = out.size() - s.avail_out;
    zng_inflateEnd(&s);
    if (rc != Z_STREAM_END || produced != expected_size)
        return fail(Status::CorruptArchive, "deflate data is corrupt or has an unexpected size");
    out.resize(produced);
    return out;
}

struct InflateStream::State
{
    zng_stream s{};
};

Result<std::unique_ptr<IChunkedStream>> InflateStream::create(std::unique_ptr<IChunkedStream> raw)
{
    std::unique_ptr<InflateStream> st(new InflateStream());
    st->state_ = std::make_unique<State>();
    if (zng_inflateInit2(&st->state_->s, -15) != Z_OK)
    {
        st->state_.reset();
        return fail(Status::Internal, "inflateInit2 failed");
    }
    st->raw_ = std::move(raw);
    st->in_.resize(256u << 10);
    return std::unique_ptr<IChunkedStream>(std::move(st));
}

InflateStream::~InflateStream()
{
    if (state_)
        zng_inflateEnd(&state_->s);
}

Result<std::size_t> InflateStream::read(std::uint8_t* buf, std::size_t len)
{
    if (finished_ || len == 0)
        return 0;
    auto& s = state_->s;
    s.next_out = buf;
    s.avail_out = static_cast<uint32_t>(std::min<std::size_t>(len, 1u << 30));
    const auto want = s.avail_out;
    while (s.avail_out > 0)
    {
        if (s.avail_in == 0 && !raw_eof_)
        {
            auto n = raw_->read(in_.data(), in_.size());
            if (!n)
                return std::unexpected(n.error());
            if (*n == 0)
                raw_eof_ = true;
            s.next_in = in_.data();
            s.avail_in = static_cast<uint32_t>(*n);
        }
        const int rc = zng_inflate(&s, Z_NO_FLUSH);
        if (rc == Z_STREAM_END)
        {
            finished_ = true;
            break;
        }
        if (rc == Z_BUF_ERROR && raw_eof_ && s.avail_in == 0)
            return fail(Status::CorruptArchive, "deflate stream is truncated");
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            return fail(Status::CorruptArchive, std::format("deflate stream is corrupt ({})", rc));
    }
    const auto got = static_cast<std::size_t>(want - s.avail_out);
    produced_ += got;
    return got;
}

}
