// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/builder.hpp"

#include "io/zip/reader.hpp"
#include "io/zip/zip_writer.hpp"

#include <atomic>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>

namespace zp {

namespace {

enum class SegmentKind : std::uint8_t
{
    Data,
    Directory,
    Kept,
    SkipEntry,
    Fatal,
};

struct Segment
{
    SegmentKind kind = SegmentKind::Data;
    std::size_t entry = 0;
    bool first = true;
    bool last = true;
    ZipMethod method = ZipMethod::Store;
    std::vector<std::uint8_t> output;
    std::uint32_t crc = 0;
    std::uint64_t input_size = 0;
    std::uint64_t budget = 0;
    Error error;
};

class Job
{
public:
    Job(const ArchivePlan& plan, Context& context) :
        plan(plan),
        context(context),
        budget(context.memory_budget()),
        store(CodecRegistry::make_encoder(ZipMethod::Store, 0)),
        deflate(CodecRegistry::make_encoder(ZipMethod::Deflate, plan.options.deflate_level))
    {
    }

    void post(std::size_t seq, Segment s)
    {
        std::lock_guard lock(mutex);
        ready.emplace(seq, std::move(s));
        cv.notify_all();
    }

    void finish_reading(std::size_t total)
    {
        std::lock_guard lock(mutex);
        total_segments = total;
        cv.notify_all();
    }

    void abort()
    {
        cancelled.store(true);
        budget.cancel();
        std::lock_guard lock(mutex);
        cv.notify_all();
    }

