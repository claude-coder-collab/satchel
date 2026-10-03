// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/builder.hpp"

#include "io/zip/build_job.hpp"
#include "io/zip/reader.hpp"
#include "io/zip/zip_writer.hpp"

#include <algorithm>
#include <format>
#include <random>

namespace zp
{

namespace
{

using build::FlacState;
using build::Job;
using build::Segment;
using build::SegmentKind;

void merge_frame_sizes(FlacState& st, std::size_t k, std::uint32_t min_frame, std::uint32_t max_frame)
{
    if (max_frame == 0)
        return;
    st.min_frame[k] = st.min_frame[k] == 0 ? min_frame : std::min(st.min_frame[k], min_frame);
    st.max_frame[k] = std::max(st.max_frame[k], max_frame);
}

// Final metadata for stream k once hashes and frame sizes are known. Same length as the
// placeholder version.
std::vector<std::uint8_t> final_header(FlacState& st, std::size_t k)
{
    auto& hp = st.headers[k];
    hp.info.min_frame_size = st.min_frame[k];
    hp.info.max_frame_size = st.max_frame[k];
    if (st.hashes)
    {
        hp.info.md5 = st.hashes->md5[st.hashes->md5.size() == 1 ? 0 : k];
        hp.project.sha256 = st.hashes->sha256;
    }
    return flac::assemble_header(hp).bytes;
}

Result<std::filesystem::path> spill_dir(const BuilderOptions& options)
{
    if (!options.temp_dir.empty())
        return options.temp_dir;
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec)
        return fail(Status::IoError, std::format("no temporary directory: {}", ec.message()));
    return dir;
}

Result<build::SpillFile> create_spill(const std::filesystem::path& dir)
{
    static std::atomic<std::uint64_t> counter{ 0 };
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        auto path = dir / std::format("satchel-spill-{:08x}-{}.flac", rd(), counter.fetch_add(1));
        auto f = FileStream::open(path, FileMode::CreateNew);
        if (!f)
            continue;
        build::SpillFile s;
        s.path = std::move(path);
        s.stream = std::move(*f);
        return s;
    }
    return fail(Status::IoError, std::format("cannot create a temporary file in '{}'", path_to_utf8(dir)));
}

}

