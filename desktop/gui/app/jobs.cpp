// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "jobs.hpp"

#include "labels.hpp"

#include <condition_variable>

namespace
{

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

std::string utf8(const QString& s)
{
    return s.toStdString();
}

}

zp_context_t* app_context()
{
    static zpp::Context ctx(zpp::not_null(zp_context_create(0, 0)));
    return ctx.get();
}

JobQueue::JobQueue(QObject* parent) :
    QObject(parent)
{
    qRegisterMetaType<JobOutcome>();
    thread_ = QThread::create([this] { loop(); });
    thread_->start();
}

JobQueue::~JobQueue()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        cancel_running_ = true;
    }
    cv_.notify_all();
    thread_->wait();
    delete thread_;
}

int JobQueue::enqueue(QString title, std::function<JobOutcome(JobContext&)> run)
{
    std::lock_guard lock(mutex_);
    const int id = next_id_++;
    queue_.push_back({ id, std::move(title), std::move(run) });
    cv_.notify_all();
    return id;
}

void JobQueue::cancel(int id)
{
    std::lock_guard lock(mutex_);
    if (running_ == id)
        cancel_running_ = true;
    std::erase_if(queue_, [id](const Job& j) { return j.id == id; });
}

bool JobQueue::busy() const
{
    std::lock_guard lock(mutex_);
    return running_ != 0 || !queue_.empty();
}

void JobQueue::loop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            job = std::move(queue_.front());
            queue_.pop_front();
            running_ = job.id;
            cancel_running_ = false;
        }
        emit started(job.id, job.title);
        JobContext jc;
        jc.cancelled = &cancel_running_;
        jc.progress = [this, id = job.id](std::uint64_t done, std::uint64_t total) {
            emit progress(id, done, total);
            return !cancel_running_.load();
        };
        JobOutcome outcome;
        try
        {
            outcome = job.run(jc);
        } catch (const zpp::Error& e)
        {
            outcome.status = e.status();
            outcome.message = QString::fromUtf8(e.what());
        } catch (const std::exception& e)
        {
            outcome.status = ZP_INTERNAL;
            outcome.message = QString::fromUtf8(e.what());
        }
        {
            std::lock_guard lock(mutex_);
            running_ = 0;
        }
        emit finished(job.id, job.title, outcome);
    }
}

JobOutcome compress_job(JobContext& jc, zp_plan_t* plan, const QString& output, const satchel_gui::Settings& settings)
{
    JobOutcome out;
    out.output = output;
    zpp::Stream stream(zpp::not_null(zp_stream_create_file(utf8(output).c_str())));
    zp_build_options_t options{};
    std::string temp;
    if (settings.temp_folder)
    {
        temp = settings.temp_folder->string();
        options.temp_dir = temp.c_str();
    }
    zpp::ProgressAdapter adapter{ jc.progress };
    zp_build_result_t* raw = nullptr;
    out.status = zp_build(plan, stream.get(), &options, zpp::ProgressAdapter::call, &adapter, &raw);
    out.message = QString::fromUtf8(zp_last_error());
    zpp::BuildResult result(raw);
    for (const auto& w : zpp::plan_warnings(plan))
        out.details << QString("%1: %2").arg(qs(w.source_path), qs(w.detail));
    for (std::size_t i = 0; result && i < zp_build_result_entry_count(result.get()); ++i)
    {
        zp_entry_result_t r{};
        zpp::check(zp_build_result_get_entry(result.get(), i, &r));
        out.output_bytes += r.compressed_size;
        if (r.status != ZP_OK)
            out.details << QString("%1: %2").arg(QString::fromUtf8(r.name), QString::fromUtf8(r.message));
    }
    out.input_bytes = zp_plan_total_bytes(plan);
    if (out.status == ZP_OK || out.status == ZP_SOURCE_CHANGED)
    {
        zpp::check(zp_stream_commit(stream.get()));
        out.summary = labels::savings(out.input_bytes, out.output_bytes);
        if (!out.details.isEmpty())
            out.summary += QString(" · %1 warning(s)").arg(out.details.size());
    }
    return out;
}

