// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/minizip_adapter.hpp"

#include <cstring>
#include <memory>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <string>
#include <utility>
#include <vector>

namespace zp
{

struct MinizipStreamAdapter::Impl
{
    mz_stream base{};
    MinizipStreamAdapter* owner = nullptr;
    IChunkedStream* inner = nullptr;
    std::vector<std::uint8_t> pending;
    bool discard = false;
};

struct AdapterCallbacks
{
    static MinizipStreamAdapter::Impl* impl(void* s) { return static_cast<MinizipStreamAdapter::Impl*>(s); }

    static void record(MinizipStreamAdapter::Impl* i, const Error& e)
    {
        if (!i->owner->error_)
            i->owner->error_ = e;
    }

    static bool flush_pending(MinizipStreamAdapter::Impl* i)
    {
        if (i->pending.empty())
            return true;
        auto r = i->inner->write_all(i->pending);
        i->pending.clear();
        if (!r)
        {
            record(i, r.error());
            return false;
        }
        return true;
    }

    static std::int32_t open(void*, const char*, std::int32_t) { return MZ_OK; }
    static std::int32_t is_open(void*) { return MZ_OK; }

    static std::int32_t read(void* s, void* buf, std::int32_t size)
    {
        auto* i = impl(s);
        if (!flush_pending(i))
            return MZ_WRITE_ERROR;
        auto r = i->inner->read(static_cast<std::uint8_t*>(buf), static_cast<std::size_t>(size));
        if (!r)
        {
            record(i, r.error());
            return MZ_READ_ERROR;
        }
        return static_cast<std::int32_t>(*r);
    }

    static std::int32_t write(void* s, const void* buf, std::int32_t size)
    {
        constexpr std::size_t limit = 1u << 20;
        auto* i = impl(s);
        if (i->discard)
            return size;
        const auto n = static_cast<std::size_t>(size);
        if (i->pending.size() + n > limit && !flush_pending(i))
            return MZ_WRITE_ERROR;
        if (n >= limit)
        {
            auto r = i->inner->write_all({ static_cast<const std::uint8_t*>(buf), n });
            if (!r)
            {
                record(i, r.error());
                return MZ_WRITE_ERROR;
            }
            return size;
        }
        const auto* p = static_cast<const std::uint8_t*>(buf);
        i->pending.insert(i->pending.end(), p, p + n);
        return size;
    }

    static std::int64_t tell(void* s)
    {
        auto* i = impl(s);
        return static_cast<std::int64_t>(i->inner->tell() + i->pending.size());
    }

    static std::int32_t seek(void* s, std::int64_t offset, std::int32_t origin)
    {
        auto* i = impl(s);
        if (i->discard)
            return MZ_OK;
        std::int64_t target = offset;
        if (origin == MZ_SEEK_CUR)
            target = tell(s) + offset;
        else if (origin == MZ_SEEK_END)
        {
            if (!flush_pending(i))
                return MZ_WRITE_ERROR;
            const auto size = i->inner->size();
            if (!size)
                return MZ_SEEK_ERROR;
            target = static_cast<std::int64_t>(*size) + offset;
        }
        if (target < 0)
            return MZ_SEEK_ERROR;
        if (target == tell(s))
            return MZ_OK;
        if (!flush_pending(i))
            return MZ_WRITE_ERROR;
        if (!i->inner->seekable())
        {
            record(i, Error{ Status::IoError, "output stream is not seekable" });
            return MZ_SEEK_ERROR;
        }
        auto r = i->inner->seek(static_cast<std::uint64_t>(target));
        if (!r)
        {
            record(i, r.error());
            return MZ_SEEK_ERROR;
        }
        return MZ_OK;
    }

    static std::int32_t close(void* s) { return flush_pending(impl(s)) ? MZ_OK : MZ_WRITE_ERROR; }
    static std::int32_t error(void* s) { return impl(s)->owner->error_ ? MZ_STREAM_ERROR : MZ_OK; }
    static std::int32_t get_prop(void*, std::int32_t, std::int64_t*) { return MZ_EXIST_ERROR; }
    static std::int32_t set_prop(void*, std::int32_t, std::int64_t) { return MZ_EXIST_ERROR; }
};

namespace
{

mz_stream_vtbl adapter_vtbl = {
    AdapterCallbacks::open,
    AdapterCallbacks::is_open,
    AdapterCallbacks::read,
    AdapterCallbacks::write,
    AdapterCallbacks::tell,
    AdapterCallbacks::seek,
    AdapterCallbacks::close,
    AdapterCallbacks::error,
    nullptr,
    nullptr,
    AdapterCallbacks::get_prop,
    AdapterCallbacks::set_prop,
};

}

MinizipStreamAdapter::MinizipStreamAdapter(IChunkedStream& inner) :
    impl_(std::make_unique<Impl>())
{
    impl_->base.vtbl = &adapter_vtbl;
    impl_->owner = this;
    impl_->inner = &inner;
}

MinizipStreamAdapter::~MinizipStreamAdapter() = default;

void* MinizipStreamAdapter::handle()
{
    return &impl_->base;
}

VoidResult MinizipStreamAdapter::flush()
{
    if (!AdapterCallbacks::flush_pending(impl_.get()))
        return std::unexpected(error_or(Status::IoError, "write failed"));
    if (auto r = impl_->inner->flush(); !r)
        return r;
    return {};
}

void MinizipStreamAdapter::discard_writes()
{
    impl_->pending.clear();
    impl_->discard = true;
}

Error MinizipStreamAdapter::error_or(Status s, std::string message) const
{
    if (error_)
        return *error_;
    return Error{ s, std::move(message) };
}

MinizipHandle::MinizipHandle() :
    handle_(mz_zip_create())
{
}

MinizipHandle::~MinizipHandle()
{
    if (open_)
        mz_zip_close(handle_);
    mz_zip_delete(&handle_);
}

std::int32_t MinizipHandle::close()
{
    if (!open_)
        return MZ_OK;
    open_ = false;
    return mz_zip_close(handle_);
}

}