ArchiveBuilder::ArchiveBuilder(IChunkedStream& output, Context& context, BuilderOptions options) :
    output_(output),
    context_(context),
    options_(std::move(options))
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

    Job job(plan, context_, options_, output_.seekable());
    job.tasks.add();
    context_.services().submit([&job] {
        build::read_entries(job);
        job.tasks.done();
    });

    const std::uint64_t total_bytes = plan.total_input_bytes();
    std::uint64_t done_bytes = 0;
    std::optional<Error> failure;
    std::optional<EntryResult> current;
    std::size_t next = 0;
    std::string readme_name;
    std::vector<std::uint8_t> copy_buffer;

    const auto fail_with = [&](Error e) {
        if (!failure)
            failure = std::move(e);
        job.abort();
    };

    const auto begin = [&](const PlanEntry& entry, std::size_t index, ZipMethod method, std::optional<std::uint64_t> hint) -> bool {
        EntryHeader h{ entry.output_name, false, method, plan.options.deflate_level, entry.item.mtime_seconds(), entry.item.unix_mode, hint };
        auto w = (*writer)->begin_entry(h);
        if (!w)
        {
            fail_with(w.error());
            return false;
        }
        result.zip64 |= w->zip64;
        current = EntryResult{ index, entry.output_name, method, 0, 0, 0, Status::Ok, {} };
        return true;
    };

    const auto finish_current = [&](std::uint32_t crc, std::uint64_t size) -> bool {
        if (auto r = (*writer)->end_entry(crc, size, size); !r)
        {
            fail_with(r.error());
            return false;
        }
        current->crc32 = crc;
        current->compressed_size = size;
        current->uncompressed_size = size;
        result.per_entry.push_back(std::move(*current));
        current.reset();
        return true;
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
            case SegmentKind::Fatal:
                fail_with(seg.error);
                break;
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
                    const auto size = entry.codec == PlanCodec::Generated ? seg.input_size : entry.snapshot.size;
                    if (!begin(entry, seg.entry, seg.method, seg.method == ZipMethod::Store ? size : build::deflate_size_hint(size)))
                        break;
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
                if (entry.codec != PlanCodec::Generated)
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
            case SegmentKind::FlacHeader:
            {
                auto& st = *seg.flac;
                const auto& header = st.assembled[0].bytes;
                const auto hint = header.size() + flac::encoded_size_bound(*entry.pcm, entry.pcm->frames, entry.pcm->channels);
                if (!begin(entry, seg.entry, ZipMethod::Store, hint))
                    break;
                if (auto r = (*writer)->write(header); !r)
                    fail_with(r.error());
                break;
            }
            case SegmentKind::FlacFrames:
            {
                auto& st = *seg.flac;
                if (auto r = (*writer)->write(seg.output); !r)
                {
                    fail_with(r.error());
                    break;
                }
                st.frames_crc = crc32_combine(st.frames_crc, seg.crc, seg.output.size());
                st.frames_bytes[0] += seg.output.size();
                merge_frame_sizes(st, 0, seg.min_frame, seg.max_frame);
                done_bytes += seg.input_size;
                break;
            }
            case SegmentKind::FlacEnd:
            {
                auto& st = *seg.flac;
                if (st.hasher)
                    st.hashes = st.hasher->wait();
                std::vector<std::uint8_t> header;
                if (st.patch_hashes)
                {
                    header = final_header(st, 0);
                    if (auto r = (*writer)->patch(0, header); !r)
                    {
                        fail_with(r.error());
                        break;
                    }
                }
                else
                    header = st.assembled[0].bytes;
                const auto crc = crc32_combine(crc32_update(0, header), st.frames_crc, st.frames_bytes[0]);
                finish_current(crc, header.size() + st.frames_bytes[0]);
                done_bytes += entry.pcm->non_audio_bytes();
                break;
            }
            case SegmentKind::MonoStart:
            {
                auto& st = *seg.flac;
                auto dir = spill_dir(options_);
                if (!dir)
                {
                    fail_with(dir.error());
                    break;
                }
                for (std::size_t k = 0; k < st.entries.size() && !failure; ++k)
                {
                    auto spill = create_spill(*dir);
                    if (!spill)
                    {
                        fail_with(spill.error());
                        break;
                    }
                    if (auto r = spill->stream->write_all(st.assembled[k].bytes); !r)
                        fail_with(r.error());
                    st.spills.push_back(std::move(*spill));
                }
                break;
            }
            case SegmentKind::MonoFrames:
            {
                auto& st = *seg.flac;
                if (auto r = st.spills[seg.channel].stream->write_all(seg.output); !r)
                {
                    fail_with(r.error());
                    break;
                }
                st.frames_bytes[seg.channel] += seg.output.size();
                merge_frame_sizes(st, seg.channel, seg.min_frame, seg.max_frame);
                done_bytes += seg.input_size;
                break;
            }
            case SegmentKind::MonoMember:
            {
                auto& st = *seg.flac;
                const auto k = seg.channel;
                if (st.hasher && !st.hashes)
                    st.hashes = st.hasher->wait();
                auto& spill = st.spills[k];
                const auto header = final_header(st, k);
                auto& fs = *spill.stream;
                const auto size = fs.tell();
                if (auto r = fs.seek(0); !r)
                {
                    fail_with(r.error());
                    break;
                }
                if (auto r = fs.write_all(header); !r)
                {
                    fail_with(r.error());
                    break;
                }
                if (!begin(entry, seg.entry, ZipMethod::Store, size))
                    break;
                if (auto r = fs.seek(0); !r)
                {
                    fail_with(r.error());
                    break;
                }
                copy_buffer.resize(1u << 20);
                std::uint32_t crc = 0;
                std::uint64_t copied = 0;
                while (copied < size && !failure)
                {
                    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(copy_buffer.size(), size - copied));
                    auto n = fs.read(copy_buffer.data(), want);
                    if (!n || *n != want)
                    {
                        fail_with(n ? Error{ Status::IoError, "temporary file is shorter than expected" } : n.error());
                        break;
                    }
                    const std::span<const std::uint8_t> chunk(copy_buffer.data(), *n);
                    crc = crc32_update(crc, chunk);
                    if (auto r = (*writer)->write(chunk); !r)
                        fail_with(r.error());
                    copied += *n;
                }
                if (failure)
                    break;
                finish_current(crc, size);
                spill = build::SpillFile{};
                if (k == 0)
                    done_bytes += entry.pcm->non_audio_bytes();
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
