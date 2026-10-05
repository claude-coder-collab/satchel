// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/flac/flac_file.hpp"

#include <algorithm>
#include <cstring>

namespace zp::flac
{

AssembledHeader assemble_header(const HeaderPlan& plan)
{
    AssembledHeader out;
    ByteWriter w;
    w.str("fLaC");
    const auto info = plan.info.serialize();
    append_block_header(w, BlockType::StreamInfo, info.size(), false);
    out.streaminfo_offset = w.data().size();
    w.bytes(info);
    const auto comments = plan.comments.serialize();
    append_block_header(w, BlockType::VorbisComment, comments.size(), false);
    w.bytes(comments);
    for (const auto& r : plan.standard)
    {
        append_block_header(w, BlockType::Application, r.bytes.size() + 4, false);
        w.bytes(r.id);
        w.bytes(r.bytes);
    }
    const auto project = plan.project.serialize();
    append_block_header(w, BlockType::Application, project.size(), true);
    out.sha_offset = w.data().size() + plan.project.sha_offset();
    w.bytes(project);
    out.bytes = w.take();
    return out;
}

AppId record_id(PcmContainer c)
{
    switch (c)
    {
        case PcmContainer::Wav:
        case PcmContainer::Rf64:
            return { 'r', 'i', 'f', 'f' };
        case PcmContainer::Aiff:
        case PcmContainer::Aifc:
            return { 'a', 'i', 'f', 'f' };
        case PcmContainer::Wave64:
            return { 'w', '6', '4', ' ' };
        case PcmContainer::Caf:
            return { 'c', 'a', 'f', 'f' };
    }
    return {};
}

Result<std::vector<std::uint8_t>> read_range(IChunkedStream& in, std::uint64_t offset, std::uint64_t length)
{
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(length));
    if (auto r = in.seek(offset); !r)
        return std::unexpected(r.error());
    auto n = in.read_full(buf);
    if (!n)
        return std::unexpected(n.error());
    if (*n != buf.size())
        return fail(Status::SourceChanged, "file is shorter than when it was planned");
    return buf;
}

Result<std::vector<ForeignRecord>> read_records(IChunkedStream& in, const PcmLayout& layout)
{
    std::vector<ForeignRecord> out;
    const auto id = record_id(layout.container);
    for (const auto& b : layout.blocks)
    {
        auto bytes = read_range(in, b.offset, b.length);
        if (!bytes)
            return std::unexpected(bytes.error());
        out.push_back({ id, std::move(*bytes) });
    }
    return out;
}

std::uint64_t encoded_size_bound(const PcmLayout& layout, std::uint64_t frames, std::uint32_t channels_per_stream)
{
    const auto blocks = (frames + block_size - 1) / block_size;
    return frames * channels_per_stream * layout.bytes_per_sample + blocks * (32ull + 2ull * channels_per_stream);
}

SourceHasher::SourceHasher(const PcmLayout& layout, bool per_channel, std::size_t md5_groups) :
    layout_(layout),
    per_channel_(per_channel),
    md5_(per_channel ? layout.channels : 1)
{
    const std::size_t channels = per_channel ? layout.channels : 1;
    const std::size_t groups = std::clamp<std::size_t>(md5_groups, 1, channels);
    for (std::size_t g = 0; g < groups; ++g)
    {
        Group group;
        group.first_channel = channels * g / groups;
        group.channels = channels * (g + 1) / groups - group.first_channel;
        if (per_channel_)
            group.channel_scratch.resize(group.channels);
        groups_.push_back(std::move(group));
    }
}

void SourceHasher::update(std::span<const std::uint8_t> chunk)
{
    update_sha256(chunk);
    for (std::size_t g = 0; g < groups_.size(); ++g)
        update_md5(chunk, g);
}

void SourceHasher::update_sha256(std::span<const std::uint8_t> chunk)
{
    sha_.update(chunk);
}

void SourceHasher::update_md5(std::span<const std::uint8_t> chunk, std::size_t group)
{
    auto& g = groups_.at(group);
    const auto start = g.position;
    const auto end = g.position + chunk.size();
    g.position = end;
    const auto a0 = std::max(start, layout_.audio_offset);
    const auto a1 = std::min(end, layout_.audio_offset + layout_.audio_size);
    if (a0 < a1)
        audio(g, chunk.subspan(static_cast<std::size_t>(a0 - start), static_cast<std::size_t>(a1 - a0)));
}

