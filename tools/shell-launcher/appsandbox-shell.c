/*
 * appsandbox-shell.dll -- "Run in Sandbox" on Windows 11's short right-click menu.
 *
 * That menu only takes an IExplorerCommand from a package, so the installer registers
 * tools/installer/shell-package/AppxManifest.xml, which points at this DLL. Explorer loads
 * it in a surrogate process. It does nothing itself: each selected file goes to
 * appsandbox-run.exe, the same launcher the classic menu entry runs.
 */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <stdio.h>

/* Must match the Clsid in tools/installer/shell-package/AppxManifest.xml. */
static const GUID CLSID_RunInSandbox =
    { 0x5e1c7a42, 0x8d3b, 0x4f6e, { 0x9a, 0x21, 0xc4, 0x70, 0xb6, 0x2e, 0x13, 0xd8 } };

/* Explorer warns about selections beyond this before running a classic entry. */
#define MAX_FILES  15

static HMODULE g_module;
static volatile LONG g_refs;

typedef struct {
    IExplorerCommand iface;
    volatile LONG refs;
} RunCommand;

static BOOL sibling_path(const wchar_t *name, wchar_t *out, size_t chars)
{
    wchar_t *slash;
    DWORD len = GetModuleFileNameW(g_module, out, (DWORD)chars);
    if (!len || len >= chars) return FALSE;
    slash = wcsrchr(out, L'\\');
    if (!slash) return FALSE;
    return wcscpy_s(slash + 1, chars - (slash + 1 - out), name) == 0;
}

/* The surrogate has the package's identity and a child would inherit it, breakaway policy or
   not. A packaged process can't be relied on to start the elevated app, and removing the package
   must not take a running app with it (that would stop every VM). So Explorer starts the launcher,
   which leaves it exactly as it is from the classic entry. */
static IShellDispatch2 *get_explorer_shell(void)
{
    IShellWindows *windows = NULL;
    IDispatch *desktop = NULL, *background = NULL, *app = NULL;
    IServiceProvider *services = NULL;
    IShellBrowser *browser = NULL;
    IShellView *view = NULL;
    IShellFolderViewDual *folder_view = NULL;
    IShellDispatch2 *shell = NULL;
    VARIANT empty;
    long hwnd = 0;

    VariantInit(&empty);
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER, &IID_IShellWindows, (void **)&windows)) &&
        IShellWindows_FindWindowSW(windows, &empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop) == S_OK &&
        SUCCEEDED(IDispatch_QueryInterface(desktop, &IID_IServiceProvider, (void **)&services)) &&
        SUCCEEDED(IServiceProvider_QueryService(services, &SID_STopLevelBrowser, &IID_IShellBrowser, (void **)&browser)) &&
        SUCCEEDED(IShellBrowser_QueryActiveShellView(browser, &view)) &&
        SUCCEEDED(IShellView_GetItemObject(view, SVGIO_BACKGROUND, &IID_IDispatch, (void **)&background)) &&
        SUCCEEDED(IDispatch_QueryInterface(background, &IID_IShellFolderViewDual, (void **)&folder_view)) &&
        SUCCEEDED(IShellFolderViewDual_get_Application(folder_view, &app))) {
        if (FAILED(IDispatch_QueryInterface(app, &IID_IShellDispatch2, (void **)&shell))) shell = NULL;
    }
    if (app) IDispatch_Release(app);
    if (folder_view) IShellFolderViewDual_Release(folder_view);
    if (background) IDispatch_Release(background);
    if (view) IShellView_Release(view);
    if (browser) IShellBrowser_Release(browser);
    if (services) IServiceProvider_Release(services);
    if (desktop) IDispatch_Release(desktop);
    if (windows) IShellWindows_Release(windows);
    return shell;
}

static BOOL start_launcher(IShellDispatch2 *shell, const wchar_t *exe, const wchar_t *file)
{
    wchar_t quoted[4096 + 3];
    VARIANT args, none, show;
    BSTR exe_bstr;
    BOOL ok = FALSE;

    if (swprintf_s(quoted, _countof(quoted), L"\"%s\"", file) < 0) return FALSE;
    exe_bstr = SysAllocString(exe);
    VariantInit(&args);
    V_VT(&args) = VT_BSTR;
    V_BSTR(&args) = SysAllocString(quoted);
    VariantInit(&none);
    VariantInit(&show);
    V_VT(&show) = VT_I4;
    V_I4(&show) = SW_SHOWNORMAL;
    if (exe_bstr && V_BSTR(&args))
        ok = SUCCEEDED(IShellDispatch2_ShellExecute(shell, exe_bstr, args, none, none, show));
    VariantClear(&args);
    SysFreeString(exe_bstr);
    return ok;
}