JobOutcome extract_job(JobContext& jc, const QString& archive, const QString& destination, const satchel_gui::Settings& settings, const std::vector<std::size_t>& selection, bool replace_existing)
{
    JobOutcome out;
    out.output = destination;
    zpp::Stream in(zpp::not_null(zp_stream_open_file(utf8(archive).c_str())));
    zpp::Reader reader(zpp::not_null(zp_reader_open(app_context(), in.get())));
    zpp::Sink sink(zpp::not_null(zp_sink_filesystem(utf8(destination).c_str())));
    zp_extract_options_t options{};
    zp_extract_options_init(&options);
    options.restore_wav = settings.restore_audio ? 1 : 0;
    options.include_readme = settings.include_readme ? 1 : 0;
    options.overwrite = replace_existing                        ? ZP_OVERWRITE_REPLACE
        : settings.overwrite == satchel_gui::Overwrite::Replace ? ZP_OVERWRITE_REPLACE
                                                                : ZP_OVERWRITE_SKIP;
    zpp::ExtractionPlan plan(zpp::not_null(zp_extract_plan(reader.get(), selection.empty() ? nullptr : selection.data(), selection.size(), sink.get(), &options)));
    zp_extract_issue_t issue{};
    for (std::size_t i = 0; i < zp_xplan_issue_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_issue(plan.get(), i, &issue));
        if (issue.kind != ZP_ISSUE_EXISTS_AT_DESTINATION)
            out.details << QString("%1: %2").arg(QString::fromUtf8(issue.name), QString::fromUtf8(issue.detail));
    }
    zpp::ProgressAdapter adapter{ jc.progress };
    out.status = zp_extract(plan.get(), zpp::ProgressAdapter::call, &adapter);
    out.message = QString::fromUtf8(zp_last_error());
    std::size_t written = 0;
    zp_extract_outcome_t o{};
    for (std::size_t i = 0; i < zp_xplan_outcome_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_outcome(plan.get(), i, &o));
        if (o.status == ZP_OK)
            ++written;
        else
            out.details << QString("%1: %2").arg(QString::fromUtf8(o.target), QString::fromUtf8(o.message));
    }
    out.summary = QString("%1 file(s) written").arg(written);
    if (!out.details.isEmpty())
        out.summary += QString(" · %1 problem(s)").arg(out.details.size());
    return out;
}

JobOutcome verify_job(JobContext& jc, const QString& archive)
{
    JobOutcome out;
    zpp::Stream in(zpp::not_null(zp_stream_open_file(utf8(archive).c_str())));
    zpp::Reader reader(zpp::not_null(zp_reader_open(app_context(), in.get())));
    zpp::Sink sink(zpp::not_null(zp_sink_null()));
    zp_extract_options_t options{};
    zp_extract_options_init(&options);
    options.include_readme = 1;
    options.overwrite = ZP_OVERWRITE_REPLACE;
    zpp::ExtractionPlan plan(zpp::not_null(zp_extract_plan(reader.get(), nullptr, 0, sink.get(), &options)));
    zpp::ProgressAdapter adapter{ jc.progress };
    out.status = zp_extract(plan.get(), zpp::ProgressAdapter::call, &adapter);
    out.message = QString::fromUtf8(zp_last_error());
    std::size_t ok = 0;
    zp_extract_outcome_t o{};
    for (std::size_t i = 0; i < zp_xplan_outcome_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_outcome(plan.get(), i, &o));
        out.entry_status.append({ static_cast<int>(o.entry), o.status == ZP_OK ? QStringLiteral("OK") : QString::fromUtf8(zp_status_name(o.status)) });
        if (o.status == ZP_OK)
            ++ok;
        else
            out.details << QString("%1: %2 (%3)").arg(QString::fromUtf8(o.target), QString::fromUtf8(zp_status_name(o.status)), QString::fromUtf8(o.message));
    }
    out.summary = QString("%1 entries verified, %2 error(s)").arg(ok).arg(out.details.size());
    return out;
}
