// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/parallel_decode.hpp"

#include "codecs/flac/flac_codec.hpp"
#include "common/bytes.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace zp::flac
{

namespace
{

// Sample-rate code and sample-size bits of a frame header: constant within a stream.
std::uint16_t format_bits(std::span<const std::uint8_t> header)
{
    return static_cast<std::uint16_t>(((header[2] & 0x0F) << 8) | (header[3] & 0x0E));
}

}

VoidResult FrameSplitter::feed(std::span<const std::uint8_t> bytes)
{
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    return scan(false);
}

VoidResult FrameSplitter::scan(bool at_end)
{
    constexpr std::size_t header_max = 16;
    if (!checked_first_)
    {
        if (buf_.size() - start_ < header_max && !at_end)
            return {};
        if (buf_.size() == start_)
            return {};
        const auto n = frame_start(std::span(buf_).subspan(start_));
        if (!n || *n != next_number_)
            return fail(Status::CorruptArchive, "FLAC frames do not start where expected");
        checked_first_ = true;
        format_ = format_bits(std::span(buf_).subspan(start_));
        scan_ = start_ + 2;
    }
    while (scan_ < buf_.size())
    {
        const auto it = std::find(buf_.begin() + static_cast<std::ptrdiff_t>(scan_), buf_.end(), std::uint8_t{ 0xFF });
        if (it == buf_.end())
        {
            scan_ = buf_.size();
            break;
        }
        const auto i = static_cast<std::size_t>(it - buf_.begin());
        if (buf_.size() - i < header_max && !at_end)
        {
            scan_ = i;
            break;
        }
        const auto n = frame_start(std::span(buf_).subspan(i));
        if (n && *n == next_number_ + 1 && format_bits(std::span(buf_).subspan(i)) == format_)
        {
            ready_.bytes.insert(ready_.bytes.end(), buf_.begin() + static_cast<std::ptrdiff_t>(start_), it);
            ++ready_.count;
            ++next_number_;
            start_ = i;
            scan_ = i + 2;
            continue;
        }
        scan_ = i + 1;
    }
    if (buf_.size() - start_ > max_frame_bytes)
        return fail(Status::CorruptArchive, "FLAC frame boundary not found");
    if (start_ >= (1u << 20) && start_ * 2 >= buf_.size())
    {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(start_));
        scan_ -= start_;
        start_ = 0;
    }
    return {};
}

VoidResult FrameSplitter::finish()
{
    if (auto r = scan(true); !r)
        return r;
    if (start_ == buf_.size())
        return {};
    ready_.bytes.insert(ready_.bytes.end(), buf_.begin() + static_cast<std::ptrdiff_t>(start_), buf_.end());
    ++ready_.count;
    ++next_number_;
    start_ = buf_.size();
    scan_ = start_;
    return {};
}

FrameSplitter::Frames FrameSplitter::take()
{
    return std::exchange(ready_, {});
}

namespace
{

std::vector<std::uint8_t> stream_header(const StreamInfo& info)
{
    StreamInfo unknown_length = info;
    unknown_length.total_samples = 0;
    unknown_length.md5 = {};
    ByteWriter w;
    w.str("fLaC");
    append_block_header(w, BlockType::StreamInfo, 34, true);
    const auto si = unknown_length.serialize();
    w.bytes(si);
    return w.take();
}

}

Result<std::uint64_t> decode_parallel(IChunkedStream& frames, const StreamInfo& info, const ParallelDecodeOptions& options, const std::function<void(std::span<const std::int32_t>, std::vector<std::uint8_t>&)>& convert, const std::function<VoidResult(std::span<const std::uint8_t>)>& emit)
{
    struct Batch
    {
        std::size_t seq = 0;
        std::vector<std::uint8_t> bytes;
    };
    struct Done
    {
        std::vector<std::uint8_t> bytes;
        std::uint64_t samples = 0;
    };

    const auto header = stream_header(info);
    const std::size_t threads = std::max<std::size_t>(1, options.threads);
    const std::size_t max_in_flight = threads * 2;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Batch> work;
    std::map<std::size_t, Done> done;
    std::size_t in_flight = 0;
    std::optional<std::size_t> total_batches;
    std::optional<Error> error;
    bool stop = false;

    const auto set_error = [&](Error e) {
        std::lock_guard lock(mutex);
        if (!error)
            error = std::move(e);
        stop = true;
        cv.notify_all();
    };

    std::thread reader([&] {
        FrameSplitter splitter(0);
        std::vector<std::uint8_t> buf(options.read_size);
        std::size_t seq = 0;
        const auto push = [&](bool all) -> bool {
            while (splitter.ready() >= options.frames_per_batch || (all && splitter.ready() > 0))
            {
                auto frames_taken = splitter.take();
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop || in_flight < max_in_flight; });
                if (stop)
                    return false;
                ++in_flight;
                work.push_back({ seq++, std::move(frames_taken.bytes) });
                cv.notify_all();
            }
            return true;
        };
        while (true)
        {
            {
                std::lock_guard lock(mutex);
                if (stop)
                    return;
            }
            auto n = frames.read(buf.data(), buf.size());
            if (!n)
                return set_error(n.error());
            if (*n == 0)
                break;
            if (auto r = splitter.feed({ buf.data(), *n }); !r)
                return set_error(r.error());
            if (!push(false))
                return;
        }
        if (auto r = splitter.finish(); !r)
            return set_error(r.error());
        if (!push(true))
            return;
        std::lock_guard lock(mutex);
        total_batches = seq;
        cv.notify_all();
    });

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t)
    {
        workers.emplace_back([&] {
            std::vector<std::int32_t> samples;
            while (true)
            {
                Batch batch;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stop || !work.empty() || total_batches.has_value(); });
                    if (stop || work.empty())
                        return;
                    batch = std::move(work.front());
                    work.pop_front();
                }
                std::vector<std::uint8_t> stream_bytes;
                stream_bytes.reserve(header.size() + batch.bytes.size());
                stream_bytes.insert(stream_bytes.end(), header.begin(), header.end());
                stream_bytes.insert(stream_bytes.end(), batch.bytes.begin(), batch.bytes.end());
                batch.bytes = {};
                MemoryStream in(std::move(stream_bytes));
                auto decoder = Decoder::open(in, false);
                if (!decoder)
                    return set_error(decoder.error());
                samples.clear();
                while (true)
                {
                    auto block = (*decoder)->next();
                    if (!block)
                        return set_error(block.error());
                    if (block->empty())
                        break;
                    samples.insert(samples.end(), block->begin(), block->end());
                }
                Done d;
                d.samples = samples.size() / std::max<std::uint32_t>(1, info.channels);
                convert(samples, d.bytes);
                std::lock_guard lock(mutex);
                done.emplace(batch.seq, std::move(d));
                cv.notify_all();
            }
        });
    }

    std::uint64_t total_samples = 0;
    for (std::size_t next = 0;; ++next)
    {
        Done d;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return stop || done.contains(next) || (total_batches && next >= *total_batches); });
            if (stop)
                break;
            if (total_batches && next >= *total_batches)
                break;
            d = std::move(done.at(next));
            done.erase(next);
            --in_flight;
            cv.notify_all();
        }
        total_samples += d.samples;
        if (auto r = emit(d.bytes); !r)
        {
            set_error(r.error());
            break;
        }
    }
    {
        std::lock_guard lock(mutex);
        stop = true;
        cv.notify_all();
    }
    reader.join();
    for (auto& w : workers)
        w.join();
    if (error)
        return std::unexpected(*error);
    return total_samples;
}

}
