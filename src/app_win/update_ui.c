/*
 * update_ui.c -- Auto-update as the app sees it.
 *
 * Installing an update closes the app, and closing the app terminates every VM
 * (asb_cleanup), so an update never goes ahead while a VM is running or being
 * built. The download and verification happen straight away; the install then
 * waits until the last VM stops. Nothing here stops a VM.
 */

#include "update_ui.h"
#include "update_config.h"
#include "updater.h"
#include "webview2_bridge.h"
#include "ui.h"
#include "asb_core.h"
#include "hcs_vm.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

#define UPDATE_FIRST_CHECK_MS  (15u * 1000)
#define UPDATE_RECHECK_MS      (24u * 60 * 60 * 1000)

enum { EVT_CHECK_DONE = 1, EVT_DOWNLOAD_PROGRESS, EVT_DOWNLOAD_DONE };

typedef enum {
    PHASE_IDLE,
    PHASE_CHECKING,
    PHASE_DOWNLOADING,
    PHASE_WAITING_FOR_VMS,
    PHASE_INSTALLING
} UpdatePhase;

typedef struct {
    UpdateCheckResult result;
    UpdateInfo info;
    BOOL manual;
    wchar_t err[UPDATER_ERR_CHARS];
} CheckResult;

typedef struct {
    BOOL ok;
    HANDLE held;
    wchar_t path[MAX_PATH];
    wchar_t err[UPDATER_ERR_CHARS];
} DownloadResult;

static HWND g_hwnd;
static UpdatePhase g_phase = PHASE_IDLE;
static BOOL g_auto_check = TRUE;
static BOOL g_include_prerelease = FALSE;
static BOOL g_installed = FALSE;
static BOOL g_have_update = FALSE;
static UpdateInfo g_update;
static int g_progress = 0;
static wchar_t g_message[UPDATER_ERR_CHARS];
static BOOL g_message_is_error = FALSE;
static wchar_t g_verified_path[MAX_PATH];
static HANDLE g_verified_handle = NULL;
static volatile LONG g_cancel = 0;

static const UpdateEndpoint g_endpoint = {
    ASB_UPDATE_API_URL, ASB_UPDATE_ASSET_PREFIX, ASB_UPDATE_RELEASE_PREFIX
};
static const wchar_t *const g_pins[] = ASB_UPDATE_SIGNER_PINS;

/* What an update must wait for: the same set the headless API's shutdown guard
   refuses on. A building VM isn't running yet, but its worker holds the disk. */
static BOOL vm_blocks_update(VmInstance *v)
{
    return v && (v->running || v->building_vhdx || v->management_busy);
}

static int count_blockers(void)
{
    int i, n = 0, count = asb_vm_count();
    VmInstance *appliance = asb_shared_appliance_runtime();

    for (i = 0; i < count; i++) {
        if (vm_blocks_update(asb_vm_instance(asb_vm_get(i)))) n++;
    }
    /* A running appliance stops cleanly with the app, as on any exit. Only one
       still being set up has work to lose. */
    if (appliance && appliance->building_vhdx) n++;
    return n;
}

static void post_state(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    int i, count = asb_vm_count(), written = 0;
    VmInstance *appliance = asb_shared_appliance_runtime();
    static const wchar_t *const phases[] = {
        L"idle", L"checking", L"downloading", L"waiting", L"installing"
    };

    jb_init(&jb, buf, _countof(buf));
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"updateState");
    jb_string(&jb, L"phase", phases[g_phase]);
    jb_string(&jb, L"currentVersion", updater_current_version());
    jb_bool(&jb, L"autoCheck", g_auto_check);
    jb_bool(&jb, L"includePrereleases", g_include_prerelease);
    jb_bool(&jb, L"canInstall", g_installed);
    jb_bool(&jb, L"available", g_have_update);
    jb_string(&jb, L"message", g_message);
    jb_bool(&jb, L"isError", g_message_is_error);
    jb_int(&jb, L"progress", g_progress);
    if (g_have_update) {
        jb_string(&jb, L"version", g_update.version);
        jb_string(&jb, L"tag", g_update.tag);
        jb_string(&jb, L"releaseUrl", g_update.release_url);
        jb_bool(&jb, L"prerelease", g_update.prerelease);
        jb_int(&jb, L"sizeMb", (int)(g_update.size / (1024 * 1024)) + 1);
    }
    jb_array_begin(&jb, L"blockers");
    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!vm_blocks_update(v) || written >= 32) continue;
        jb_append(&jb, written++ ? L",\"" : L"\"");
        jb_append_escaped(&jb, v->name);
        jb_append(&jb, L"\"");
    }
    if (appliance && appliance->building_vhdx) {
        jb_append(&jb, written++ ? L",\"Shared appliance\"" : L"\"Shared appliance\"");
    }
    jb_array_end(&jb);
    jb_object_end(&jb);
    webview2_post(buf);
}

