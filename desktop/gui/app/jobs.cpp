// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "jobs.hpp"

#include <algorithm>

#include "labels.hpp"

#include <QCoreApplication>

#include <condition_variable>
#include <filesystem>
#include <set>

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

QString counted(const char* text, std::size_t n)
{
    return QCoreApplication::translate("Jobs", text, nullptr, static_cast<int>(n));
}

std::filesystem::path fs_path(const QString& s)
{
    return { s.toStdU16String() };
}

}

zp_context_t* app_context()
{
    static zpp::Context ctx(zpp::not_null(zp_context_create(0, 0)));
    return ctx.get();
}

std::vector<std::string> exclude_patterns(const satchel_gui::Settings& settings)
{
    std::vector<std::string> out;
    const auto add = [&](int preset) {
        const auto list = zpp::exclude_preset(preset);
        out.insert(out.end(), list.begin(), list.end());
    };
    if (settings.exclude_system)
        add(ZP_EXCLUDE_PRESET_SYSTEM);
    if (settings.exclude_daw_caches)
        add(ZP_EXCLUDE_PRESET_DAW_CACHES);
    out.insert(out.end(), settings.exclude_patterns.begin(), settings.exclude_patterns.end());
    return out;
}

QString exclude_pattern_error(const std::vector<std::string>& patterns)
{
    try
    {
        const zpp::Input probe(zpp::not_null(zp_input_memory()));
        zpp::set_excludes(probe, patterns);
    } catch (const zpp::Error& e)
    {
        return QString::fromUtf8(e.what());
    }
    return {};
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
    QString title;
    {
        std::lock_guard lock(mutex_);
        if (running_ == id)
            cancel_running_ = true;
        const auto it = std::ranges::find(queue_, id, &Job::id);
        if (it == queue_.end())
            return;
        title = it->title;
        queue_.erase(it);
    }
    JobOutcome outcome;
    outcome.status = ZP_CANCELLED;
    outcome.message = tr("Cancelled");
    emit finished(id, title, outcome);
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

std::shared_ptr<zpp::Plan> make_plan(const QStringList& inputs, const satchel_gui::Settings& settings)
{
    std::vector<std::string> paths;
    for (const auto& i : inputs)
        paths.push_back(i.toStdString());
    auto input = zpp::input_from_paths(paths);
    zpp::set_excludes(input, exclude_patterns(settings));
    zp_plan_options_t options{};
    zp_plan_options_init(&options);
    options.flac_enabled = settings.flac ? 1 : 0;
    options.deflate_level = settings.deflate_level;
    options.flac_level = settings.flac_level;
    return std::make_shared<zpp::Plan>(zpp::not_null(zp_plan_create(app_context(), input.get(), &options)));
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
            out.summary += counted(QT_TRANSLATE_N_NOOP("Jobs", " · %n warning(s)"), static_cast<std::size_t>(out.details.size()));
    }
    return out;
}

JobOutcome extract_job(JobContext& jc, const QString& archive, const QString& destination, const satchel_gui::Settings& settings, const std::vector<std::size_t>& selection, bool replace_existing, const QString& base_folder)
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
    const auto base = base_folder.toStdString();
    options.base_folder = base.c_str();
    zpp::ExtractionPlan plan(zpp::not_null(zp_extract_plan(reader.get(), selection.empty() ? nullptr : selection.data(), selection.size(), sink.get(), &options)));
    zp_extract_issue_t issue{};
    for (std::size_t i = 0; i < zp_xplan_issue_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_issue(plan.get(), i, &issue));
        if (issue.kind != ZP_ISSUE_EXISTS_AT_DESTINATION)
            out.details << QString("%1: %2").arg(QString::fromUtf8(issue.name), QString::fromUtf8(issue.detail));
    }
    const auto root = fs_path(destination);
    std::vector<std::filesystem::path> new_dirs;
    std::set<std::filesystem::path> existing_files;
    for (std::size_t i = 0; i < zp_xplan_item_count(plan.get()); ++i)
    {
        zp_extract_item_t item{};
        zpp::check(zp_xplan_get_item(plan.get(), i, &item));
        const auto target = root / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(item.target)));
        std::error_code ec;
        if (item.kind != ZP_KIND_DIRECTORY && std::filesystem::exists(target, ec))
            existing_files.insert(target);
        for (auto d = item.kind == ZP_KIND_DIRECTORY ? target : target.parent_path(); d != root && d.has_relative_path(); d = d.parent_path())
        {
            if (!std::filesystem::exists(d, ec))
                new_dirs.push_back(d);
        }
    }
    zpp::ProgressAdapter adapter{ jc.progress };
    out.status = zp_extract(plan.get(), zpp::ProgressAdapter::call, &adapter);
    out.message = QString::fromUtf8(zp_last_error());
    std::size_t written = 0;
    std::vector<std::filesystem::path> files;
    zp_extract_outcome_t o{};
    for (std::size_t i = 0; i < zp_xplan_outcome_count(plan.get()); ++i)
    {
        zpp::check(zp_xplan_get_outcome(plan.get(), i, &o));
        if (o.status == ZP_OK)
        {
            ++written;
            files.push_back(root / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(o.target))));
        }
        else if (out.status != ZP_CANCELLED)
            out.details << QString("%1: %2").arg(QString::fromUtf8(o.target), QString::fromUtf8(o.message));
    }
    if (out.status == ZP_CANCELLED)
    {
        std::error_code ec;
        for (const auto& f : files)
        {
            if (!existing_files.contains(f))
                std::filesystem::remove(f, ec);
        }
        std::ranges::sort(new_dirs, std::greater<>{});
        for (const auto& d : new_dirs)
            std::filesystem::remove(d, ec);
        return out;
    }
    out.summary = counted(QT_TRANSLATE_N_NOOP("Jobs", "%n file(s) extracted"), written);
    if (!out.details.isEmpty())
        out.summary += counted(QT_TRANSLATE_N_NOOP("Jobs", " · %n problem(s)"), static_cast<std::size_t>(out.details.size()));
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
    out.output = archive;
    out.summary = out.details.isEmpty() ? counted(QT_TRANSLATE_N_NOOP("Jobs", "All %n file(s) intact"), ok) : counted(QT_TRANSLATE_N_NOOP("Jobs", "%n file(s) damaged"), static_cast<std::size_t>(out.details.size()));
    return out;
}