void SourceHasher::audio(Group& g, std::span<const std::uint8_t> bytes)
{
    const std::size_t unit = per_channel_ ? layout_.frame_bytes() : layout_.bytes_per_sample;
    std::span<const std::uint8_t> data = bytes;
    std::vector<std::uint8_t> joined;
    if (!g.carry.empty())
    {
        joined = g.carry;
        joined.insert(joined.end(), bytes.begin(), bytes.end());
        g.carry.clear();
        data = joined;
    }
    const auto whole = data.size() - data.size() % unit;
    if (whole < data.size())
        g.carry.assign(data.begin() + static_cast<std::ptrdiff_t>(whole), data.end());
    data = data.first(whole);
    if (data.empty())
        return;
    const auto canonical = md5_bytes(layout_, data, g.scratch);
    if (!per_channel_)
    {
        md5_[0].update(canonical);
        return;
    }
    const auto width = layout_.bytes_per_sample;
    const auto frames = canonical.size() / layout_.frame_bytes();
    for (auto& s : g.channel_scratch)
        s.resize(frames * width);
    const auto split = [&]<std::size_t W>() {
        const auto stride = layout_.frame_bytes();
        for (std::size_t k = 0; k < g.channels; ++k)
        {
            const auto* src = canonical.data() + (g.first_channel + k) * W;
            auto* dst = g.channel_scratch[k].data();
            for (std::size_t f = 0; f < frames; ++f, src += stride, dst += W)
                std::memcpy(dst, src, W);
        }
    };
    switch (width)
    {
        case 1:
            split.template operator()<1>();
            break;
        case 2:
            split.template operator()<2>();
            break;
        case 3:
            split.template operator()<3>();
            break;
        default:
            split.template operator()<4>();
            break;
    }
    for (std::size_t k = 0; k < g.channels; ++k)
        md5_[g.first_channel + k].update(g.channel_scratch[k]);
}

Hashes SourceHasher::finish()
{
    Hashes h;
    h.sha256 = sha_.finish();
    for (auto& m : md5_)
        h.md5.push_back(m.finish());
    return h;
}

std::size_t HashJob::md5_groups_for(const PcmLayout& layout, bool per_channel)
{
#ifdef __EMSCRIPTEN__
    (void) layout;
    (void) per_channel;
    return 1;
#else
    if (!per_channel || layout.channels < 8)
        return 1;
    return std::min<std::size_t>(4, layout.channels / 4);
#endif
}

HashJob::HashJob(const PcmLayout& layout, bool per_channel) :
    hasher_(layout, per_channel, md5_groups_for(layout, per_channel)),
    next_(1 + hasher_.md5_groups(), 0)
{
    for (std::size_t lane = 2; lane < next_.size(); ++lane)
        extra_lanes_.emplace_back([this, lane] { run(lane); });
}

HashJob::~HashJob()
{
    close();
    for (auto& t : extra_lanes_)
        t.join();
}

void HashJob::push(std::shared_ptr<const std::vector<std::uint8_t>> chunk)
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return pushed_ - std::ranges::min(next_) < max_queued; });
    queue_.push_back(std::move(chunk));
    ++pushed_;
    cv_.notify_all();
}

void HashJob::close()
{
    std::lock_guard lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

void HashJob::run(std::size_t lane)
{
    while (true)
    {
        std::shared_ptr<const std::vector<std::uint8_t>> chunk;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return closed_ || next_[lane] < pushed_; });
            if (next_[lane] == pushed_)
                break;
            chunk = queue_[static_cast<std::size_t>(next_[lane] - first_)];
        }
        if (lane == 0)
            hasher_.update_sha256(*chunk);
        else
            hasher_.update_md5(*chunk, lane - 1);
        std::lock_guard lock(mutex_);
        ++next_[lane];
        while (first_ < std::ranges::min(next_))
        {
            queue_.pop_front();
            ++first_;
        }
        cv_.notify_all();
    }
    std::lock_guard lock(mutex_);
    if (++lanes_done_ == next_.size())
        result_ = hasher_.finish();
    cv_.notify_all();
}

Hashes HashJob::wait()
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return lanes_done_ == next_.size(); });
    return result_.value_or(Hashes{});
}

}
