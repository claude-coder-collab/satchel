// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/deflate.hpp"
#include "codecs/flac/flac_codec.hpp"
#include "codecs/pcm/tags.hpp"
#include "common/product.hpp"
#include "io/zip/build_job.hpp"
#include "io/zip/readme.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace zp::build
{

namespace
{

constexpr std::size_t read_chunk = 1u << 20;

bool deflate_pays_off(std::size_t compressed, std::size_t original, double min_saving)
{
    return original > 0 && static_cast<double>(compressed) <= static_cast<double>(original) * (1.0 - min_saving);
}

enum class EntryMode : std::uint8_t {
    Fixed, // method chosen by the reader
    Auto, // single segment; the worker keeps deflate only if it pays off
};

class Reader
{
public:
    explicit Reader(Job& job) :
        job_(job)
    {
    }

    void run();

private:
    void fatal(std::size_t entry, Error e)
    {
        Segment s;
        s.kind = SegmentKind::Fatal;
        s.entry = entry;
        s.error = std::move(e);
        job_.post(seq_++, std::move(s));
        failed_ = true;
    }

    void skip(std::size_t entry, Error e)
    {
        Segment s;
        s.kind = SegmentKind::SkipEntry;
        s.entry = entry;
        e.status = Status::SourceChanged;
        s.error = std::move(e);
        job_.post(seq_++, std::move(s));
        skipped_.insert(entry);
    }

    void general(std::size_t i, IChunkedStream& stream, std::uint64_t size, bool force_store);
    void generated(std::size_t i);
    void flac(std::size_t i, IChunkedStream& stream);

    Job& job_;
    std::size_t seq_ = 0;
    bool failed_ = false;
    std::set<std::size_t> skipped_;
};

void Reader::general(std::size_t i, IChunkedStream& stream, std::uint64_t size, bool force_store)
{
    const auto& entry = job_.plan.entries[i];
    const Error change{ Status::SourceChanged, std::format("'{}' changed while it was being read", entry.item.source_path) };

    EntryMode mode = EntryMode::Fixed;
    const SegmentEncoder* encoder = job_.store.get();
    std::vector<std::uint8_t> prefix;
    std::uint64_t seg_size = encoder->segment_size();
    if (!force_store && size > 0 && size <= job_.options.small_entry_threshold)
    {
        mode = EntryMode::Auto;
        encoder = job_.deflate.get();
        seg_size = size;
    }
    else if (!force_store && size > 0)
    {
        prefix.resize(static_cast<std::size_t>(std::min<std::uint64_t>(job_.options.sample_window, size)));
        auto got = stream.read_full(prefix);
        if (!got || *got != prefix.size())
            return fatal(i, got ? change : got.error());
        auto sample = deflate_buffer(prefix, job_.plan.options.deflate_level);
        if (!sample)
            return fatal(i, sample.error());
        if (deflate_pays_off(sample->size(), prefix.size(), job_.options.min_deflate_saving))
            encoder = job_.deflate.get();
        seg_size = encoder->segment_size();
    }

    std::uint64_t remaining = size;
    std::vector<std::uint8_t> history;
    bool first = true;
    do
    {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(seg_size, remaining));
        const std::uint64_t cost = encoder->method() == ZipMethod::Store ? n : 2ull * n + 1024;
        if (!job_.budget.acquire(cost))
        {
            failed_ = true;
            return;
        }
        std::vector<std::uint8_t> buf(n);
        const auto from_prefix = std::min(prefix.size(), n);
        std::copy_n(prefix.begin(), from_prefix, buf.begin());
        prefix.erase(prefix.begin(), prefix.begin() + static_cast<std::ptrdiff_t>(from_prefix));
        auto got = stream.read_full(std::span(buf).subspan(from_prefix));
        const bool last = remaining == n;
        if (!got || *got != n - from_prefix)
        {
            job_.budget.release(cost);
            return fatal(i, got ? change : got.error());
        }
        if (last)
        {
            std::uint8_t probe = 0;
            auto extra = stream.read(&probe, 1);
            if (!extra || *extra != 0)
            {
                job_.budget.release(cost);
                return fatal(i, extra ? change : extra.error());
            }
        }

        std::vector<std::uint8_t> hist;
        if (encoder->history_size() > 0 && !history.empty())
            hist = history;
        if (encoder->history_size() > 0 && !last)
        {
            const auto keep = std::min<std::size_t>(encoder->history_size(), buf.size());
            history.assign(buf.end() - static_cast<std::ptrdiff_t>(keep), buf.end());
        }

        const auto my_seq = seq_++;
        auto& job = job_;
        job.tasks.add();
        job.context.workers().submit([&job, encoder, mode, my_seq, i, first, last, cost, data = std::move(buf), hist = std::move(hist)]() mutable {
            Segment s;
            s.entry = i;
            s.first = first;
            s.last = last;
            s.method = encoder->method();
            s.input_size = data.size();
            s.budget = cost;
            s.crc = crc32_update(0, data);
            if (mode == EntryMode::Auto)
            {
                auto out = deflate_buffer(data, job.plan.options.deflate_level);
                if (!out)
                {
                    s.kind = SegmentKind::Fatal;
                    s.error = out.error();
                }
                else if (deflate_pays_off(out->size(), data.size(), job.options.min_deflate_saving))
                    s.output = std::move(*out);
                else
                {
                    s.method = ZipMethod::Store;
                    s.output = std::move(data);
                }
            }
            else if (auto out = encoder->encode(hist, std::move(data), last))
                s.output = std::move(*out);
            else
            {
                s.kind = SegmentKind::Fatal;
                s.error = out.error();
            }
            job.post(my_seq, std::move(s));
            job.tasks.done();
        });
        remaining -= n;
        first = false;
    } while (remaining > 0);
}

void Reader::generated(std::size_t i)
{
    const auto text = render_readme(job_.plan, skipped_, job_.options.readme_template);
    auto data = std::make_shared<const std::vector<std::uint8_t>>(text.begin(), text.end());
    SharedBufferStream stream(data);
    general(i, stream, data->size(), false);
}

std::string base_name(std::string_view path)
{
    const auto p = path.rfind('/');
    return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}

void Reader::flac(std::size_t i, IChunkedStream& stream)
{
    const auto& entry = job_.plan.entries[i];
    const auto& layout = *entry.pcm;
    const bool mono = entry.codec == PlanCodec::FlacMono;
    const std::uint32_t streams = mono ? layout.channels : 1;
    const Error change{ Status::SourceChanged, std::format("'{}' changed while it was being read", entry.item.source_path) };

    auto state = std::make_shared<FlacState>();
    state->entries.push_back(i);
    if (mono)
    {
        for (std::size_t j = i + 1; j < job_.plan.entries.size() && state->entries.size() < streams; ++j)
        {
            const auto& m = job_.plan.entries[j];
            if (m.codec == PlanCodec::FlacMono && m.group_id == entry.group_id)
                state->entries.push_back(j);
        }
        if (state->entries.size() != streams)
            return fatal(i, Error{ Status::Internal, "multi-mono group is incomplete in the plan" });
    }
    state->patch_hashes = mono || job_.output_seekable;

    auto records = flac::read_records(stream, layout);
    if (!records)
        return fatal(i, records.error());
    std::vector<std::uint8_t> trailing;
    if (layout.trailing_size > 0)
    {
        auto t = flac::read_range(stream, layout.trailing_offset, layout.trailing_size);
        if (!t)
            return fatal(i, t.error());
        trailing = std::move(*t);
    }

    if (!state->patch_hashes)
    {
        flac::SourceHasher hasher(layout, false);
        if (auto r = stream.seek(0); !r)
            return fatal(i, r.error());
        std::vector<std::uint8_t> buf(read_chunk);
        std::uint64_t total = 0;
        while (true)
        {
            auto n = stream.read(buf.data(), buf.size());
            if (!n)
                return fatal(i, n.error());
            if (*n == 0)
                break;
            total += *n;
            hasher.update({ buf.data(), *n });
        }
        if (total != entry.snapshot.size)
            return fatal(i, change);
        state->hashes = hasher.finish();
    }

    const bool standard = !mono && layout.standard_application_id().has_value();
    std::vector<std::uint8_t> private_data;
    if (!standard)
    {
        auto packed = flac::pack_private(*records);
        if (!packed)
            return fatal(i, packed.error());
        private_data = std::move(*packed);
    }
    const auto original = base_name(entry.restored_name());
    for (std::uint32_t k = 0; k < streams; ++k)
    {
        flac::HeaderPlan hp;
        hp.info.sample_rate = layout.sample_rate;
        hp.info.channels = mono ? 1 : layout.channels;
        hp.info.bits_per_sample = layout.flac_bits();
        hp.info.total_samples = layout.frames;
        if (state->hashes)
            hp.info.md5 = state->hashes->md5[0];
        hp.comments.vendor = flac::vendor_string();
        hp.comments.fields = mirror_tags(*records, layout, mono ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(k + 1)) : std::nullopt);
        hp.comments.fields.emplace_back("ENCODER", std::format("{} {}", product::app_name, product::version()));
        if (standard)
            hp.standard = *records;
        hp.project.layout = mono ? flac::Layout::MultiMonoMember : standard ? flac::Layout::Standard
                                                                            : flac::Layout::Private;
        if (mono)
            hp.project.group_id = entry.group_id.value_or(Uuid{});
        hp.project.channel_index = static_cast<std::uint16_t>(k + 1);
        hp.project.channel_count = layout.channels;
        hp.project.original_name = original;
        if (k == 0)
        {
            hp.project.private_data = private_data;
            hp.project.trailing = trailing;
        }
        if (state->hashes)
            hp.project.sha256 = state->hashes->sha256;
        auto assembled = flac::assemble_header(hp);
        state->headers.push_back(std::move(hp));
        state->assembled.push_back(std::move(assembled));
    }
    state->min_frame.assign(streams, 0);
    state->max_frame.assign(streams, 0);
    state->frames_bytes.assign(streams, 0);

    {
        Segment s;
        s.kind = mono ? SegmentKind::MonoStart : SegmentKind::FlacHeader;
        s.entry = i;
        s.flac = state;
        job_.post(seq_++, std::move(s));
    }

    if (state->patch_hashes)
    {
        auto& job = job_;
        state->hasher = std::make_shared<flac::HashJob>(layout, mono);
        for (const std::size_t lane : { std::size_t{ 0 }, std::size_t{ 1 } })
        {
            job.tasks.add();
            job.context.services().submit([hasher = state->hasher, &job, lane] {
                hasher->run(lane);
                job.tasks.done();
            });
        }
    }
    struct CloseHasher
    {
        std::shared_ptr<flac::HashJob> h;
        explicit CloseHasher(std::shared_ptr<flac::HashJob> hasher) :
            h(std::move(hasher))
        {
        }
        CloseHasher(const CloseHasher&) = delete;
        CloseHasher& operator=(const CloseHasher&) = delete;
        CloseHasher(CloseHasher&&) = delete;
        CloseHasher& operator=(CloseHasher&&) = delete;
        ~CloseHasher()
        {
            if (h)
                h->close();
        }
    } close_hasher(state->hasher);

    flac::EncoderSettings settings;
    settings.channels = mono ? 1 : layout.channels;
    settings.bits_per_sample = layout.flac_bits();
    settings.sample_rate = layout.sample_rate;
    settings.level = job_.plan.options.flac_level;

    const std::uint32_t seg_blocks = flac::segment_blocks(layout.channels);
    const std::uint64_t seg_frames = static_cast<std::uint64_t>(flac::block_size) * seg_blocks;
    const std::uint64_t seg_bytes = seg_frames * layout.frame_bytes();
    const std::uint64_t frame_cost = layout.frame_bytes() + 4ull * layout.channels;
    std::vector<std::uint8_t> seg;
    std::uint64_t seg_index = 0;
    std::uint64_t seg_budget = 0;
    std::size_t seg_target = 0;

    const auto dispatch = [&]() {
        const auto frames = seg.size() / layout.frame_bytes();
        auto& job = job_;
        auto raw = std::make_shared<const std::vector<std::uint8_t>>(std::move(seg));
        seg.clear();
        const auto first_frame = seg_index * seg_blocks;
        for (std::uint32_t k = 0; k < streams; ++k)
        {
            const auto my_seq = seq_++;
            const std::uint64_t cost = k + 1 == streams ? seg_budget : 0;
            job.tasks.add();
            job.context.workers().submit([&job, &layout, raw, frames, k, mono, settings, first_frame, my_seq, cost, i, state] {
                Segment s;
                s.kind = mono ? SegmentKind::MonoFrames : SegmentKind::FlacFrames;
                s.entry = i;
                s.flac = state;
                s.channel = static_cast<std::uint16_t>(k);
                s.budget = cost;
                s.input_size = mono ? raw->size() / layout.channels : raw->size();
                std::vector<std::int32_t> samples(frames * settings.channels);
                if (mono)
                    decode_channel(layout, *raw, static_cast<std::uint16_t>(k), samples);
                else
                    decode_samples(layout, *raw, samples);
                auto enc = flac::encode_segment(settings, samples, first_frame);
                if (!enc)
                {
                    s.kind = SegmentKind::Fatal;
                    s.error = enc.error();
                }
                else
                {
                    s.output = std::move(enc->bytes);
                    s.crc = crc32_update(0, s.output);
                    s.min_frame = enc->min_frame_size;
                    s.max_frame = enc->max_frame_size;
                }
                job.post(my_seq, std::move(s));
                job.tasks.done();
            });
        }
        ++seg_index;
    };

    if (auto r = stream.seek(0); !r)
        return fatal(i, r.error());
    const auto audio_begin = layout.audio_offset;
    const auto audio_end = layout.audio_offset + layout.audio_size;
    std::uint64_t pos = 0;
    while (pos < entry.snapshot.size)
    {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(read_chunk, entry.snapshot.size - pos));
        auto chunk = std::make_shared<std::vector<std::uint8_t>>(n);
        auto got = stream.read_full(*chunk);
        if (!got || *got != n)
            return fatal(i, got ? change : got.error());
        const auto a0 = std::max(pos, audio_begin);
        const auto a1 = std::min(pos + n, audio_end);
        if (a0 < a1)
        {
            auto audio = std::span<const std::uint8_t>(*chunk).subspan(static_cast<std::size_t>(a0 - pos), static_cast<std::size_t>(a1 - a0));
            while (!audio.empty())
            {
                if (seg.empty())
                {
                    const auto left = audio_end - (a1 - audio.size());
                    const auto this_seg = std::min<std::uint64_t>(seg_bytes, left);
                    seg_budget = 2 * this_seg + (this_seg / layout.frame_bytes()) * frame_cost;
                    if (!job_.budget.acquire(seg_budget))
                    {
                        failed_ = true;
                        return;
                    }
                    seg_target = static_cast<std::size_t>(this_seg);
                    seg.reserve(seg_target);
                }
                const auto take = std::min(audio.size(), seg_target - seg.size());
                seg.insert(seg.end(), audio.begin(), audio.begin() + static_cast<std::ptrdiff_t>(take));
                audio = audio.subspan(take);
                if (seg.size() == seg_target)
                    dispatch();
            }
        }
        if (state->hasher)
            state->hasher->push(std::move(chunk));
        pos += n;
    }
    std::uint8_t probe = 0;
    auto extra = stream.read(&probe, 1);
    if (!extra || *extra != 0)
        return fatal(i, extra ? change : extra.error());
    if (!seg.empty())
        dispatch();

    if (state->hasher)
        state->hasher->close();
    for (std::uint32_t k = 0; k < streams; ++k)
    {
        Segment s;
        s.kind = mono ? SegmentKind::MonoMember : SegmentKind::FlacEnd;
        s.entry = state->entries[k];
        s.channel = static_cast<std::uint16_t>(k);
        s.flac = state;
        job_.post(seq_++, std::move(s));
    }
}

