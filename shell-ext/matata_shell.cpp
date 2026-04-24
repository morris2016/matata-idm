// matata_shell.dll - in-process COM shell extension.
//
// Implements IShellExtInit + IContextMenu so Explorer's right-click menu
// gets a "Download with matata" entry whenever one or more files are
// selected. Invoking it spawns matata-gui.exe with the file paths passed
// as command-line arguments; the GUI's existing CLI-arg handler queues
// each as a file:// URL.
//
// Registration (HKCU so no admin):
//   HKCU\Software\Classes\CLSID\{GUID}
//     (default) = "matata shell extension"
//     \InprocServer32
//       (default)  = "C:\path\to\matata_shell.dll"
//       ThreadingModel = "Apartment"
//   HKCU\Software\Classes\*\shellex\ContextMenuHandlers\matata
//     (default) = "{GUID}"
// (Same entry is written under Directory\shellex for right-click on
// folders, and under Folder\shellex for the background.)
//
// Build: matata_shell.dll is a standard Win32 DLL with an exported .def
// file covering the four required entry points (DllCanUnloadNow,
// DllGetClassObject, DllRegisterServer, DllUnregisterServer).

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <objbase.h>
#include <olectl.h>

#include <new>
#include <string>
#include <vector>

// ---- CLSID ----------------------------------------------------------
// {7E1C8F52-4B1A-4A3D-9C2F-4C5E6D7B8A90}
static const GUID CLSID_MatataShellExt =
  { 0x7e1c8f52, 0x4b1a, 0x4a3d,
    { 0x9c, 0x2f, 0x4c, 0x5e, 0x6d, 0x7b, 0x8a, 0x90 } };

HINSTANCE    g_hDll         = nullptr;
LONG         g_lockCount    = 0;

// ---- helpers --------------------------------------------------------

static std::wstring guiExePath() {
    // Look for matata-gui.exe next to the DLL (typical install layout).
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_hDll, buf, MAX_PATH);
    if (n == 0) return L"";
    std::wstring p(buf, n);
    auto sl = p.find_last_of(L'\\');
    if (sl == std::wstring::npos) return L"";
    p = p.substr(0, sl + 1);
    return p + L"matata-gui.exe";
}

static std::wstring quoteArg(const std::wstring& s) {
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        int slashes = 0;
        while (i < s.size() && s[i] == L'\\') { ++slashes; ++i; }
        if (i == s.size())        { out.append(slashes * 2, L'\\'); break; }
        else if (s[i] == L'"')    { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else                      { out.append(slashes, L'\\'); out.push_back(s[i]); }
    }
    out.push_back(L'"');
    return out;
}

