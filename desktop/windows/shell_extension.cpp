// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "shell_extension.hpp"

#include "command_line.hpp"
#include "io/preview.hpp"

#include <commctrl.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <new>
#include <optional>

namespace satchel_shell
{

std::atomic<long>& module_references()
{
    static std::atomic<long> count{ 0 };
    return count;
}

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::wstring gui_executable()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&gui_executable), &module);
    std::wstring path(MAX_PATH, L'\0');
    while (true)
    {
        const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size())
        {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    const auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1)) + L"satchel-gui.exe";
}

std::vector<std::wstring> item_paths(IShellItemArray* items)
{
    std::vector<std::wstring> paths;
    DWORD count = 0;
    if (!items || FAILED(items->GetCount(&count)))
        return paths;
    for (DWORD i = 0; i < count; ++i)
    {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item)))
            continue;
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
        {
            paths.emplace_back(name);
            CoTaskMemFree(name);
        }
        item->Release();
    }
    return paths;
}

namespace
{

// zp stream over a COM IStream (seekable; reads the archive on demand).
class ComStream final : public zp::IChunkedStream
{
public:
    explicit ComStream(IStream* stream) :
        stream_(stream)
    {
    }

    zp::Result<std::size_t> read(std::uint8_t* buf, std::size_t len) override
    {
        ULONG got = 0;
        const auto want = static_cast<ULONG>(std::min<std::size_t>(len, 1u << 30));
        const HRESULT hr = stream_->Read(buf, want, &got);
        if (FAILED(hr))
            return zp::fail(zp::Status::IoError, "stream read failed");
        pos_ += got;
        return static_cast<std::size_t>(got);
    }
    [[nodiscard]] bool seekable() const override { return true; }
    zp::VoidResult seek(std::uint64_t pos) override
    {
        LARGE_INTEGER to{};
        to.QuadPart = static_cast<LONGLONG>(pos);
        if (FAILED(stream_->Seek(to, STREAM_SEEK_SET, nullptr)))
            return zp::fail(zp::Status::IoError, "stream seek failed");
        pos_ = pos;
        return {};
    }
    [[nodiscard]] std::uint64_t tell() const override { return pos_; }
    [[nodiscard]] std::optional<std::uint64_t> size() const override
    {
        STATSTG st{};
        if (FAILED(stream_->Stat(&st, STATFLAG_NONAME)))
            return std::nullopt;
        return st.cbSize.QuadPart;
    }
    zp::VoidResult reopen() override { return seek(0); }

private:
    IStream* stream_;
    std::uint64_t pos_ = 0;
};

template <typename Self>
class ComObject
{
public:
    ComObject() { ++module_references(); }
    ComObject(const ComObject&) = delete;
    ComObject& operator=(const ComObject&) = delete;
    ComObject(ComObject&&) = delete;
    ComObject& operator=(ComObject&&) = delete;
    virtual ~ComObject() { --module_references(); }

protected:
    ULONG add_ref() { return ++refs_; }
    ULONG release()
    {
        const ULONG n = --refs_;
        if (n == 0)
            delete static_cast<Self*>(this);
        return n;
    }

private:
    std::atomic<ULONG> refs_{ 1 };
};

class PreviewHandler final : public ComObject<PreviewHandler>, public IPreviewHandler, public IInitializeWithStream, public IObjectWithSite, public IOleWindow
{
public:
    PreviewHandler() = default;
    PreviewHandler(const PreviewHandler&) = delete;
    PreviewHandler& operator=(const PreviewHandler&) = delete;
    PreviewHandler(PreviewHandler&&) = delete;
    PreviewHandler& operator=(PreviewHandler&&) = delete;
    ~PreviewHandler() override
    {
        Unload();
        if (site_)
            site_->Release();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandler)
            *out = static_cast<IPreviewHandler*>(this);
        else if (riid == IID_IInitializeWithStream)
            *out = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IObjectWithSite)
            *out = static_cast<IObjectWithSite*>(this);
        else if (riid == IID_IOleWindow)
            *out = static_cast<IOleWindow*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG)
    AddRef() override { return add_ref(); }
    IFACEMETHODIMP_(ULONG)
    Release() override { return release(); }

    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override
    {
        if (!stream)
            return E_INVALIDARG;
        if (stream_)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = stream;
        stream_->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP SetWindow(HWND parent, const RECT* rect) override
    {
        if (!parent || !rect)
            return E_INVALIDARG;
        parent_ = parent;
        rect_ = *rect;
        if (list_)
        {
            SetParent(summary_, parent_);
            SetParent(list_, parent_);
            layout();
        }
        return S_OK;
    }

    IFACEMETHODIMP SetRect(const RECT* rect) override
    {
        if (!rect)
            return E_INVALIDARG;
        rect_ = *rect;
        layout();
        return S_OK;
    }

    IFACEMETHODIMP DoPreview() override
    {
        if (!stream_ || !parent_)
            return E_FAIL;
        if (list_)
            return S_OK;
        ComStream in(stream_);
        auto preview = zp::make_preview(in, {});
        if (!preview)
            return E_FAIL;
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);
        summary_ = CreateWindowExW(0, L"STATIC", widen(zp::preview_summary_text(*preview)).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, parent_, nullptr, nullptr, nullptr);
        list_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS, 0, 0, 0, 0, parent_, nullptr, nullptr, nullptr);
        if (!summary_ || !list_)
            return E_FAIL;
        SendMessageW(summary_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        if (preview->kind == zp::Preview::Kind::Flac && preview->flac)
            fill_flac(*preview->flac);
        else
            fill_zip(*preview);
        layout();
        return S_OK;
    }

    IFACEMETHODIMP Unload() override
    {
        if (list_)
            DestroyWindow(list_);
        if (summary_)
            DestroyWindow(summary_);
        list_ = nullptr;
        summary_ = nullptr;
        if (stream_)
        {
            stream_->Release();
            stream_ = nullptr;
        }
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override
    {
        if (list_)
            ::SetFocus(list_);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* focus) override
    {
        if (!focus)
            return E_INVALIDARG;
        *focus = GetFocus();
        return S_OK;
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override
    {
        IPreviewHandlerFrame* frame = nullptr;
        if (site_ && SUCCEEDED(site_->QueryInterface(IID_PPV_ARGS(&frame))))
        {
            const HRESULT hr = frame->TranslateAccelerator(msg);
            frame->Release();
            return hr;
        }
        return S_FALSE;
    }

    IFACEMETHODIMP SetSite(IUnknown* site) override
    {
        if (site_)
            site_->Release();
        site_ = site;
        if (site_)
            site_->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        return site_ ? site_->QueryInterface(riid, out) : E_FAIL;
    }

    IFACEMETHODIMP GetWindow(HWND* window) override
    {
        if (!window)
            return E_INVALIDARG;
        *window = parent_;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

private:
    void add_column(int index, const wchar_t* title, int width, bool right = false)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = right ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx = width;
        col.pszText = const_cast<wchar_t*>(title);
        ListView_InsertColumn(list_, index, &col);
    }

    void add_row(const std::vector<std::wstring>& cells)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(list_);
        item.pszText = const_cast<wchar_t*>(cells[0].c_str());
        const int row = ListView_InsertItem(list_, &item);
        for (std::size_t c = 1; c < cells.size(); ++c)
            ListView_SetItemText(list_, row, static_cast<int>(c), const_cast<wchar_t*>(cells[c].c_str()));
    }

    void fill_zip(const zp::Preview& p)
    {
        add_column(0, L"Name", 320);
        add_column(1, L"Size", 90, true);
        add_column(2, L"Packed", 90, true);
        add_column(3, L"Method", 80);
        add_column(4, L"Restores to", 160);
        for (const auto& e : p.entries)
        {
            if (e.directory)
                add_row({ widen(e.name), L"", L"", L"", L"" });
            else
                add_row({ widen(e.name), widen(zp::preview_size_text(e.size)), widen(zp::preview_size_text(e.packed)), widen(e.method), widen(e.restores_to) });
        }
    }

    void fill_flac(const zp::Preview::Flac& f)
    {
        add_column(0, L"Property", 160);
        add_column(1, L"Value", 420);
        for (const auto& [k, v] : zp::preview_flac_rows(f))
            add_row({ widen(k), widen(v) });
    }

    void layout()
    {
        if (!list_)
            return;
        constexpr int summary_height = 24;
        const int width = rect_.right - rect_.left;
        const int height = rect_.bottom - rect_.top;
        MoveWindow(summary_, rect_.left + 8, rect_.top + 4, std::max(0, width - 16), summary_height - 4, TRUE);
        MoveWindow(list_, rect_.left, rect_.top + summary_height, width, std::max(0, height - summary_height), TRUE);
    }

    IStream* stream_ = nullptr;
    IUnknown* site_ = nullptr;
    HWND parent_ = nullptr;
    HWND summary_ = nullptr;
    HWND list_ = nullptr;
    RECT rect_{};
};

