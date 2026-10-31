/*
 * sandbox_run.c -- Run in Sandbox (see sandbox_run.h).
 *
 * One worker thread per VM drains that VM's queue of files, so both the GUI and
 * the headless API only ever ask and then watch the status. Workers hold a VM's
 * unique_id, not a pointer: g_vms[] compacts when a VM is deleted.
 */

#include <winsock2.h>
#include "sandbox_run.h"
#include "disk_util.h"
#include "hcs_vm.h"
#include "snapshot.h"
#include "ui.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>

#ifndef AF_HYPERV
#define AF_HYPERV       34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif

#pragma pack(push, 1)
typedef struct {
    unsigned short Family;
    unsigned short Reserved;
    GUID VmId;
    GUID ServiceId;
} DropSockAddrHv;
#pragma pack(pop)

/* ASB_CH_FILEDROP in tools/transport/asb_transport.h; the guest listens on it
   (tools/agent/file_drop.c, tools/linux/agent/appsandbox-agent.c). */
#define DROP_CHANNEL        ASB_FILE_DROP_CHANNEL
#define DROP_CHUNK          (1024 * 1024)

#define SHUTDOWN_WAIT_MS    (3 * 60 * 1000)
#define START_WAIT_MS       (5 * 60 * 1000)
/* From boot to the file service answering. A Linux guest may rebuild its helpers
   first, and a Windows one may still be signing in. */
#define GUEST_WAIT_MS       (15 * 60 * 1000)
/* Agent up, this long, and still nothing on the file channel: an old guest. */
#define GUEST_OLD_AGENT_MS  (6 * 60 * 1000)
#define SAVED_WAIT_MS       (5 * 60 * 1000)
/* The guest waits up to 90 s for someone to sign in, then up to 20 s for the handler. */
#define OPEN_WAIT_MS        (2 * 60 * 1000 + 30 * 1000)
#define COPY_ATTEMPTS       3
#define HOST_SPARE_BYTES    (1024ull * 1024 * 1024)

typedef struct RunJob {
    UINT64          vm_id;
    AsbRunMode      mode;
    wchar_t        *path;
    struct RunJob  *next;
} RunJob;

static CRITICAL_SECTION g_run_cs;
static INIT_ONCE        g_run_once = INIT_ONCE_STATIC_INIT;
static RunJob          *g_jobs;
static wchar_t          g_last_vm[256];

static BOOL CALLBACK init_run_cs(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_run_cs);
    return TRUE;
}

static void run_lock(void)
{
    InitOnceExecuteOnce(&g_run_once, init_run_cs, NULL, NULL);
    EnterCriticalSection(&g_run_cs);
}

static void run_unlock(void) { LeaveCriticalSection(&g_run_cs); }

static VmInstance *find_vm(UINT64 id, AsbVm *vm_out)
{
    int i, count = asb_vm_count();
    for (i = 0; i < count; i++) {
        AsbVm vm = asb_vm_get(i);
        VmInstance *inst = asb_vm_instance(vm);
        if (inst && inst->unique_id == id) {
            if (vm_out) *vm_out = vm;
            return inst;
        }
    }
    return NULL;
}

static void set_status(UINT64 id, int phase, int pct, const wchar_t *fmt, ...)
{
    AsbVm vm = NULL;
    VmInstance *inst;
    va_list ap;
    wchar_t text[512];

    va_start(ap, fmt);
    vswprintf_s(text, _countof(text), fmt, ap);
    va_end(ap);

    run_lock();
    inst = find_vm(id, &vm);
    if (inst) {
        inst->sandbox_phase = phase;
        inst->sandbox_pct = pct;
        wcscpy_s(inst->sandbox_message, _countof(inst->sandbox_message), text);
    }
    run_unlock();
    if (inst) {
        ui_log(L"Run in Sandbox: \"%s\": %s", inst->name, text);
        asb_notify_vm_state(vm);
    }
}

/* ---- Names ---- */

void asb_run_encode_name(const wchar_t *in, wchar_t *out, size_t chars)
{
    size_t used = 0;
    char utf8[1024];
    const unsigned char *p;
    out[0] = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, utf8, sizeof(utf8), NULL, NULL)) return;
    for (p = (const unsigned char *)utf8; *p && used + 4 < chars; p++) {
        if (*p >= 0x20 && *p < 0x7F && *p != L'%') {
            out[used++] = (wchar_t)*p;
        } else {
            swprintf_s(out + used, chars - used, L"%%%02X", *p);
            used += 3;
        }
    }
    out[used] = L'\0';
}