static void launchMatataWithPaths(const std::vector<std::wstring>& paths) {
    std::wstring exe = guiExePath();
    if (exe.empty()) return;
    if (paths.empty()) {
        // No files selected -> just open the GUI.
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    // matata-gui.exe currently queues its command line as a single URL.
    // To keep this session atomic we invoke the GUI once per file. On
    // the first call the window starts; subsequent launches re-use the
    // same process via queued add-download messages if we ever wire it
    // up; for v0.6.1 each spawn is fine because the GUI is lightweight.
    for (auto& p : paths) {
        std::wstring arg = quoteArg(p);
        ShellExecuteW(nullptr, L"open", exe.c_str(),
                      arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

// ---- IShellExtInit + IContextMenu impl ------------------------------

class MatataContextMenu : public IShellExtInit, public IContextMenu {
    LONG                       m_ref = 1;
    std::vector<std::wstring>  m_paths;
public:
    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IShellExtInit) {
            *ppv = static_cast<IShellExtInit*>(this); AddRef(); return S_OK;
        }
        if (iid == IID_IContextMenu) {
            *ppv = static_cast<IContextMenu*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // IShellExtInit
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE, IDataObject* dataObj, HKEY) override {
        m_paths.clear();
        if (!dataObj) return S_OK;  // background invocation; no selection
        FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        if (dataObj->GetData(&fe, &med) != S_OK) return E_INVALIDARG;
        HDROP h = (HDROP)med.hGlobal;
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
        for (UINT k = 0; k < n; ++k) {
            wchar_t path[MAX_PATH * 2];
            if (DragQueryFileW(h, k, path, MAX_PATH * 2))
                m_paths.emplace_back(path);
        }
        ReleaseStgMedium(&med);
        return S_OK;
    }

    // IContextMenu
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU hMenu, UINT indexMenu,
                                               UINT idCmdFirst, UINT idCmdLast,
                                               UINT uFlags) override {
        if (uFlags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        InsertMenuW(hMenu, indexMenu, MF_BYPOSITION | MF_STRING,
                    idCmdFirst + 0, L"Download with matata");
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* ici) override {
        if (!ici) return E_INVALIDARG;
        if (HIWORD(ici->lpVerb) != 0) return E_FAIL;
        UINT id = LOWORD(ici->lpVerb);
        if (id != 0) return E_FAIL;
        launchMatataWithPaths(m_paths);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR idCmd, UINT uType, UINT*, CHAR* pszName, UINT cchMax) override {
        if (idCmd != 0) return E_INVALIDARG;
        const wchar_t* helpW = L"Queue this file / folder in matata";
        const char*    helpA = "Queue this file / folder in matata";
        switch (uType) {
        case GCS_HELPTEXTW:
            StringCchCopyW((PWSTR)pszName, cchMax, helpW);
            return S_OK;
        case GCS_HELPTEXTA:
            StringCchCopyA(pszName, cchMax, helpA);
            return S_OK;
        case GCS_VERBW:
            StringCchCopyW((PWSTR)pszName, cchMax, L"matata.download");
            return S_OK;
        case GCS_VERBA:
            StringCchCopyA(pszName, cchMax, "matata.download");
            return S_OK;
        }
        return S_OK;
    }
};

// ---- IClassFactory --------------------------------------------------

class ClassFactory : public IClassFactory {
    LONG m_ref = 1;
public:
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* m = new (std::nothrow) MatataContextMenu();
        if (!m) return E_OUTOFMEMORY;
        HRESULT hr = m->QueryInterface(iid, ppv);
        m->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_lockCount);
        else       InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};

// ---- DLL exports ----------------------------------------------------

BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hDll = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return g_lockCount == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID iid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_MatataShellExt) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(iid, ppv);
    f->Release();
    return hr;
}

// ---- self-registration --------------------------------------------

static bool writeReg(HKEY root, const wchar_t* subkey,
                     const wchar_t* valueName, const wchar_t* data) {
    HKEY h;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(h, valueName, 0, REG_SZ,
                            (const BYTE*)data,
                            (DWORD)((lstrlenW(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static std::wstring clsidToStr(REFGUID g) {
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t dllPath[MAX_PATH];
    if (!GetModuleFileNameW(g_hDll, dllPath, MAX_PATH)) return SELFREG_E_CLASS;
    std::wstring clsid = clsidToStr(CLSID_MatataShellExt);

    std::wstring rootKey = L"Software\\Classes\\CLSID\\" + clsid;
    std::wstring inproc  = rootKey + L"\\InprocServer32";

    if (!writeReg(HKEY_CURRENT_USER, rootKey.c_str(), nullptr,
                  L"matata shell extension")) return SELFREG_E_CLASS;
    if (!writeReg(HKEY_CURRENT_USER, inproc.c_str(), nullptr, dllPath))
        return SELFREG_E_CLASS;
    if (!writeReg(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel",
                  L"Apartment")) return SELFREG_E_CLASS;

    // Register the handler on all-files, directory, and background contexts.
    const wchar_t* roots[] = {
        L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\matata",
        L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\matata",
        L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\matata",
    };
    for (auto* k : roots)
        if (!writeReg(HKEY_CURRENT_USER, k, nullptr, clsid.c_str()))
            return SELFREG_E_CLASS;

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    std::wstring clsid = clsidToStr(CLSID_MatataShellExt);
    std::wstring rootKey = L"Software\\Classes\\CLSID\\" + clsid;
    RegDeleteTreeW(HKEY_CURRENT_USER, rootKey.c_str());

    const wchar_t* roots[] = {
        L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\matata",
        L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\matata",
        L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\matata",
    };
    for (auto* k : roots)
        RegDeleteKeyW(HKEY_CURRENT_USER, k);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