static HRESULT STDMETHODCALLTYPE cmd_QueryInterface(IExplorerCommand *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IExplorerCommand)) {
        *out = self;
        IExplorerCommand_AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cmd_AddRef(IExplorerCommand *self)
{
    return (ULONG)InterlockedIncrement(&((RunCommand *)self)->refs);
}

static ULONG STDMETHODCALLTYPE cmd_Release(IExplorerCommand *self)
{
    RunCommand *cmd = (RunCommand *)self;
    LONG refs = InterlockedDecrement(&cmd->refs);
    if (!refs) {
        HeapFree(GetProcessHeap(), 0, cmd);
        InterlockedDecrement(&g_refs);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE cmd_GetTitle(IExplorerCommand *self, IShellItemArray *items, LPWSTR *title)
{
    (void)self; (void)items;
    return SHStrDupW(L"Run in Sandbox", title);
}

static HRESULT STDMETHODCALLTYPE cmd_GetIcon(IExplorerCommand *self, IShellItemArray *items, LPWSTR *icon)
{
    wchar_t path[MAX_PATH + 8];
    (void)self; (void)items;
    if (!sibling_path(L"AppSandbox.exe", path, MAX_PATH)) return E_FAIL;
    wcscat_s(path, _countof(path), L",0");
    return SHStrDupW(path, icon);
}

static HRESULT STDMETHODCALLTYPE cmd_GetToolTip(IExplorerCommand *self, IShellItemArray *items, LPWSTR *tip)
{
    (void)self; (void)items;
    *tip = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cmd_GetCanonicalName(IExplorerCommand *self, GUID *name)
{
    (void)self;
    *name = CLSID_RunInSandbox;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cmd_GetState(IExplorerCommand *self, IShellItemArray *items, BOOL slow, EXPCMDSTATE *state)
{
    (void)self; (void)items; (void)slow;
    *state = ECS_ENABLED;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cmd_Invoke(IExplorerCommand *self, IShellItemArray *items, IBindCtx *ctx)
{
    wchar_t exe[MAX_PATH];
    IShellDispatch2 *shell;
    DWORD count = 0, i;
    (void)self; (void)ctx;

    if (!items) return E_INVALIDARG;
    if (!sibling_path(L"appsandbox-run.exe", exe, _countof(exe))) return E_FAIL;
    if (FAILED(IShellItemArray_GetCount(items, &count))) return E_FAIL;
    shell = get_explorer_shell();
    if (!shell) {
        MessageBoxW(NULL, L"Windows wouldn't start App Sandbox from here. Try Show more options, then Run in Sandbox.",
                    L"Run in Sandbox", MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        return S_OK;
    }
    /* Explorer passes this on to the launcher, and the launcher to the app window. */
    CoAllowSetForegroundWindow((IUnknown *)shell, NULL);
    if (count > MAX_FILES) {
        wchar_t text[128];
        swprintf_s(text, _countof(text), L"Only the first %d of the %lu selected files will be sent.", MAX_FILES, count);
        MessageBoxW(NULL, text, L"Run in Sandbox", MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
        count = MAX_FILES;
    }
    for (i = 0; i < count; i++) {
        IShellItem *item = NULL;
        wchar_t *path = NULL;
        BOOL started = FALSE;
        if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &item))) {
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                started = start_launcher(shell, exe, path);
                CoTaskMemFree(path);
            }
            IShellItem_Release(item);
        }
        if (!started)
            MessageBoxW(NULL, L"This item can't be sent: it isn't a file on a drive or network share.",
                        L"Run in Sandbox", MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
    }
    IShellDispatch2_Release(shell);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cmd_GetFlags(IExplorerCommand *self, EXPCMDFLAGS *flags)
{
    (void)self;
    *flags = ECF_DEFAULT;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cmd_EnumSubCommands(IExplorerCommand *self, IEnumExplorerCommand **subs)
{
    (void)self;
    *subs = NULL;
    return E_NOTIMPL;
}

static IExplorerCommandVtbl g_command_vtbl = {
    cmd_QueryInterface, cmd_AddRef, cmd_Release, cmd_GetTitle, cmd_GetIcon, cmd_GetToolTip,
    cmd_GetCanonicalName, cmd_GetState, cmd_Invoke, cmd_GetFlags, cmd_EnumSubCommands
};

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IClassFactory *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* The factory is static: it's counted by LockServer alone. */
static ULONG STDMETHODCALLTYPE factory_AddRef(IClassFactory *self)
{
    (void)self;
    return 2;
}

static ULONG STDMETHODCALLTYPE factory_Release(IClassFactory *self)
{
    (void)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE factory_CreateInstance(IClassFactory *self, IUnknown *outer, REFIID riid, void **out)
{
    RunCommand *cmd;
    HRESULT hr;
    (void)self;
    if (!out) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    cmd = (RunCommand *)HeapAlloc(GetProcessHeap(), 0, sizeof(*cmd));
    if (!cmd) return E_OUTOFMEMORY;
    cmd->iface.lpVtbl = &g_command_vtbl;
    cmd->refs = 1;
    InterlockedIncrement(&g_refs);
    hr = IExplorerCommand_QueryInterface(&cmd->iface, riid, out);
    IExplorerCommand_Release(&cmd->iface);
    return hr;
}

static HRESULT STDMETHODCALLTYPE factory_LockServer(IClassFactory *self, BOOL lock)
{
    (void)self;
    if (lock) InterlockedIncrement(&g_refs);
    else InterlockedDecrement(&g_refs);
    return S_OK;
}

static IClassFactoryVtbl g_factory_vtbl = {
    factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer
};
static IClassFactory g_factory = { &g_factory_vtbl };

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualCLSID(clsid, &CLSID_RunInSandbox)) return CLASS_E_CLASSNOTAVAILABLE;
    return IClassFactory_QueryInterface(&g_factory, riid, out);
}

STDAPI DllCanUnloadNow(void)
{
    return g_refs ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