void asb_run_decode_name(const wchar_t *in, wchar_t *out, size_t chars)
{
    char utf8[1024];
    size_t used = 0;
    const wchar_t *p;
    for (p = in; *p && used + 1 < sizeof(utf8); p++) {
        if (*p == L'%' && iswxdigit(p[1]) && iswxdigit(p[2])) {
            wchar_t hex[3] = { p[1], p[2], 0 };
            utf8[used++] = (char)wcstoul(hex, NULL, 16);
            p += 2;
        } else {
            utf8[used++] = (char)*p;
        }
    }
    utf8[used] = '\0';
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)chars)) out[0] = L'\0';
}

static const wchar_t *base_name(const wchar_t *path)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

/* CreateFile takes a long path only with the \\?\ prefix. Paths from Explorer can
   be longer than MAX_PATH, and the app isn't marked long-path aware. */
static wchar_t *long_path(const wchar_t *path)
{
    size_t len = wcslen(path) + 16;
    wchar_t *out = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (!out) return NULL;
    if (wcslen(path) < MAX_PATH - 12 || wcsncmp(path, L"\\\\?\\", 4) == 0)
        wcscpy_s(out, len, path);
    else if (wcsncmp(path, L"\\\\", 2) == 0)
        swprintf_s(out, len, L"\\\\?\\UNC\\%s", path + 2);
    else
        swprintf_s(out, len, L"\\\\?\\%s", path);
    return out;
}

