// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <shobjidl.h>
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace satchel_shell
{

// {8C4F2B1C-E643-456B-A814-5A3F0DB05DFB}: preview handler for .zip and .flac.
inline constexpr CLSID clsid_preview = { 0x8c4f2b1c, 0xe643, 0x456b, { 0xa8, 0x14, 0x5a, 0x3f, 0x0d, 0xb0, 0x5d, 0xfb } };
// {71C262A4-D20B-4503-9034-1243F449AFC8}: "Compress with Satchel".
inline constexpr CLSID clsid_compress = { 0x71c262a4, 0xd20b, 0x4503, { 0x90, 0x34, 0x12, 0x43, 0xf4, 0x49, 0xaf, 0xc8 } };
// {5F750275-DF17-4595-BA33-3771E2C5A0D3}: "Extract with Satchel".
inline constexpr CLSID clsid_extract = { 0x5f750275, 0xdf17, 0x4595, { 0xba, 0x33, 0x37, 0x71, 0xe2, 0xc5, 0xa0, 0xd3 } };

// Live COM objects and server locks, for DllCanUnloadNow.
std::atomic<long>& module_references();

// Creates one of the objects above (E_NOINTERFACE / CLASS_E_CLASSNOTAVAILABLE on mismatch).
HRESULT create_object(REFCLSID clsid, REFIID riid, void** out);

// File-system paths of the items of an IShellItemArray (items without one are skipped).
std::vector<std::wstring> item_paths(IShellItemArray* items);

// satchel-gui.exe next to the module that contains this code.
std::wstring gui_executable();

std::wstring widen(const std::string& utf8);

}