static void set_message(const wchar_t *text, BOOL is_error)
{
    wcscpy_s(g_message, _countof(g_message), text ? text : L"");
    g_message_is_error = is_error;
}

/* ---- Workers ---- */

typedef struct {
    BOOL manual;
    BOOL include_prerelease;
} CheckArgs;

static DWORD WINAPI check_thread(LPVOID param)
{
    CheckArgs args = *(CheckArgs *)param;
    CheckResult *r = (CheckResult *)calloc(1, sizeof(CheckResult));

    free(param);
    if (!r) return 0;
    r->manual = args.manual;
    r->result = updater_check(&g_endpoint, updater_current_version(), args.include_prerelease,
                              &r->info, r->err, _countof(r->err));
    if (!PostMessageW(g_hwnd, WM_UPDATE_EVENT, EVT_CHECK_DONE, (LPARAM)r)) free(r);
    return 0;
}

static void on_download_progress(int percent, void *ctx)
{
    (void)ctx;
    PostMessageW(g_hwnd, WM_UPDATE_EVENT, EVT_DOWNLOAD_PROGRESS, (LPARAM)percent);
}

static DWORD WINAPI download_thread(LPVOID param)
{
    UpdateInfo *info = (UpdateInfo *)param;
    DownloadResult *r = (DownloadResult *)calloc(1, sizeof(DownloadResult));
    wchar_t dir[MAX_PATH];

    if (!r) { free(info); return 0; }
    if (!updater_updates_dir(dir, _countof(dir))) {
        wcscpy_s(r->err, _countof(r->err), L"Could not prepare the updates folder.");
    } else {
        swprintf_s(r->path, _countof(r->path), L"%s\\%s", dir, info->asset_name);
        if (updater_download(info, r->path, on_download_progress, NULL, &g_cancel, r->err, _countof(r->err))) {
            r->ok = updater_verify_installer(r->path, info->sha256, info->version,
                                             g_pins, (int)_countof(g_pins), &r->held,
                                             r->err, _countof(r->err));
            if (!r->ok) DeleteFileW(r->path);
        }
    }
    free(info);
    if (!PostMessageW(g_hwnd, WM_UPDATE_EVENT, EVT_DOWNLOAD_DONE, (LPARAM)r)) {
        if (r->held) CloseHandle(r->held);
        free(r);
    }
    return 0;
}