static HANDLE open_source(const wchar_t *path, ULONGLONG *size_out)
{
    wchar_t *lp = long_path(path);
    HANDLE h;
    LARGE_INTEGER size;
    if (!lp) { SetLastError(ERROR_OUTOFMEMORY); return INVALID_HANDLE_VALUE; }
    /* Read-only, and tolerant of whatever else has the file open. This handle is
       the only thing that ever touches the host's copy. */
    h = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(lp);
    if (h == INVALID_HANDLE_VALUE) return h;
    {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(h, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !GetFileSizeEx(h, &size)) {
            CloseHandle(h);
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
    }
    if (size_out) *size_out = (ULONGLONG)size.QuadPart;
    return h;
}

/* ---- Suitability ---- */

const wchar_t *asb_run_unsuitable_reason(AsbVm vm)
{
    VmInstance *v = asb_vm_instance(vm);
    if (!v) return L"This VM no longer exists.";
    if (v->is_template) return L"Templates can't run files.";
    if (v->building_vhdx) return L"It is still being built.";
    if (v->install_error[0]) return L"Its setup failed.";
    if (!v->install_complete) return L"It is still being set up.";
    if (v->lab_mode) return L"Malware lab VMs use their own recorded runs.";
    return NULL;
}

/* ---- The throwaway disk ---- */

static BOOL disk_is_frozen(SnapshotTree *tree, const wchar_t *disk)
{
    int i, b;
    if (tree->base_vhdx[0] && _wcsicmp(disk, tree->base_vhdx) == 0) {
        if (tree->count > 0) return TRUE;
        for (b = 0; b < tree->base_branch_count; b++)
            if (tree->base_branches[b].valid) return TRUE;
    }
    for (i = 0; i < tree->count; i++)
        if (tree->nodes[i].valid && _wcsicmp(disk, tree->nodes[i].snap_vhdx) == 0) return TRUE;
    return FALSE;
}

static void overlay_folder(VmInstance *inst, SnapshotTree *tree, wchar_t *out, size_t chars)
{
    if (tree && tree->base_dir[0]) wcscpy_s(out, chars, tree->base_dir);
    else swprintf_s(out, chars, L"%s\\snapshots", inst->storage_root);
    CreateDirectoryW(out, NULL);
}

static void delete_overlay_file(const wchar_t *path)
{
    int tries;
    /* HCS can hold the file for a moment after the VM stops. If it still won't go,
       asb_run_sweep_orphans takes it at the next start: nothing refers to it now. */
    for (tries = 0; tries < 10; tries++) {
        if (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
        Sleep(1000);
    }
    ui_log(L"Run in Sandbox: could not delete %s yet (%lu); it goes at the next start.",
           path, GetLastError());
}

/* The VM must be stopped. On success it boots from the new overlay, and that is on disk. */
static HRESULT overlay_create(AsbVm vm, VmInstance *inst, const wchar_t *file_name,
                              wchar_t *error, size_t error_chars)
{
    SnapshotTree *tree = asb_vm_snap_tree(vm);
    wchar_t folder[MAX_PATH], overlay[MAX_PATH], parent[MAX_PATH], id[40];
    GUID g = {0};
    HRESULT hr;

    if (InterlockedCompareExchange(&inst->management_busy, 1, 0) != 0) {
        swprintf_s(error, error_chars, L"The VM is busy with another operation.");
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    /* A closed VM keeps no system; a stale one would boot its old disk. */
    if (inst->handle) hcs_close_vm(inst);

    /* A disposable VM starts each session clean, so a run should too. */
    if (inst->disposable_snap[0]) {
        hr = asb_disposable_new_session(vm);
        if (FAILED(hr)) {
            InterlockedExchange(&inst->management_busy, 0);
            swprintf_s(error, error_chars, L"Could not make a clean copy of this disposable VM (0x%08X).", hr);
            return hr;
        }
    }
    /* Merging into a disk other disks hang off would corrupt them. */
    if (tree && disk_is_frozen(tree, inst->vhdx_path)) {
        int node = -2, i;
        for (i = 0; i < tree->count; i++)
            if (tree->nodes[i].valid && _wcsicmp(inst->vhdx_path, tree->nodes[i].snap_vhdx) == 0) node = i;
        hr = snapshot_new_branch(tree, inst, node);
        if (FAILED(hr)) {
            InterlockedExchange(&inst->management_busy, 0);
            swprintf_s(error, error_chars, L"This VM's current disk is a frozen checkpoint. Select a branch of it first.");
            return hr;
        }
    }

    if (FAILED(hr = CoCreateGuid(&g))) {
        InterlockedExchange(&inst->management_busy, 0);
        return hr;
    }
    swprintf_s(id, _countof(id), L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
               g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
               g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    overlay_folder(inst, tree, folder, _countof(folder));
    if (_snwprintf_s(overlay, MAX_PATH, _TRUNCATE, L"%s\\sandbox_%s.vhdx", folder, id) < 0) {
        InterlockedExchange(&inst->management_busy, 0);
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }
    wcscpy_s(parent, MAX_PATH, inst->vhdx_path);
    hr = vhdx_create_differencing(overlay, parent);
    if (FAILED(hr)) {
        InterlockedExchange(&inst->management_busy, 0);
        swprintf_s(error, error_chars, L"Could not create the throwaway checkpoint (0x%08X).", hr);
        return hr;
    }

    wcscpy_s(inst->sandbox_run_id, _countof(inst->sandbox_run_id), id);
    wcscpy_s(inst->sandbox_run_parent, MAX_PATH, parent);
    wcscpy_s(inst->sandbox_run_overlay, MAX_PATH, overlay);
    wcsncpy_s(inst->sandbox_run_file, _countof(inst->sandbox_run_file), file_name, _TRUNCATE);
    wcscpy_s(inst->vhdx_path, MAX_PATH, overlay);
    if (!asb_save_checked()) {
        inst->sandbox_run_id[0] = L'\0';
        inst->sandbox_run_parent[0] = L'\0';
        inst->sandbox_run_overlay[0] = L'\0';
        inst->sandbox_run_file[0] = L'\0';
        wcscpy_s(inst->vhdx_path, MAX_PATH, parent);
        DeleteFileW(overlay);
        InterlockedExchange(&inst->management_busy, 0);
        swprintf_s(error, error_chars, L"Could not record the run in vms.cfg, so no checkpoint was taken.");
        return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    InterlockedExchange(&inst->management_busy, 0);
    ui_log(L"Run in Sandbox: \"%s\" now boots from a throwaway checkpoint on %s.", inst->name, parent);
    return S_OK;
}

/* Point the VM back at its own disk. Recorded first, deleted second: a crash between
   the two leaves an orphan for the sweep, never a VM pointing at a deleted disk. */
static HRESULT overlay_drop(VmInstance *inst)
{
    wchar_t overlay[MAX_PATH], saved_id[40], saved_parent[MAX_PATH], saved_file[260];

    if (GetFileAttributesW(inst->sandbox_run_parent) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    wcscpy_s(overlay, MAX_PATH, inst->sandbox_run_overlay);
    wcscpy_s(saved_id, _countof(saved_id), inst->sandbox_run_id);
    wcscpy_s(saved_parent, MAX_PATH, inst->sandbox_run_parent);
    wcscpy_s(saved_file, _countof(saved_file), inst->sandbox_run_file);
    if (inst->handle) hcs_close_vm(inst);

    wcscpy_s(inst->vhdx_path, MAX_PATH, inst->sandbox_run_parent);
    inst->sandbox_run_id[0] = L'\0';
    inst->sandbox_run_parent[0] = L'\0';
    inst->sandbox_run_overlay[0] = L'\0';
    inst->sandbox_run_file[0] = L'\0';
    inst->ssh_key_deployed = FALSE;   /* the disk just changed */
    if (!asb_save_checked()) {
        wcscpy_s(inst->vhdx_path, MAX_PATH, overlay);
        wcscpy_s(inst->sandbox_run_id, _countof(inst->sandbox_run_id), saved_id);
        wcscpy_s(inst->sandbox_run_parent, MAX_PATH, saved_parent);
        wcscpy_s(inst->sandbox_run_overlay, MAX_PATH, overlay);
        wcscpy_s(inst->sandbox_run_file, _countof(inst->sandbox_run_file), saved_file);
        return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    delete_overlay_file(overlay);
    return S_OK;
}

void asb_run_sweep_orphans(VmInstance *inst, const wchar_t *snap_dir)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (_snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\sandbox_*.vhdx", snap_dir) < 0) return;
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", snap_dir, fd.cFileName) < 0) continue;
        /* Either pointer keeping it is enough: never delete a disk anything names. */
        if (_wcsicmp(path, inst->sandbox_run_overlay) == 0 || _wcsicmp(path, inst->vhdx_path) == 0)
            continue;
        if (DeleteFileW(path))
            ui_log(L"Run in Sandbox: removed a leftover throwaway disk: %s", path);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

/* ---- Sending the file ---- */

static SOCKET drop_connect(VmInstance *v, DWORD timeout_ms)
{
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    DropSockAddrHv addr;
    u_long nonblock = 1;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD send_timeout = 60000;

    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    ioctlsocket(s, FIONBIO, &nonblock);
    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = v->runtime_id;
    hcs_service_guid(v->os_type, DROP_CHANNEL, &addr.ServiceId);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(s, &wfds); FD_SET(s, &efds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&send_timeout, sizeof(send_timeout));
    return s;
}

static BOOL drop_send_all(SOCKET s, const char *p, int len)
{
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* One line of status from the guest. Its contents are guest data: they are shown,
   never acted on. */
static BOOL drop_recv_line(SOCKET s, char *buf, int cap, DWORD timeout_ms)
{
    int pos = 0;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (pos + 1 < cap) {
        fd_set rfds;
        struct timeval tv;
        char c;
        int ready, n;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return FALSE;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        ready = select(0, &rfds, NULL, NULL, &tv);
        if (ready == SOCKET_ERROR) return FALSE;
        if (ready == 0) continue;
        n = recv(s, &c, 1, 0);
        if (n <= 0) return FALSE;
        if (c == '\n') break;
        if (c != '\r' && (unsigned char)c >= 0x20) buf[pos++] = c;
    }
    buf[pos] = '\0';
    return TRUE;
}

static const wchar_t *guest_error_text(const char *line)
{
    if (strcmp(line, "error:nospace") == 0) return L"The VM's disk doesn't have room for this file.";
    if (strcmp(line, "error:write") == 0) return L"The VM could not write the file; its disk may be full.";
    if (strcmp(line, "error:nouser") == 0) return L"The VM has no user account to give the file to.";
    if (strcmp(line, "error:folder") == 0 || strcmp(line, "error:create") == 0)
        return L"The VM could not create the file.";
    return NULL;
}

typedef enum { DROP_OK, DROP_RETRY, DROP_FAILED } DropResult;

static DropResult send_file(UINT64 id, VmInstance *v, SOCKET s, const wchar_t *path, ULONGLONG size,
                            wchar_t *why, size_t why_chars)
{
    const wchar_t *name = base_name(path);
    char head[1200], utf8_name[1024], line[1200];
    char *buf = NULL;
    HANDLE file;
    ULONGLONG sent = 0, last_tick;
    DropResult result = DROP_RETRY;
    ULONGLONG opened_size = 0;

    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8_name, sizeof(utf8_name), NULL, NULL)) {
        swprintf_s(why, why_chars, L"The file name is too long.");
        return DROP_FAILED;
    }
    file = open_source(path, &opened_size);
    if (file == INVALID_HANDLE_VALUE) {
        swprintf_s(why, why_chars, L"Could not read the file on the host (error %lu).", GetLastError());
        return DROP_FAILED;
    }
    size = opened_size;
    sprintf_s(head, sizeof(head), "ASBDROP1 %llu\n%s\n", size, utf8_name);
    if (!drop_send_all(s, head, (int)strlen(head)) ||
        !drop_recv_line(s, line, sizeof(line), 30000)) goto done;
    if (strcmp(line, "ready") != 0) {
        const wchar_t *text = guest_error_text(line);
        swprintf_s(why, why_chars, L"%s", text ? text : L"The VM refused the file.");
        result = DROP_FAILED;
        goto done;
    }

    buf = (char *)malloc(DROP_CHUNK);
    if (!buf) { swprintf_s(why, why_chars, L"Out of memory."); result = DROP_FAILED; goto done; }
    last_tick = GetTickCount64();
    while (sent < size) {
        DWORD want = size - sent < DROP_CHUNK ? (DWORD)(size - sent) : DROP_CHUNK, got = 0;
        if (!ReadFile(file, buf, want, &got, NULL) || got == 0) {
            /* Closing here makes the guest delete its partial copy. */
            swprintf_s(why, why_chars, L"The file changed or could not be read while copying (error %lu).", GetLastError());
            result = DROP_FAILED;
            goto done;
        }
        if (!drop_send_all(s, buf, (int)got)) goto done;
        sent += got;
        if (GetTickCount64() - last_tick >= 1000) {
            last_tick = GetTickCount64();
            set_status(id, ASB_RUN_PHASE_COPYING, (int)(sent * 100 / (size ? size : 1)),
                       L"Copying %s into the VM...", name);
        }
    }
    if (!drop_recv_line(s, line, sizeof(line), SAVED_WAIT_MS)) goto done;
    if (strncmp(line, "saved:", 6) != 0) {
        const wchar_t *text = guest_error_text(line);
        swprintf_s(why, why_chars, L"%s", text ? text : L"The VM could not save the file.");
        result = DROP_FAILED;
        goto done;
    }
    {
        wchar_t guest_path[512];
        if (!MultiByteToWideChar(CP_UTF8, 0, line + 6, -1, guest_path, _countof(guest_path)))
            wcscpy_s(guest_path, _countof(guest_path), name);
        run_lock();
        wcscpy_s(v->sandbox_guest_path, _countof(v->sandbox_guest_path), guest_path);
        run_unlock();

        set_status(id, ASB_RUN_PHASE_OPENING, 100, L"Opening %s in the VM...", name);
        /* The copy is safe from here on, so a failure below isn't a reason to send it again. */
        if (!drop_send_all(s, "open\n", 5) || !drop_recv_line(s, line, sizeof(line), OPEN_WAIT_MS))
            swprintf_s(why, why_chars, L"Copied to %s, but the VM didn't say whether it opened.", guest_path);
        else if (strcmp(line, "opened") == 0)
            swprintf_s(why, why_chars, L"Opened %s in the VM.", name);
        else if (strcmp(line, "nohandler") == 0)
            swprintf_s(why, why_chars, L"Copied to %s. Nothing in this VM opens this type of file.", guest_path);
        else if (strcmp(line, "nosession") == 0)
            swprintf_s(why, why_chars, L"Copied to %s. Nobody is signed in to the VM, so open it from there after signing in.", guest_path);
        else
            swprintf_s(why, why_chars, L"Copied to %s, but it could not be opened.", guest_path);
        result = DROP_OK;
    }
done:
    free(buf);
    CloseHandle(file);
    return result;
}

/* The VM might still be booting, signing in, or rebuilding its Linux helpers, and
   that last one restarts the agent, so a dropped copy is retried. */
static BOOL deliver(UINT64 id, const wchar_t *path, ULONGLONG size)
{
    const wchar_t *name = base_name(path);
    ULONGLONG deadline = GetTickCount64() + GUEST_WAIT_MS, agent_since = 0;
    int attempts = 0;
    wchar_t why[512] = L"";

    set_status(id, ASB_RUN_PHASE_WAITING_GUEST, 0, L"Waiting for the VM to be ready for %s...", name);
    while (GetTickCount64() < deadline) {
        AsbVm vm = NULL;
        VmInstance *v = find_vm(id, &vm);
        SOCKET s;

        if (!v) return FALSE;
        if (!v->running) {
            set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM stopped before %s could be copied in.", name);
            return FALSE;
        }
        if (v->agent_online && !agent_since) agent_since = GetTickCount64();
        s = drop_connect(v, 2000);
        if (s == INVALID_SOCKET) {
            if (agent_since && GetTickCount64() - agent_since > GUEST_OLD_AGENT_MS) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0,
                           L"The VM's guest helpers don't answer. Restart the VM so they can update, then try again.");
                return FALSE;
            }
            Sleep(2000);
            continue;
        }
        set_status(id, ASB_RUN_PHASE_COPYING, 0, L"Copying %s into the VM...", name);
        {
            DropResult r = send_file(id, v, s, path, size, why, _countof(why));
            closesocket(s);
            if (r == DROP_OK) {
                set_status(id, ASB_RUN_PHASE_READY, 100, L"%s", why);
                return TRUE;
            }
            if (r == DROP_FAILED) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0, L"%s", why);
                return FALSE;
            }
        }
        if (++attempts >= COPY_ATTEMPTS) {
            set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The connection to the VM kept dropping while copying %s.", name);
            return FALSE;
        }
        Sleep(3000);
    }
    set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM was not ready in time to take %s.", name);
    return FALSE;
}

/* ---- The worker ---- */

static BOOL host_has_room(VmInstance *v, ULONGLONG size)
{
    ULARGE_INTEGER free_bytes;
    wchar_t folder[MAX_PATH], *slash;
    wcscpy_s(folder, MAX_PATH, v->vhdx_path);
    slash = wcsrchr(folder, L'\\');
    if (slash) slash[1] = L'\0';
    if (!GetDiskFreeSpaceExW(folder, &free_bytes, NULL, NULL)) return TRUE;
    return free_bytes.QuadPart >= size + HOST_SPARE_BYTES;
}

static BOOL wait_stopped(UINT64 id, DWORD timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        VmInstance *v = find_vm(id, NULL);
        if (!v) return FALSE;
        if (!v->running) return TRUE;
        if (GetTickCount64() >= deadline) return FALSE;
        Sleep(500);
    }
}

/* Starts run on a background thread; failure shows up as not running once it lets
   go of management_busy. */
static BOOL wait_started(UINT64 id)
{
    ULONGLONG start = GetTickCount64();
    for (;;) {
        VmInstance *v = find_vm(id, NULL);
        if (!v) return FALSE;
        if (v->running) return TRUE;
        if (GetTickCount64() - start > 2000 &&
            InterlockedCompareExchange(&v->management_busy, 0, 0) == 0)
            return FALSE;
        if (GetTickCount64() - start > START_WAIT_MS) return FALSE;
        Sleep(250);
    }
}

static void request_display(UINT64 id)
{
    AsbVm vm = NULL;
    VmInstance *v = find_vm(id, &vm);
    if (!v) return;
    v->sandbox_want_display = TRUE;
    asb_notify_vm_state(vm);
}

static void run_one(RunJob *job)
{
    UINT64 id = job->vm_id;
    AsbVm vm = NULL;
    VmInstance *v = find_vm(id, &vm);
    const wchar_t *name = base_name(job->path);
    ULONGLONG size = 0;
    BOOL created_now = FALSE;
    wchar_t error[256] = L"";
    HANDLE probe;

    if (!v) return;

    /* Check the file before touching the VM: a typo shouldn't shut anything down. */
    probe = open_source(job->path, &size);
    if (probe == INVALID_HANDLE_VALUE) {
        set_status(id, ASB_RUN_PHASE_FAILED, 0, L"Could not read %s on the host (error %lu).", name, GetLastError());
        return;
    }
    CloseHandle(probe);
    if (!host_has_room(v, size)) {
        set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The host's disk doesn't have room for %s and the VM's changes.", name);
        return;
    }

    if (!v->sandbox_run_id[0] && job->mode != ASB_RUN_MODE_NO_CHECKPOINT) {
        if (v->running) {
            if (job->mode != ASB_RUN_MODE_SHUTDOWN_FIRST) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM is running, so no checkpoint can be taken. Nothing was changed.");
                return;
            }
            set_status(id, ASB_RUN_PHASE_SHUTTING_DOWN, 0, L"Shutting the VM down to take a checkpoint...");
            if (FAILED(asb_vm_shutdown(vm)) || !wait_stopped(id, SHUTDOWN_WAIT_MS)) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0,
                           L"The VM didn't shut down, so no checkpoint was taken. An open app may be asking to save. Deal with it in the VM and try again.");
                return;
            }
            v = find_vm(id, &vm);
            if (!v) return;
        }
        set_status(id, ASB_RUN_PHASE_CHECKPOINT, 0, L"Taking a throwaway checkpoint...");
        {
            HRESULT hr = overlay_create(vm, v, name, error, _countof(error));
            if (FAILED(hr)) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0, L"%s", error);
                return;
            }
        }
        created_now = TRUE;
    }

    if (!v->running) {
        HRESULT hr;
        set_status(id, ASB_RUN_PHASE_STARTING, 0, L"Starting the VM...");
        /* The file is the point here, so a missing shared-storage appliance doesn't stop it. */
        hr = asb_vm_start_ex(vm, -1, -1, NULL, TRUE);
        if (FAILED(hr) || !wait_started(id)) {
            v = find_vm(id, &vm);
            /* It never booted, so there is nothing in the checkpoint to lose. */
            if (created_now && v && !v->running && SUCCEEDED(overlay_drop(v)))
                set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM didn't start, so the checkpoint was removed. Nothing was changed.");
            else
                set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM didn't start.");
            return;
        }
    }
    request_display(id);
    deliver(id, job->path, size);
}

