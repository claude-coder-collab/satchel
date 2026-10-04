// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "cli.hpp"

#include <cstdio>
#include <format>
#include <iostream>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
    #define ZP_ISATTY _isatty
    #define ZP_FILENO _fileno
#else
    #include <unistd.h>
    #define ZP_ISATTY isatty
    #define ZP_FILENO fileno
#endif

namespace satchel_cli
{

std::atomic<bool> interrupted{ false };

namespace
{

bool is_terminal(FILE* f)
{
#ifdef _WIN32
    // _isatty also reports NUL and other character devices; only a console counts.
    auto* handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
    DWORD mode = 0;
    return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
#else
    return ZP_ISATTY(ZP_FILENO(f)) != 0;
#endif
}

}

bool stdin_is_tty()
{
    return is_terminal(stdin);
}

bool stderr_is_tty()
{
    return is_terminal(stderr);
}

bool confirm(const std::string& question)
{
    if (!stdin_is_tty())
        return false;
    std::cerr << question << " [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer))
        return false;
    return answer == "y" || answer == "Y" || answer == "yes";
}

ProgressBar::ProgressBar(std::string label, const GlobalOptions& options) :
    label_(std::move(label)),
    enabled_(!options.json && !options.quiet && stderr_is_tty()),
    start_(std::chrono::steady_clock::now()),
    last_(start_)
{
    adapter_.fn = [this](std::uint64_t done, std::uint64_t total) { return update(done, total); };
}

ProgressBar::~ProgressBar()
{
    if (drawn_)
        std::cerr << "\r\033[K" << std::flush;
}

bool ProgressBar::update(std::uint64_t done, std::uint64_t total)
{
    if (interrupted.load())
        return false;
    if (!enabled_)
        return true;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ < std::chrono::milliseconds(100) && done < total)
        return true;
    last_ = now;
    const double seconds = std::chrono::duration<double>(now - start_).count();
    const double fraction = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    const double rate = seconds > 0 ? static_cast<double>(done) / seconds : 0.0;
    constexpr int width = 30;
    const auto filled = static_cast<int>(fraction * width);
    std::string bar(static_cast<std::size_t>(filled), '=');
    bar.resize(width, ' ');
    std::string eta;
    if (rate > 0 && done < total)
    {
        const auto left = static_cast<long long>(static_cast<double>(total - done) / rate);
        eta = std::format("  ETA {}:{:02}", left / 60, left % 60);
    }
    std::cerr << std::format("\r{} [{}] {:3.0f}%  {}/s{}\033[K", label_, bar, fraction * 100.0, human_size(static_cast<std::uint64_t>(rate)), eta) << std::flush;
    drawn_ = true;
    return true;
}

zpp::Context make_context(const GlobalOptions& options)
{
    return zpp::Context(zpp::not_null(zp_context_create(options.threads, options.memory_mb * 1024 * 1024)));
}

std::string human_size(std::uint64_t bytes)
{
    constexpr std::array<const char*, 5> units{ "B", "KB", "MB", "GB", "TB" };
    auto v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1000.0 && u + 1 < units.size())
    {
        v /= 1000.0;
        ++u;
    }
    return u == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", v, units[u]);
}

std::string method_name(int method)
{
    switch (method)
    {
        case ZP_METHOD_STORE:
            return "store";
        case ZP_METHOD_DEFLATE:
            return "deflate";
        default:
            return std::format("method {}", method);
    }
}

std::string_view codec_name(int codec)
{
    switch (codec)
    {
        case ZP_CODEC_FLAC:
            return "flac";
        case ZP_CODEC_FLAC_MONO:
            return "flac-mono";
        case ZP_CODEC_GENERATED:
            return "generated";
        case ZP_CODEC_KEPT:
            return "kept";
        default:
            return "general";
    }
}

void print_json(const nlohmann::json& j)
{
    std::cout << j.dump(2) << '\n';
}

void print_error(const std::string& message)
{
    std::cerr << "satchel: " << message << '\n';
}

}
