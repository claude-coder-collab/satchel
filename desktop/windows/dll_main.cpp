// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// In-process COM server: the preview handler and Explorer commands of shell_extension.cpp.
#include "shell_extension.hpp"

#include <new>

namespace
{

class ClassFactory final : public IClassFactory
{
public:
    explicit ClassFactory(const CLSID& clsid) :
        clsid_(clsid)
    {
        ++satchel_shell::module_references();
    }
    ClassFactory(const ClassFactory&) = delete;
    ClassFactory& operator=(const ClassFactory&) = delete;
    ClassFactory(ClassFactory&&) = delete;
    ClassFactory& operator=(ClassFactory&&) = delete;
    ~ClassFactory() { --satchel_shell::module_references(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (riid != IID_IUnknown && riid != IID_IClassFactory)
            return E_NOINTERFACE;
        *out = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG)
    AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG)
    Release() override
    {
        const ULONG n = --refs_;
        if (n == 0)
            delete this;
        return n;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override
    {
        if (outer)
            return CLASS_E_NOAGGREGATION;
        return satchel_shell::create_object(clsid_, riid, out);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock)
            ++satchel_shell::module_references();
        else
            --satchel_shell::module_references();
        return S_OK;
    }

private:
    CLSID clsid_;
    std::atomic<ULONG> refs_{ 1 };
};

}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (clsid != satchel_shell::clsid_preview && clsid != satchel_shell::clsid_compress && clsid != satchel_shell::clsid_extract)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory(clsid);
    if (!factory)
        return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, out);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return satchel_shell::module_references() == 0 ? S_OK : S_FALSE;
}
