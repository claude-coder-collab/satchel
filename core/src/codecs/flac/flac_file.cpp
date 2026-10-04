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

SourceHasher::SourceHasher(const PcmLayout& layout, bool per_channel) :
    layout_(layout),
    per_channel_(per_channel),
    md5_(per_channel ? layout.channels : 1)
{
    if (per_channel_)
        channel_scratch_.resize(layout.channels);
}

void SourceHasher::update(std::span<const std::uint8_t> chunk)
{
    update_sha256(chunk);
    update_md5(chunk);
}

void SourceHasher::update_sha256(std::span<const std::uint8_t> chunk)
{
    sha_.update(chunk);
}

void SourceHasher::update_md5(std::span<const std::uint8_t> chunk)
{
    const auto start = position_;
    const auto end = position_ + chunk.size();
    position_ = end;
    const auto a0 = std::max(start, layout_.audio_offset);
    const auto a1 = std::min(end, layout_.audio_offset + layout_.audio_size);
    if (a0 < a1)
        audio(chunk.subspan(static_cast<std::size_t>(a0 - start), static_cast<std::size_t>(a1 - a0)));
}

void SourceHasher::audio(std::span<const std::uint8_t> bytes)
{
    const std::size_t unit = per_channel_ ? layout_.frame_bytes() : layout_.bytes_per_sample;
    std::span<const std::uint8_t> data = bytes;
    std::vector<std::uint8_t> joined;
    if (!carry_.empty())
    {
        joined = carry_;
        joined.insert(joined.end(), bytes.begin(), bytes.end());
        carry_.clear();
        data = joined;
    }
    const auto whole = data.size() - data.size() % unit;
    if (whole < data.size())
        carry_.assign(data.begin() + static_cast<std::ptrdiff_t>(whole), data.end());
    data = data.first(whole);
    if (data.empty())
        return;
    const auto canonical = md5_bytes(layout_, data, scratch_);
    if (!per_channel_)
    {
        md5_[0].update(canonical);
        return;
    }
    const auto width = layout_.bytes_per_sample;
    const auto channels = layout_.channels;
    const auto frames = canonical.size() / layout_.frame_bytes();
    for (auto& s : channel_scratch_)
        s.resize(frames * width);
    for (std::size_t f = 0; f < frames; ++f)
    {
        const auto* src = canonical.data() + f * layout_.frame_bytes();
        for (std::size_t c = 0; c < channels; ++c)
            std::memcpy(channel_scratch_[c].data() + f * width, src + c * width, width);
    }
    for (std::size_t c = 0; c < channels; ++c)
        md5_[c].update(channel_scratch_[c]);
}

Hashes SourceHasher::finish()
{
    Hashes h;
    h.sha256 = sha_.finish();
    for (auto& m : md5_)
        h.md5.push_back(m.finish());
    return h;
}

HashJob::HashJob(const PcmLayout& layout, bool per_channel) :
    hasher_(layout, per_channel)
{
}

void HashJob::push(std::shared_ptr<const std::vector<std::uint8_t>> chunk)
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return pushed_ - std::min(next_[0], next_[1]) < max_queued; });
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

void HashJob::run(Lane lane)
{
    const auto l = static_cast<std::size_t>(lane);
    while (true)
    {
        std::shared_ptr<const std::vector<std::uint8_t>> chunk;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return closed_ || next_[l] < pushed_; });
            if (next_[l] == pushed_)
                break;
            chunk = queue_[static_cast<std::size_t>(next_[l] - first_)];
        }
        if (lane == Lane::Sha256)
            hasher_.update_sha256(*chunk);
        else
            hasher_.update_md5(*chunk);
        std::lock_guard lock(mutex_);
        ++next_[l];
        while (first_ < std::min(next_[0], next_[1]))
        {
            queue_.pop_front();
            ++first_;
        }
        cv_.notify_all();
    }
    std::lock_guard lock(mutex_);
    if (++lanes_done_ == 2)
        result_ = hasher_.finish();
    cv_.notify_all();
}

Hashes HashJob::wait()
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return lanes_done_ == 2; });
    return result_.value_or(Hashes{});
}

}