static void finish_run(UINT64 id, BOOL keep)
{
    AsbVm vm = NULL;
    VmInstance *v = find_vm(id, &vm);
    HRESULT hr;
    int tries;

    if (!v || !v->sandbox_run_id[0]) return;

    if (v->running) {
        if (keep) {
            set_status(id, ASB_RUN_PHASE_FINISHING, 0, L"Shutting the VM down to keep its changes...");
            if (FAILED(asb_vm_shutdown(vm)) || !wait_stopped(id, SHUTDOWN_WAIT_MS)) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0,
                           L"The VM didn't shut down, so nothing was merged and the run is still open. An open app may be asking to save.");
                return;
            }
        } else {
            set_status(id, ASB_RUN_PHASE_FINISHING, 0, L"Stopping the VM...");
            asb_vm_stop(vm);
            if (!wait_stopped(id, 60000)) {
                set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM didn't stop, so the run is still open.");
                return;
            }
        }
        v = find_vm(id, &vm);
        if (!v) return;
    }

    if (InterlockedCompareExchange(&v->management_busy, 1, 0) != 0) {
        set_status(id, ASB_RUN_PHASE_FAILED, 0, L"The VM is busy with another operation. The run is still open.");
        return;
    }
    if (keep) {
        set_status(id, ASB_RUN_PHASE_FINISHING, 0, L"Merging the changes into the VM's disk...");
        if (v->handle) hcs_close_vm(v);
        /* A merge copies the overlay's blocks down and the overlay still wins while
           it is in the chain, so one that dies partway is safe to run again. */
        hr = E_FAIL;
        for (tries = 0; tries < 10; tries++) {
            hr = vhdx_merge(v->sandbox_run_overlay);
            if (hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) &&
                hr != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) break;
            Sleep(1000);
        }
        if (FAILED(hr)) {
            InterlockedExchange(&v->management_busy, 0);
            set_status(id, ASB_RUN_PHASE_FAILED, 0, L"Could not merge the changes (0x%08X). The run is still open and nothing was lost.", hr);
            return;
        }
    } else {
        set_status(id, ASB_RUN_PHASE_FINISHING, 0, L"Discarding the changes...");
    }

    hr = overlay_drop(v);
    InterlockedExchange(&v->management_busy, 0);
    if (FAILED(hr)) {
        set_status(id, ASB_RUN_PHASE_FAILED, 0,
                   hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                       ? L"The VM's original disk is missing, so the run was left as it is."
                       : L"Could not record the end of the run in vms.cfg (0x%08X). The run is still open.", hr);
        return;
    }
    v->sandbox_guest_path[0] = L'\0';
    set_status(id, ASB_RUN_PHASE_IDLE, 0, keep
               ? L"The changes were kept and the checkpoint removed."
               : L"The VM was rolled back and the checkpoint removed.");
}

