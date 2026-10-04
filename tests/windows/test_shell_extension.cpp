// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/preview.hpp"
#include "pcm_fixtures.hpp"
#include "shell_extension.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <commctrl.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <filesystem>
#include <fstream>

using namespace satchel_shell;

namespace
{

struct ComInit
{
    ComInit() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    ComInit(ComInit&&) = delete;
    ComInit& operator=(ComInit&&) = delete;
    ~ComInit() { CoUninitialize(); }
};

struct Window
{
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr, nullptr, nullptr);
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;
    ~Window() { DestroyWindow(hwnd); }
};

int preview_rows(const std::vector<std::uint8_t>& bytes, std::wstring* summary = nullptr)
{
    IPreviewHandler* handler = nullptr;
    REQUIRE(SUCCEEDED(create_object(clsid_preview, IID_PPV_ARGS(&handler))));
    IInitializeWithStream* init = nullptr;
    REQUIRE(SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&init))));
    IStream* stream = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    REQUIRE(stream);
    REQUIRE(SUCCEEDED(init->Initialize(stream, STGM_READ)));
    stream->Release();
    init->Release();
    Window host;
    RECT rc{ 0, 0, 800, 600 };
    REQUIRE(SUCCEEDED(handler->SetWindow(host.hwnd, &rc)));
    REQUIRE(SUCCEEDED(handler->DoPreview()));
    HWND list = FindWindowExW(host.hwnd, nullptr, WC_LISTVIEWW, nullptr);
    REQUIRE(list);
    const int rows = ListView_GetItemCount(list);
    if (summary)
    {
        HWND label = FindWindowExW(host.hwnd, nullptr, L"STATIC", nullptr);
        REQUIRE(label);
        wchar_t text[512]{};
        GetWindowTextW(label, text, 512);
        *summary = text;
    }
    REQUIRE(SUCCEEDED(handler->Unload()));
    CHECK_FALSE(IsWindow(list));
    handler->Release();
    return rows;
}

}

TEST_CASE("the preview handler lists a zip archive", "[shell]")
{
    ComInit com;
    zp::test::WavSpec w;
    w.frames = 4000;
    zp::MemoryInputSource in;
    in.add_file("docs/readme.txt", zp::test::text_like(5000, 1));
    in.add_file("take.wav", zp::test::make_wav(w));
    const auto built = zp::test::build(in);
    zp::MemoryStream copy(built.zip);
    const auto expected = zp::make_preview(copy, "x.zip").value().entries.size();
    std::wstring summary;
    CHECK(preview_rows(built.zip, &summary) == static_cast<int>(expected));
    CHECK(summary.find(L"audio file restorable") != std::wstring::npos);
    CHECK(module_references() == 0);
}

TEST_CASE("the preview handler describes a FLAC file", "[shell]")
{
    ComInit com;
    zp::test::WavSpec w;
    w.frames = 4000;
    zp::MemoryInputSource in;
    in.add_file("take.wav", zp::test::make_wav(w));
    const auto raw = zp::test::extract_all(zp::test::build(in).zip, { .restore_wav = false });
    CHECK(preview_rows(raw.files.at("take.flac").data) >= 3);
}

TEST_CASE("the preview handler refuses other files", "[shell]")
{
    ComInit com;
    IPreviewHandler* handler = nullptr;
    REQUIRE(SUCCEEDED(create_object(clsid_preview, IID_PPV_ARGS(&handler))));
    IInitializeWithStream* init = nullptr;
    REQUIRE(SUCCEEDED(handler->QueryInterface(IID_PPV_ARGS(&init))));
    const char text[] = "not an archive";
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(text), sizeof text);
    REQUIRE(SUCCEEDED(init->Initialize(stream, STGM_READ)));
    stream->Release();
    init->Release();
    Window host;
    RECT rc{ 0, 0, 100, 100 };
    REQUIRE(SUCCEEDED(handler->SetWindow(host.hwnd, &rc)));
    CHECK(FAILED(handler->DoPreview()));
    handler->Release();
}

TEST_CASE("Explorer commands show for the right selections", "[shell]")
{
    ComInit com;
    zp::test::TempDir dir;
    const auto zip = dir.write("a.zip", zp::test::text_bytes("x"));
    const auto txt = dir.write("b.txt", zp::test::text_bytes("y"));
    const auto state_for = [](const CLSID& clsid, const std::filesystem::path& file) {
        IShellItem* item = nullptr;
        REQUIRE(SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item))));
        IShellItemArray* items = nullptr;
        REQUIRE(SUCCEEDED(SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items))));
        item->Release();
        CHECK(item_paths(items) == std::vector<std::wstring>{ file.wstring() });
        IExplorerCommand* command = nullptr;
        REQUIRE(SUCCEEDED(create_object(clsid, IID_PPV_ARGS(&command))));
        EXPCMDSTATE state = ECS_DISABLED;
        REQUIRE(SUCCEEDED(command->GetState(items, FALSE, &state)));
        LPWSTR title = nullptr;
        REQUIRE(SUCCEEDED(command->GetTitle(items, &title)));
        const std::wstring t(title);
        CoTaskMemFree(title);
        command->Release();
        items->Release();
        return std::pair{ state, t };
    };
    CHECK(state_for(clsid_compress, txt) == std::pair{ ECS_ENABLED, std::wstring(L"Compress with Satchel") });
    CHECK(state_for(clsid_extract, zip).first == ECS_ENABLED);
    CHECK(state_for(clsid_extract, txt).first == ECS_HIDDEN);
    CHECK(gui_executable().ends_with(L"satchel-gui.exe"));
    IUnknown* none = nullptr;
    CHECK(create_object(IID_IUnknown, IID_PPV_ARGS(&none)) == CLASS_E_CLASSNOTAVAILABLE);
}