void Reader::run()
{
    for (std::size_t i = 0; i < job_.plan.entries.size() && !job_.cancelled.load() && !failed_; ++i)
    {
        const auto& entry = job_.plan.entries[i];
        if (entry.is_directory() || entry.codec == PlanCodec::Kept)
        {
            Segment s;
            s.kind = entry.is_directory() ? SegmentKind::Directory : SegmentKind::Kept;
            s.entry = i;
            job_.post(seq_++, std::move(s));
            continue;
        }
        if (entry.codec == PlanCodec::Generated)
        {
            generated(i);
            continue;
        }
        if (entry.codec == PlanCodec::FlacMono && entry.channel_index.value_or(1) != 1)
        {
            if (skipped_.contains(i - 1))
                skipped_.insert(i);
            continue;
        }

        auto now = entry.item.current();
        if (!now || *now != entry.snapshot)
        {
            skip(i, now ? Error{ Status::SourceChanged, std::format("'{}' changed after planning", entry.item.source_path) } : now.error());
            continue;
        }
        auto stream = entry.item.open();
        if (!stream)
        {
            fatal(i, stream.error());
            break;
        }
        if (entry.is_flac())
            flac(i, **stream);
        else
            general(i, **stream, entry.snapshot.size, entry.flac_fallback_reason.has_value());
    }
    job_.finish_reading(seq_);
}

}

std::uint64_t deflate_size_hint(std::uint64_t size)
{
    return size + size / 256 + (1u << 20);
}

Job::Job(const ArchivePlan& job_plan, Context& job_context, const BuilderOptions& job_options, bool seekable) :
    plan(job_plan),
    context(job_context),
    options(job_options),
    output_seekable(seekable),
    budget(job_context.memory_budget()),
    store(CodecRegistry::make_encoder(ZipMethod::Store, 0)),
    deflate(CodecRegistry::make_encoder(ZipMethod::Deflate, job_plan.options.deflate_level))
{
}

void Job::post(std::size_t seq, Segment s)
{
    std::lock_guard lock(mutex);
    ready.emplace(seq, std::move(s));
    cv.notify_all();
}

void Job::finish_reading(std::size_t total)
{
    std::lock_guard lock(mutex);
    total_segments = total;
    cv.notify_all();
}

void Job::abort()
{
    cancelled.store(true);
    budget.cancel();
    std::lock_guard lock(mutex);
    cv.notify_all();
}

SpillFile::~SpillFile()
{
    if (stream)
        stream->close();
    if (!path.empty())
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

void read_entries(Job& job)
{
    Reader(job).run();
}

}