static DWORD WINAPI run_worker(LPVOID param)
{
    UINT64 id = *(UINT64 *)param;
    free(param);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        RunJob *job = NULL, **pp;
        run_lock();
        for (pp = &g_jobs; *pp; pp = &(*pp)->next) {
            if ((*pp)->vm_id == id) {
                job = *pp;
                *pp = job->next;
                break;
            }
        }
        if (!job) {
            VmInstance *v = find_vm(id, NULL);
            if (v) InterlockedExchange(&v->sandbox_worker, 0);
            run_unlock();
            break;
        }
        run_unlock();
        if (job->path) run_one(job);
        else finish_run(id, job->mode == ASB_RUN_MODE_CHECKPOINT);
        free(job->path);
        free(job);
    }
    CoUninitialize();
    {
        AsbVm vm = NULL;
        if (find_vm(id, &vm)) asb_notify_vm_state(vm);
    }
    return 0;
}

/* Caller holds the lock. */
static HRESULT enqueue(VmInstance *v, RunJob *job)
{
    RunJob **pp = &g_jobs;
    while (*pp) pp = &(*pp)->next;
    job->next = NULL;
    *pp = job;
    if (InterlockedCompareExchange(&v->sandbox_worker, 1, 0) == 0) {
        UINT64 *arg = (UINT64 *)malloc(sizeof(UINT64));
        HANDLE t;
        if (arg) {
            *arg = v->unique_id;
            t = CreateThread(NULL, 0, run_worker, arg, 0, NULL);
            if (t) { CloseHandle(t); return S_OK; }
            free(arg);
        }
        InterlockedExchange(&v->sandbox_worker, 0);
        for (pp = &g_jobs; *pp; pp = &(*pp)->next)
            if (*pp == job) { *pp = job->next; break; }
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT asb_run_file(AsbVm vm, const wchar_t *path, AsbRunMode mode,
                     wchar_t *error, size_t error_chars)
{
    VmInstance *v = asb_vm_instance(vm);
    const wchar_t *reason;
    wchar_t local_error[16];
    RunJob *job;
    HANDLE probe;
    HRESULT hr;

    if (!error) { error = local_error; error_chars = _countof(local_error); }
    error[0] = L'\0';
    if (!v) return E_INVALIDARG;
    reason = asb_run_unsuitable_reason(vm);
    if (reason) { swprintf_s(error, error_chars, L"%s", reason); return E_ACCESSDENIED; }
    /* Absolute only: the app's working folder means nothing to whoever asked. */
    if (!path || !((iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\') ||
                   (path[0] == L'\\' && path[1] == L'\\'))) {
        swprintf_s(error, error_chars, L"Give the file's full path.");
        return E_INVALIDARG;
    }
    probe = open_source(path, NULL);
    if (probe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        swprintf_s(error, error_chars, err == ERROR_INVALID_PARAMETER
                   ? L"Only files can be run, not folders."
                   : L"Could not read this file (error %lu).", err);
        return HRESULT_FROM_WIN32(err);
    }
    CloseHandle(probe);

    run_lock();
    if (v->running && !v->sandbox_run_id[0] && mode == ASB_RUN_MODE_CHECKPOINT) {
        run_unlock();
        swprintf_s(error, error_chars, L"The VM is running, and a checkpoint needs it off.");
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    job = (RunJob *)calloc(1, sizeof(RunJob));
    if (job) job->path = _wcsdup(path);
    if (!job || !job->path) {
        run_unlock();
        if (job) free(job);
        return E_OUTOFMEMORY;
    }
    job->vm_id = v->unique_id;
    job->mode = mode;
    hr = enqueue(v, job);
    if (FAILED(hr)) { free(job->path); free(job); }
    else {
        v->sandbox_phase = ASB_RUN_PHASE_CHECKPOINT;
        v->sandbox_pct = 0;
        swprintf_s(v->sandbox_message, _countof(v->sandbox_message), L"Getting %s ready...", base_name(path));
        asb_set_last_sandbox_vm(v->name);
    }
    run_unlock();
    if (SUCCEEDED(hr)) asb_notify_vm_state(vm);
    return hr;
}

HRESULT asb_run_finish(AsbVm vm, BOOL keep, wchar_t *error, size_t error_chars)
{
    VmInstance *v = asb_vm_instance(vm);
    wchar_t local_error[16];
    RunJob *job;
    HRESULT hr;

    if (!error) { error = local_error; error_chars = _countof(local_error); }
    error[0] = L'\0';
    if (!v) return E_INVALIDARG;
    run_lock();
    if (!v->sandbox_run_id[0]) {
        run_unlock();
        swprintf_s(error, error_chars, L"This VM has no run to finish.");
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (InterlockedCompareExchange(&v->sandbox_worker, 0, 0) != 0) {
        run_unlock();
        swprintf_s(error, error_chars, L"The run is busy. Try again when it is done.");
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    job = (RunJob *)calloc(1, sizeof(RunJob));
    if (!job) { run_unlock(); return E_OUTOFMEMORY; }
    job->vm_id = v->unique_id;
    /* No path marks a finish; the mode field carries the choice. */
    job->mode = keep ? ASB_RUN_MODE_CHECKPOINT : ASB_RUN_MODE_NO_CHECKPOINT;
    hr = enqueue(v, job);
    if (FAILED(hr)) free(job);
    run_unlock();
    return hr;
}

BOOL asb_run_active(AsbVm vm)
{
    VmInstance *v = asb_vm_instance(vm);
    return v && v->sandbox_run_id[0];
}

void asb_run_get_status(AsbVm vm, AsbRunStatus *out)
{
    VmInstance *v = asb_vm_instance(vm);
    ZeroMemory(out, sizeof(*out));
    if (!v) return;
    run_lock();
    out->active = v->sandbox_run_id[0] != L'\0';
    out->busy = InterlockedCompareExchange(&v->sandbox_worker, 0, 0) != 0;
    out->phase = v->sandbox_phase;
    out->pct = v->sandbox_pct;
    wcscpy_s(out->id, _countof(out->id), v->sandbox_run_id);
    wcscpy_s(out->file, _countof(out->file), v->sandbox_run_file);
    wcscpy_s(out->message, _countof(out->message), v->sandbox_message);
    wcscpy_s(out->guest_path, _countof(out->guest_path), v->sandbox_guest_path);
    run_unlock();
}

void asb_run_clear_status(AsbVm vm)
{
    VmInstance *v = asb_vm_instance(vm);
    if (!v) return;
    run_lock();
    if (InterlockedCompareExchange(&v->sandbox_worker, 0, 0) == 0) {
        v->sandbox_phase = v->sandbox_run_id[0] ? ASB_RUN_PHASE_READY : ASB_RUN_PHASE_IDLE;
        v->sandbox_message[0] = L'\0';
        v->sandbox_guest_path[0] = L'\0';
    }
    run_unlock();
    asb_notify_vm_state(vm);
}

BOOL asb_run_take_display_request(AsbVm vm)
{
    VmInstance *v = asb_vm_instance(vm);
    BOOL want = FALSE;
    if (!v) return FALSE;
    run_lock();
    want = v->sandbox_want_display;
    v->sandbox_want_display = FALSE;
    run_unlock();
    return want;
}

void asb_set_last_sandbox_vm(const wchar_t *name)
{
    if (!name) return;
    if (_wcsicmp(g_last_vm, name) == 0) return;
    wcsncpy_s(g_last_vm, _countof(g_last_vm), name, _TRUNCATE);
    asb_save();
}

const wchar_t *asb_get_last_sandbox_vm(void) { return g_last_vm; }

void asb_run_load_last_vm(const wchar_t *name)
{
    wcsncpy_s(g_last_vm, _countof(g_last_vm), name, _TRUNCATE);
}
