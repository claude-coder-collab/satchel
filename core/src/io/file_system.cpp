// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/file_system.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <random>
#include <system_error>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <cerrno>
    #include <cstring>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

namespace zp
{

namespace
{

#ifdef _WIN32
std::string win_error(std::string_view what, const std::filesystem::path& p)
{
    const auto code = static_cast<int>(GetLastError());
    return std::format("{} '{}': {}", what, path_to_utf8(p), std::system_category().message(code));
}

constexpr std::int64_t filetime_epoch_offset_100ns = 116444736000000000LL;

std::int64_t filetime_to_unix_ns(FILETIME ft)
{
    const auto ticks = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | static_cast<std::int64_t>(ft.dwLowDateTime);
    return (ticks - filetime_epoch_offset_100ns) * 100;
}

FILETIME unix_ns_to_filetime(std::int64_t ns)
{
    const auto ticks = static_cast<std::uint64_t>(ns / 100 + filetime_epoch_offset_100ns);
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
}
#else
std::string posix_error(std::string_view what, const std::filesystem::path& p)
{
    return std::format("{} '{}': {}", what, path_to_utf8(p), std::generic_category().message(errno));
}
#endif

}

std::filesystem::path path_from_utf8(std::string_view utf8)
{
    return { std::u8string(utf8.begin(), utf8.end()) };
}

std::string path_to_utf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return { u8.begin(), u8.end() };
}

Result<FileStat> stat_no_follow(const std::filesystem::path& p)
{
    FileStat st;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data))
        return fail(Status::IoError, win_error("cannot stat", p));
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        st.kind = ItemKind::Symlink;
    else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        st.kind = ItemKind::Directory;
    else
        st.kind = ItemKind::File;
    st.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    st.mtime_ns = filetime_to_unix_ns(data.ftLastWriteTime);
    st.unix_mode = st.kind == ItemKind::Directory ? 0755u : 0644u;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY && st.kind == ItemKind::File)
        st.unix_mode = 0444u;
#else
    struct stat s{};
    if (::lstat(p.c_str(), &s) != 0)
        return fail(Status::IoError, posix_error("cannot stat", p));
    if (S_ISLNK(s.st_mode))
        st.kind = ItemKind::Symlink;
    else if (S_ISDIR(s.st_mode))
        st.kind = ItemKind::Directory;
    else if (S_ISREG(s.st_mode))
        st.kind = ItemKind::File;
    else
        st.kind = ItemKind::Other;
    st.size = st.kind == ItemKind::File ? static_cast<std::uint64_t>(s.st_size) : 0;
    #ifdef __APPLE__
    st.mtime_ns = static_cast<std::int64_t>(s.st_mtimespec.tv_sec) * 1'000'000'000 + s.st_mtimespec.tv_nsec;
    #else
    st.mtime_ns = static_cast<std::int64_t>(s.st_mtim.tv_sec) * 1'000'000'000 + s.st_mtim.tv_nsec;
    #endif
    st.unix_mode = static_cast<std::uint32_t>(s.st_mode) & 07777u;
#endif
    return st;
}

VoidResult set_file_mtime(const std::filesystem::path& p, std::int64_t mtime_ns)
{
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail(Status::IoError, win_error("cannot open for time update", p));
    const FILETIME ft = unix_ns_to_filetime(mtime_ns);
    const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    if (!ok)
        return fail(Status::IoError, win_error("cannot set time", p));
#else
    std::array<struct timespec, 2> ts{};
    ts[0].tv_sec = mtime_ns / 1'000'000'000;
    ts[0].tv_nsec = mtime_ns % 1'000'000'000;
    ts[1] = ts[0];
    if (::utimensat(AT_FDCWD, p.c_str(), ts.data(), AT_SYMLINK_NOFOLLOW) != 0)
        return fail(Status::IoError, posix_error("cannot set time", p));
#endif
    return {};
}

VoidResult set_unix_mode(const std::filesystem::path& p, std::uint32_t mode)
{
#ifdef _WIN32
    (void) p;
    (void) mode;
#else
    if (::chmod(p.c_str(), static_cast<mode_t>(mode & 07777u)) != 0)
        return fail(Status::IoError, posix_error("cannot set permissions", p));
#endif
    return {};
}

Result<std::unique_ptr<FileStream>> FileStream::open(const std::filesystem::path& p, FileMode mode)
{
    std::unique_ptr<FileStream> fs(new FileStream());
    fs->path_ = p;
#ifdef _WIN32
    DWORD access = GENERIC_READ;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_DELETE;
    DWORD disposition = OPEN_EXISTING;
    if (mode == FileMode::CreateTruncate)
    {
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        share = FILE_SHARE_DELETE;
    }
    else if (mode == FileMode::CreateNew)
    {
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = CREATE_NEW;
        share = FILE_SHARE_DELETE;
    }
    HANDLE h = CreateFileW(p.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail(Status::IoError, win_error("cannot open", p));
    fs->handle_ = h;
#else
    int flags = O_RDONLY;
    if (mode == FileMode::CreateTruncate)
        flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (mode == FileMode::CreateNew)
        flags = O_RDWR | O_CREAT | O_EXCL;
    const int fd = ::open(p.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0)
        return fail(Status::IoError, posix_error("cannot open", p));
    fs->fd_ = fd;
#endif
    return fs;
}

FileStream::~FileStream()
{
    close();
}

void FileStream::close()
{
#ifdef _WIN32
    if (handle_)
    {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

Result<std::size_t> FileStream::read(std::uint8_t* buf, std::size_t len)
{
    std::size_t total = 0;
    while (total < len)
    {
#ifdef _WIN32
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - total, 1u << 30));
        DWORD got = 0;
        OVERLAPPED ov{};
        const auto at = pos_ + total;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        if (!ReadFile(handle_, buf + total, want, &got, &ov))
        {
            if (GetLastError() == ERROR_HANDLE_EOF)
                break;
            return fail(Status::IoError, win_error("read failed", path_));
        }
#else
        const auto want = std::min<std::size_t>(len - total, 1u << 30);
        const auto got = ::pread(fd_, buf + total, want, static_cast<off_t>(pos_ + total));
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            return fail(Status::IoError, posix_error("read failed", path_));
        }
#endif
        if (got == 0)
            break;
        total += static_cast<std::size_t>(got);
    }
    pos_ += total;
    return total;
}

