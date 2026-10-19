// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "session_controller.hpp"

#include "labels.hpp"

#include <QFileInfo>
#include <QMetaObject>

using satchel_gui::Session;

namespace
{

std::filesystem::path to_path(const QString& s)
{
    return { s.toStdU16String() };
}

std::shared_ptr<const SessionSnapshot> make_snapshot(const Session& s, int generation)
{
    auto snap = std::make_shared<SessionSnapshot>();
    snap->view = s.view();
    snap->options = s.options();
    snap->target = QString::fromStdU16String(s.target().u16string());
    snap->is_new = s.is_new();
    snap->has_changes = s.has_changes();
    snap->can_undo = s.can_undo();
    snap->can_redo = s.can_redo();
    snap->base_generation = generation;
    return snap;
}

}

SessionController::SessionController(JobQueue& jobs, QObject* parent) :
    QObject(parent),
    jobs_(jobs),
    session_(std::make_shared<std::unique_ptr<Session>>())
{
    thread_ = std::thread([this] { worker(); });
    connect(&jobs_, &JobQueue::finished, this, [this](int id, const QString&, const JobOutcome& outcome) {
        if (id != committing_)
            return;
        committing_ = 0;
        post([](auto&) {}, outcome.status == ZP_OK);
    });
}

SessionController::~SessionController()
{
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void SessionController::worker()
{
    while (true)
    {
        std::pair<Task, bool> item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        QString error;
        try
        {
            item.first(*session_);
        } catch (const zpp::Error& e)
        {
            error = QString::fromUtf8(e.what());
        } catch (const std::exception& e)
        {
            error = QString::fromUtf8(e.what());
        }
        std::shared_ptr<const SessionSnapshot> snap;
        if (*session_)
        {
            int generation = 0;
            {
                std::scoped_lock lock(mutex_);
                if (item.second)
                    ++generation_;
                generation = generation_;
            }
            snap = make_snapshot(**session_, generation);
        }
        QMetaObject::invokeMethod(this, [this, snap, error] { publish(snap, error); }, Qt::QueuedConnection);
    }
}

void SessionController::post(Task task, bool reload_base)
{
    ++pending_;
    if (pending_ == 1 && !committing_)
        emit busy_changed(true);
    {
        std::scoped_lock lock(mutex_);
        queue_.emplace_back(std::move(task), reload_base);
    }
    cv_.notify_one();
}

void SessionController::publish(std::shared_ptr<const SessionSnapshot> snapshot, const QString& error)
{
    --pending_;
    if (!active_)
        snapshot_.reset();
    else if (snapshot)
        snapshot_ = std::move(snapshot);
    if (!error.isEmpty())
        emit failed(error);
    emit updated();
    if (!busy())
        emit busy_changed(false);
}

void SessionController::open(const QString& archive, satchel_gui::SessionOptions options)
{
    active_ = true;
    snapshot_.reset();
    auto* ctx = app_context();
    post([ctx, path = to_path(archive), options = std::move(options)](auto& s) {
        s.reset();
        s = std::make_unique<Session>(Session::open(ctx, path, options));
    },
        true);
}

void SessionController::create(const QString& target, satchel_gui::SessionOptions options, satchel_gui::Change initial)
{
    active_ = true;
    snapshot_.reset();
    auto* ctx = app_context();
    post([ctx, path = to_path(target), options = std::move(options), initial = std::move(initial)](auto& s) mutable {
        s.reset();
        s = std::make_unique<Session>(Session::create(ctx, path, options));
        if (!initial.empty())
            s->apply(std::move(initial));
    },
        true);
}

void SessionController::close()
{
    active_ = false;
    snapshot_.reset();
    post([](auto& s) { s.reset(); });
}

void SessionController::apply(satchel_gui::Change change)
{
    if (!active_ || committing_)
        return;
    post([change = std::move(change)](auto& s) mutable {
        if (s)
            s->apply(std::move(change));
    });
}

void SessionController::undo()
{
    if (active_ && !committing_)
        post([](auto& s) {
            if (s)
                s->undo();
        });
}

void SessionController::redo()
{
    if (active_ && !committing_)
        post([](auto& s) {
            if (s)
                s->redo();
        });
}

void SessionController::discard()
{
    if (active_ && !committing_)
        post([](auto& s) {
            if (s)
                s->discard();
        });
}

void SessionController::set_options(satchel_gui::SessionOptions options)
{
    if (active_ && !committing_)
        post([options = std::move(options)](auto& s) mutable {
            if (s)
                s->set_options(std::move(options));
        });
}

void SessionController::set_target(const QString& target)
{
    if (active_ && !committing_)
        post([path = to_path(target)](auto& s) {
            if (s)
                s->set_target(path);
        });
}

int SessionController::commit(const QString& output, const QString& title, const satchel_gui::Settings& settings)
{
    if (!active_ || busy() || !snapshot_ || !snapshot_->view.executable)
        return 0;
    const bool is_new = snapshot_->is_new;
    const auto input_bytes = snapshot_->view.summary.input_bytes;
    committing_ = jobs_.enqueue(title, [session = session_, output, settings, is_new, input_bytes](JobContext& jc) {
        JobOutcome out;
        out.output = output;
        zp_build_options_t options{};
        std::string temp;
        if (settings.temp_folder)
        {
            temp = settings.temp_folder->string();
            options.temp_dir = temp.c_str();
        }
        try
        {
            (*session)->commit(to_path(output), &options, jc.progress);
        } catch (const zpp::Error& e)
        {
            out.status = e.status();
            out.message = QString::fromUtf8(e.what());
            return out;
        }
        out.output_bytes = static_cast<std::uint64_t>(QFileInfo(output).size());
        out.input_bytes = input_bytes;
        out.summary = is_new ? labels::savings(input_bytes, out.output_bytes) : QObject::tr("Archive updated (%1)").arg(QString::fromStdString(satchel_gui::human_size(out.output_bytes)));
        if (settings.verify_after_build)
        {
            auto v = verify_job(jc, output);
            out.summary += QString(" · %1").arg(v.summary);
            out.details << v.details;
        }
        return out;
    });
    emit busy_changed(true);
    return committing_;
}