static BOOL start_worker(LPTHREAD_START_ROUTINE fn, void *arg)
{
    HANDLE thread = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

/* ---- Flow ---- */

static void start_check(BOOL manual)
{
    CheckArgs *args;

    if (g_phase != PHASE_IDLE) return;
    args = (CheckArgs *)malloc(sizeof(CheckArgs));
    if (!args) return;
    args->manual = manual;
    args->include_prerelease = g_include_prerelease;
    g_phase = PHASE_CHECKING;
    if (manual) set_message(L"", FALSE);
    if (!start_worker(check_thread, args)) {
        free(args);
        g_phase = PHASE_IDLE;
    }
    post_state();
}

static void release_verified_file(BOOL delete_file)
{
    if (g_verified_handle) {
        CloseHandle(g_verified_handle);
        g_verified_handle = NULL;
    }
    if (delete_file && g_verified_path[0]) DeleteFileW(g_verified_path);
    g_verified_path[0] = L'\0';
}

/* The app closes here, so the last look at the VMs happens here too. */
static void install_if_clear(void)
{
    wchar_t err[UPDATER_ERR_CHARS];

    if (count_blockers() > 0) {
        g_phase = PHASE_WAITING_FOR_VMS;
        post_state();
        return;
    }
    err[0] = L'\0';
    if (updater_version_compare(g_update.version, updater_current_version()) <= 0 ||
        !updater_launch_installer(g_verified_path, err, _countof(err))) {
        ui_log(L"Update: could not start the installer: %s", err);
        set_message(err[0] ? err : L"This update is no longer newer than the running version.", TRUE);
        release_verified_file(TRUE);
        g_phase = PHASE_IDLE;
        post_state();
        return;
    }
    ui_log(L"Update: installing version %s; App Sandbox will reopen.", g_update.version);
    g_phase = PHASE_INSTALLING;
    post_state();
    DestroyWindow(g_hwnd);
}

static void start_install(void)
{
    UpdateInfo *copy;

    if (g_phase != PHASE_IDLE || !g_have_update || !g_installed) return;
    copy = (UpdateInfo *)malloc(sizeof(UpdateInfo));
    if (!copy) return;
    *copy = g_update;
    InterlockedExchange(&g_cancel, 0);
    g_progress = 0;
    g_phase = PHASE_DOWNLOADING;
    set_message(L"", FALSE);
    if (!start_worker(download_thread, copy)) {
        free(copy);
        g_phase = PHASE_IDLE;
    }
    post_state();
}

static void cancel_install(void)
{
    if (g_phase == PHASE_DOWNLOADING) {
        InterlockedExchange(&g_cancel, 1);
    } else if (g_phase == PHASE_WAITING_FOR_VMS) {
        release_verified_file(TRUE);
        g_phase = PHASE_IDLE;
        post_state();
    }
}

/* ---- Entry points ---- */

void update_ui_on_ready(HWND hwnd)
{
    static BOOL started = FALSE;

    if (!started) {
        started = TRUE;
        g_hwnd = hwnd;
        g_installed = updater_is_installed();
        updater_load_prefs(&g_auto_check, &g_include_prerelease);
        updater_clean_updates_dir();
        SetTimer(hwnd, ASB_UPDATE_TIMER_ID, UPDATE_FIRST_CHECK_MS, NULL);
    }
    post_state();
}

void update_ui_on_timer(void)
{
    SetTimer(g_hwnd, ASB_UPDATE_TIMER_ID, UPDATE_RECHECK_MS, NULL);
    if (g_auto_check) start_check(FALSE);
}

void update_ui_on_vm_state(void)
{
    if (g_phase == PHASE_WAITING_FOR_VMS) install_if_clear();
    else if (g_have_update) post_state();
}

void update_ui_on_event(WPARAM wp, LPARAM lp)
{
    if (wp == EVT_CHECK_DONE) {
        CheckResult *r = (CheckResult *)lp;
        g_phase = PHASE_IDLE;
        if (r->result == UPDATE_CHECK_AVAILABLE) {
            g_update = r->info;
            g_have_update = TRUE;
            set_message(L"", FALSE);
            ui_log(L"Update: version %s is available (running %s).", g_update.version, updater_current_version());
        } else if (r->result == UPDATE_CHECK_NONE) {
            g_have_update = FALSE;
            set_message(r->manual ? L"App Sandbox is up to date." : L"", FALSE);
        } else {
            /* A background check failing is not worth interrupting anyone for. */
            ui_log(L"Update check failed: %s", r->err);
            if (r->manual) set_message(r->err, TRUE);
        }
        free(r);
        post_state();
    } else if (wp == EVT_DOWNLOAD_PROGRESS) {
        g_progress = (int)lp;
        post_state();
    } else if (wp == EVT_DOWNLOAD_DONE) {
        DownloadResult *r = (DownloadResult *)lp;
        if (r->ok) {
            ui_log(L"Update: version %s downloaded and verified.", g_update.version);
            wcscpy_s(g_verified_path, _countof(g_verified_path), r->path);
            g_verified_handle = r->held;
            free(r);
            install_if_clear();
        } else {
            ui_log(L"Update refused: %s", r->err);
            set_message(r->err, TRUE);
            g_phase = PHASE_IDLE;
            free(r);
            post_state();
        }
    }
}

BOOL update_ui_handle_action(const wchar_t *action, const wchar_t *json)
{
    if (wcscmp(action, L"updateCheck") == 0) {
        start_check(TRUE);
    } else if (wcscmp(action, L"updateInstall") == 0) {
        start_install();
    } else if (wcscmp(action, L"updateCancel") == 0) {
        cancel_install();
    } else if (wcscmp(action, L"updateSetPrefs") == 0) {
        BOOL include_before = g_include_prerelease;
        json_get_bool(json, L"autoCheck", &g_auto_check);
        json_get_bool(json, L"includePrereleases", &g_include_prerelease);
        updater_save_prefs(g_auto_check, g_include_prerelease);
        post_state();
        if (include_before != g_include_prerelease) start_check(FALSE);
    } else if (wcscmp(action, L"updateOpenRelease") == 0) {
        /* Only ever a URL that passed the release-page prefix check. */
        if (g_have_update && g_update.release_url[0])
            ShellExecuteW(g_hwnd, L"open", g_update.release_url, NULL, NULL, SW_SHOWNORMAL);
    } else {
        return FALSE;
    }
    return TRUE;
}