class Command final : public ComObject<Command>, public IExplorerCommand
{
public:
    explicit Command(bool extract) :
        extract_(extract)
    {
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (riid != IID_IUnknown && riid != IID_IExplorerCommand)
            return E_NOINTERFACE;
        *out = static_cast<IExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG)
    AddRef() override { return add_ref(); }
    IFACEMETHODIMP_(ULONG)
    Release() override { return release(); }

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override { return SHStrDupW(extract_ ? L"Extract with Satchel" : L"Compress with Satchel", name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override { return SHStrDupW((gui_executable() + L",0").c_str(), icon); }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override
    {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override
    {
        *guid = extract_ ? clsid_extract : clsid_compress;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override
    {
        *state = !extract_ || any_zip(item_paths(items)) ? ECS_ENABLED : ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override
    {
        const auto paths = item_paths(items);
        if (paths.empty())
            return S_OK;
        auto line = command_line(gui_executable(), extract_ ? L"--extract" : L"--compress", paths);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return S_OK;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override
    {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override
    {
        *commands = nullptr;
        return E_NOTIMPL;
    }

private:
    bool extract_;
};

}

HRESULT create_object(REFCLSID clsid, REFIID riid, void** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    IUnknown* object = nullptr;
    if (clsid == clsid_preview)
        object = static_cast<IPreviewHandler*>(new (std::nothrow) PreviewHandler());
    else if (clsid == clsid_compress)
        object = static_cast<IExplorerCommand*>(new (std::nothrow) Command(false));
    else if (clsid == clsid_extract)
        object = static_cast<IExplorerCommand*>(new (std::nothrow) Command(true));
    else
        return CLASS_E_CLASSNOTAVAILABLE;
    if (!object)
        return E_OUTOFMEMORY;
    const HRESULT hr = object->QueryInterface(riid, out);
    object->Release();
    return hr;
}

}