Result<std::size_t> FileStream::write(const std::uint8_t* buf, std::size_t len)
{
    std::size_t total = 0;
    while (total < len)
    {
#ifdef _WIN32
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - total, 1u << 30));
        DWORD put = 0;
        OVERLAPPED ov{};
        const auto at = pos_ + total;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        if (!WriteFile(handle_, buf + total, want, &put, &ov))
            return fail(Status::IoError, win_error("write failed", path_));
#else
        const auto want = std::min<std::size_t>(len - total, 1u << 30);
        const auto put = ::pwrite(fd_, buf + total, want, static_cast<off_t>(pos_ + total));
        if (put < 0)
        {
            if (errno == EINTR)
                continue;
            return fail(Status::IoError, posix_error("write failed", path_));
        }
#endif
        if (put == 0)
            return fail(Status::IoError, "write made no progress");
        total += static_cast<std::size_t>(put);
    }
    pos_ += total;
    return total;
}

VoidResult FileStream::seek(std::uint64_t pos)
{
    pos_ = pos;
    return {};
}

std::optional<std::uint64_t> FileStream::size() const
{
#ifdef _WIN32
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(handle_, &sz))
        return std::nullopt;
    return static_cast<std::uint64_t>(sz.QuadPart);
#else
    struct stat s{};
    if (::fstat(fd_, &s) != 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(s.st_size);
#endif
}

VoidResult FileStream::flush()
{
    return {};
}

VoidResult FileStream::sync()
{
#ifdef _WIN32
    if (!FlushFileBuffers(handle_))
        return fail(Status::IoError, win_error("sync failed", path_));
#elif defined(__APPLE__)
    if (::fcntl(fd_, F_FULLFSYNC) != 0 && ::fsync(fd_) != 0)
        return fail(Status::IoError, posix_error("sync failed", path_));
#else
    if (::fsync(fd_) != 0)
        return fail(Status::IoError, posix_error("sync failed", path_));
#endif
    return {};
}

VoidResult atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to)
{
#ifdef _WIN32
    if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (ReplaceFileW(to.c_str(), from.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
            return {};
    }
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail(Status::IoError, win_error("cannot replace", to));
#else
    if (::rename(from.c_str(), to.c_str()) != 0)
        return fail(Status::IoError, posix_error("cannot replace", to));
    return {};
#endif
}

VoidResult sync_directory_of(const std::filesystem::path& p)
{
#ifndef _WIN32
    const auto dir = p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0)
    {
        ::fsync(dfd);
        ::close(dfd);
    }
#else
    (void) p;
#endif
    return {};
}

Result<std::unique_ptr<AtomicFileStream>> AtomicFileStream::create(const std::filesystem::path& target, bool sync_on_commit)
{
    static std::atomic<std::uint64_t> counter{ 0 };
    std::random_device rd;
    const auto dir = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto tag = std::format("{:08x}{:04x}", rd(), counter.fetch_add(1) & 0xFFFFu);
        auto temp = dir / path_from_utf8(std::format(".{}.{}.tmp", path_to_utf8(target.filename()), tag));
        auto file = FileStream::open(temp, FileMode::CreateNew);
        if (!file)
            continue;
        std::unique_ptr<AtomicFileStream> s(new AtomicFileStream());
        s->file_ = std::move(*file);
        s->target_ = target;
        s->temp_ = std::move(temp);
        s->sync_ = sync_on_commit;
        return s;
    }
    return fail(Status::IoError, std::format("cannot create a temporary file next to '{}'", path_to_utf8(target)));
}

AtomicFileStream::~AtomicFileStream()
{
    discard();
}

VoidResult AtomicFileStream::commit()
{
    if (done_)
        return fail(Status::InvalidArgument, "stream already committed or discarded");
    if (sync_)
    {
        if (auto r = file_->sync(); !r)
        {
            discard();
            return r;
        }
    }
    file_->close();
    if (auto r = atomic_replace(temp_, target_); !r)
    {
        discard();
        return r;
    }
    done_ = true;
    if (sync_)
        return sync_directory_of(target_);
    return {};
}

void AtomicFileStream::discard()
{
    if (done_)
        return;
    done_ = true;
    if (file_)
        file_->close();
    std::error_code ec;
    std::filesystem::remove(temp_, ec);
}

}