    const ArchivePlan& plan;
    Context& context;
    ByteBudget budget;
    std::unique_ptr<SegmentEncoder> store;
    std::unique_ptr<SegmentEncoder> deflate;
    TaskCounter tasks;
    std::atomic<bool> cancelled{ false };

    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::size_t, Segment> ready;
    std::optional<std::size_t> total_segments;
};

std::uint64_t deflate_size_hint(std::uint64_t size)
{
    return size + size / 256 + (1u << 20);
}

void read_entries(Job& job)
{
    std::size_t seq = 0;

    const auto fatal = [&](std::size_t entry, Error e) {
        Segment s;
        s.kind = SegmentKind::Fatal;
        s.entry = entry;
        s.error = std::move(e);
        job.post(seq++, std::move(s));
    };

    for (std::size_t i = 0; i < job.plan.entries.size() && !job.cancelled.load(); ++i)
    {
        const auto& entry = job.plan.entries[i];
        if (entry.is_directory() || entry.codec == PlanCodec::Kept)
        {
            Segment s;
            s.kind = entry.is_directory() ? SegmentKind::Directory : SegmentKind::Kept;
            s.entry = i;
            job.post(seq++, std::move(s));
            continue;
        }

        auto now = entry.item.current();
        if (!now || *now != entry.snapshot)
        {
            Segment s;
            s.kind = SegmentKind::SkipEntry;
            s.entry = i;
            s.error = now ? Error{ Status::SourceChanged, std::format("'{}' changed after planning", entry.item.source_path) } : now.error();
            if (!now)
                s.error.status = Status::SourceChanged;
            job.post(seq++, std::move(s));
            continue;
        }

        auto stream = entry.item.open();
        if (!stream)
        {
            fatal(i, stream.error());
            break;
        }

        const SegmentEncoder& encoder = *job.store;
        const auto seg_size = encoder.segment_size();
        std::uint64_t remaining = entry.snapshot.size;
        std::vector<std::uint8_t> history;
        bool first = true;
        bool failed = false;
        do
        {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(seg_size, remaining));
            const std::uint64_t cost = encoder.method() == ZipMethod::Store ? n : 2ull * n + 1024;
            if (!job.budget.acquire(cost))
            {
                failed = true;
                break;
            }
            std::vector<std::uint8_t> buf(n);
            auto got = (*stream)->read_full(buf);
            const bool last = remaining == n;
            Error change{ Status::SourceChanged, std::format("'{}' changed while it was being read", entry.item.source_path) };
            if (!got || *got != n)
            {
                job.budget.release(cost);
                fatal(i, got ? change : got.error());
                failed = true;
                break;
            }
            if (last)
            {
                std::uint8_t probe = 0;
                auto extra = (*stream)->read(&probe, 1);
                if (!extra || *extra != 0)
                {
                    job.budget.release(cost);
                    fatal(i, extra ? change : extra.error());
                    failed = true;
                    break;
                }
            }

            std::vector<std::uint8_t> hist;
            if (encoder.history_size() > 0 && !history.empty())
                hist = history;
            if (encoder.history_size() > 0 && !last)
            {
                const auto keep = std::min<std::size_t>(encoder.history_size(), buf.size());
                history.assign(buf.end() - static_cast<std::ptrdiff_t>(keep), buf.end());
            }

            const auto my_seq = seq++;
            job.tasks.add();
            job.context.workers().submit([&job, &encoder, my_seq, i, first, last, cost, data = std::move(buf), hist = std::move(hist)]() mutable {
                Segment s;
                s.entry = i;
                s.first = first;
                s.last = last;
                s.method = encoder.method();
                s.input_size = data.size();
                s.budget = cost;
                s.crc = crc32_update(0, data);
                auto out = encoder.encode(hist, std::move(data), last);
                if (out)
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
        if (failed)
            break;
    }
    job.finish_reading(seq);
}

}

ArchiveBuilder::ArchiveBuilder(IChunkedStream& output, Context& context, BuilderOptions options) :
    output_(output),
    context_(context),
    options_(options)
{
}

BuildResult ArchiveBuilder::execute(const ArchivePlan& plan, ProgressSink& progress)
{
    BuildResult result;
    if (!plan.executable())
    {
        result.status = Status::ConflictsUnresolved;
        result.message = std::format("{} unresolved conflict(s)", plan.conflicts.size());
        return result;
    }
    for (const auto& e : plan.entries)
    {
        if (e.codec == PlanCodec::Kept && !kept_source_)
        {
            result.status = Status::InvalidArgument;
            result.message = "plan copies existing entries but no source archive was given";
            return result;
        }
    }

    std::lock_guard job_lock(context_.job_mutex());
    auto writer = ZipWriter::open(output_);
    if (!writer)
    {
        result.status = writer.error().status;
        result.message = writer.error().message;
        return result;
    }

    Job job(plan, context_);
    job.tasks.add();
    context_.services().submit([&job] {
        read_entries(job);
        job.tasks.done();
    });

    const std::uint64_t total_bytes = plan.total_input_bytes();
    std::uint64_t done_bytes = 0;
    std::optional<Error> failure;
    std::optional<EntryResult> current;
    std::size_t next = 0;
    std::string readme_name;

    const auto fail_with = [&](Error e) {
        if (!failure)
            failure = std::move(e);
        job.abort();
    };

    while (!failure)
    {
        Segment seg;
        {
            std::unique_lock lock(job.mutex);
            job.cv.wait(lock, [&] { return job.ready.contains(next) || (job.total_segments && *job.total_segments == next) || job.cancelled.load(); });
            if (!job.ready.contains(next))
            {
                if (job.cancelled.load() && !failure)
                    failure = Error{ Status::Cancelled, "cancelled" };
                break;
            }
            seg = std::move(job.ready.extract(next).mapped());
        }
        ++next;
        job.budget.release(seg.budget);
        const auto& entry = plan.entries[seg.entry];

        switch (seg.kind)
        {
            case SegmentKind::Fatal: fail_with(seg.error); break;
            case SegmentKind::SkipEntry:
                result.per_entry.push_back({ seg.entry, entry.output_name, ZipMethod::Store, 0, 0, 0, seg.error.status, seg.error.message });
                break;
            case SegmentKind::Directory:
            {
                EntryHeader h{ entry.output_name, true, ZipMethod::Store, 0, entry.item.mtime_seconds(), entry.item.unix_mode, 0 };
                auto w = (*writer)->begin_entry(h);
                if (!w)
                {
                    fail_with(w.error());
                    break;
                }
                result.zip64 |= w->zip64;
                if (auto r = (*writer)->end_entry(0, 0, 0); !r)
                {
                    fail_with(r.error());
                    break;
                }
                result.per_entry.push_back({ seg.entry, entry.output_name, ZipMethod::Store, 0, 0, 0, Status::Ok, {} });
                break;
            }
            case SegmentKind::Kept:
            {
                if (!entry.kept_index)
                {
                    fail_with(Error{ Status::Internal, "kept entry without a source index" });
                    break;
                }
                auto r = kept_source_->copy_raw(*entry.kept_index, entry.output_name, entry.item.mtime_seconds(), entry.item.unix_mode, **writer);
                if (!r)
                {
                    fail_with(r.error());
                    break;
                }
                result.zip64 |= r->zip64;
                result.per_entry.push_back({ seg.entry, entry.output_name, static_cast<ZipMethod>(r->method), r->compressed_size, r->uncompressed_size, r->crc32, Status::Ok, {} });
                done_bytes += r->uncompressed_size;
                break;
            }
            case SegmentKind::Data:
            {
                if (seg.first)
                {
                    const auto size = entry.snapshot.size;
                    EntryHeader h{ entry.output_name, false, seg.method, plan.options.deflate_level, entry.item.mtime_seconds(), entry.item.unix_mode,
                        seg.method == ZipMethod::Store ? size : deflate_size_hint(size) };
                    auto w = (*writer)->begin_entry(h);
                    if (!w)
                    {
                        fail_with(w.error());
                        break;
                    }
                    result.zip64 |= w->zip64;
                    current = EntryResult{ seg.entry, entry.output_name, seg.method, 0, 0, 0, Status::Ok, {} };
                    if (entry.codec == PlanCodec::Generated)
                        readme_name = entry.output_name;
                }
                if (!current)
                {
                    fail_with(Error{ Status::Internal, "segment without an open entry" });
                    break;
                }
                auto& cur = *current;
                if (auto r = (*writer)->write(seg.output); !r)
                {
                    fail_with(r.error());
                    break;
                }
                cur.crc32 = crc32_combine(cur.crc32, seg.crc, seg.input_size);
                cur.compressed_size += seg.output.size();
                cur.uncompressed_size += seg.input_size;
                done_bytes += seg.input_size;
                if (seg.last)
                {
                    if (auto r = (*writer)->end_entry(cur.crc32, cur.compressed_size, cur.uncompressed_size); !r)
                    {
                        fail_with(r.error());
                        break;
                    }
                    result.per_entry.push_back(std::move(cur));
                    current.reset();
                }
                break;
            }
        }
        if (!failure && !progress.on_progress(done_bytes, total_bytes))
            fail_with(Error{ Status::Cancelled, "cancelled" });
    }

    if (failure)
        job.abort();
    job.tasks.wait();
    result.peak_buffered_bytes = job.budget.peak();

    if (failure)
    {
        (*writer)->abandon();
        result.status = failure->status;
        result.message = failure->message;
        return result;
    }

    auto meta = metadata_.value_or(ArchiveMetadata::current());
    meta.readme_name = readme_name;
    if (auto r = (*writer)->finish(meta.to_comment()); !r)
    {
        result.status = r.error().status;
        result.message = r.error().message;
        return result;
    }
    result.zip64 |= Zip64Policy::needs_zip64_archive(output_.tell(), (*writer)->entry_count());
    for (const auto& e : result.per_entry)
    {
        if (e.status != Status::Ok)
        {
            result.status = e.status;
            result.message = e.message;
            break;
        }
    }
    return result;
}

}
