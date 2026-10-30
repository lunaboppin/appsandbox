/*
 * lab_recorder.c -- Malware lab recorder (see lab_recorder.h).
 *
 *   Files      NTFS change journal of C: from the USN at lab_start: every
 *              create, data change, rename and delete, merged per file.
 *   Registry   hashed snapshot of every key and value under the watched
 *              roots at start and stop; the diff gives added, changed and
 *              deleted keys and values, with the current data of the first two.
 *   Processes  polled every 500 ms: new ones with parent, image path and
 *              command line, and when they exit.
 *   Network    TCP and UDP endpoints polled with the owning process.
 *
 * Each file and registry entry is flagged "noise" when it is Windows' own
 * background churn (or this agent's), and registry entries in autostart
 * locations are flagged "persistence", so the report can lead with what the
 * sample did.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "lab_recorder.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#define LAB_DIR        L"C:\\ProgramData\\AppSandbox\\lab"
#define LAB_REPORT     LAB_DIR L"\\report.json"
#define LAB_MAX_PROCS  4096
#define LAB_MAX_CONNS  4096
#define LAB_MAX_FILES  30000
#define LAB_MAX_REG    30000
#define LAB_FILE_SLOTS 65536   /* power of two, > LAB_MAX_FILES */

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static LabLogFn g_log;
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static UINT64 fnv64(const void *data, size_t len, UINT64 h)
{
    const BYTE *p = (const BYTE *)data;
    size_t i;
    for (i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
#define FNV_INIT 14695981039346656037ull

/* Growable UTF-8 buffer for the JSON report. */
typedef struct { char *p; size_t n, cap; } Buf;

static void buf_add(Buf *b, const char *s, size_t len)
{
    if (b->n + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        char *np;
        while (cap < b->n + len + 1) cap *= 2;
        np = (char *)realloc(b->p, cap);
        if (!np) return;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}
static void buf_s(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_f(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(tmp);
    buf_add(b, tmp, (size_t)n);
}
/* JSON string from UTF-16, at most max_chars of it. */
static void buf_jw(Buf *b, const wchar_t *w, size_t max_chars)
{
    char out[8];
    size_t i;
    buf_s(b, "\"");
    for (i = 0; w && w[i] && i < max_chars; i++) {
        wchar_t c = w[i];
        if (c == L'"' || c == L'\\') { out[0] = '\\'; out[1] = (char)c; buf_add(b, out, 2); }
        else if (c < 0x20) { sprintf_s(out, sizeof(out), "\\u%04x", (unsigned)c); buf_s(b, out); }
        else if (c < 0x80) { out[0] = (char)c; buf_add(b, out, 1); }
        else {
            /* UTF-8 (surrogate pairs pass through as two units; fine for a report) */
            char u[4];
            int k = WideCharToMultiByte(CP_UTF8, 0, &w[i], 1, u, sizeof(u), NULL, NULL);
            if (k > 0) buf_add(b, u, (size_t)k);
        }
    }
    buf_s(b, "\"");
}

static BOOL wcontains_i(const wchar_t *hay, const wchar_t *needle)
{
    size_t n = wcslen(needle), i;
    for (i = 0; hay[i]; i++)
        if (_wcsnicmp(hay + i, needle, n) == 0) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    DWORD pid, ppid;
    ULONGLONG created;       /* FILETIME as integer, tells reused PIDs apart */
    ULONGLONG start_ms, end_ms;
    BOOL existing, ended, seen;
    wchar_t image[MAX_PATH];
    wchar_t *cmd;
} LabProc;

typedef struct {
    BOOL udp;
    DWORD laddr, raddr, state, pid;
    WORD lport, rport;
    ULONGLONG ms;
} LabConn;

typedef struct { UINT64 id, val; UINT32 str; } RegEnt;
typedef struct {
    RegEnt *e; size_t n, cap;
    wchar_t *pool; size_t pool_n, pool_cap;
} RegSnap;

typedef struct {
    DWORDLONG frn, parent;
    DWORD reasons, attributes;
    ULONGLONG first_ms, last_ms;
    wchar_t name[128];
    BOOL used;
} FileEnt;

static struct {
    CRITICAL_SECTION cs;
    BOOL cs_ready;
    volatile LONG running;
    ULONGLONG t0;              /* GetTickCount64 at start */
    LARGE_INTEGER t0_time;     /* system time at start (FILETIME units) */
    SYSTEMTIME started;
    HANDLE thread, stop;
    LabProc *procs; int nprocs;
    LabConn *conns; int nconns;
    DWORDLONG journal; USN usn0;
    RegSnap reg0;
    char *report; size_t report_len;
} g;

void lab_init(LabLogFn log)
{
    g_log = log;
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = TRUE; }
}

static ULONGLONG now_ms(void) { return GetTickCount64() - g.t0; }

/* ------------------------------------------------------------------ */
/* Processes and network                                                */
/* ------------------------------------------------------------------ */

typedef LONG (WINAPI *NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static wchar_t *process_cmdline(HANDLE h)
{
    static NtQIP query;
    BYTE buf[8192];
    ULONG got = 0;
    if (!query) query = (NtQIP)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    /* 60 = ProcessCommandLineInformation: a UNICODE_STRING then its text. */
    if (query && query(h, 60, buf, sizeof(buf), &got) == 0) {
        USHORT len = *(USHORT *)buf;
        wchar_t *text = *(wchar_t **)(buf + sizeof(void *));
        wchar_t *out = (wchar_t *)calloc(len / 2 + 1, sizeof(wchar_t));
        if (out && text && (BYTE *)text >= buf && (BYTE *)text + len <= buf + sizeof(buf))
            memcpy(out, text, len);
        return out;
    }
    return NULL;
}

static void poll_processes(BOOL baseline)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    int i;
    if (snap == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g.cs);
    for (i = 0; i < g.nprocs; i++) g.procs[i].seen = FALSE;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) do {
        ULONGLONG created = 0;
        FILETIME c, e, k, u;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        int found = -1;
        if (h && GetProcessTimes(h, &c, &e, &k, &u))
            created = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
        for (i = 0; i < g.nprocs; i++)
            if (g.procs[i].pid == pe.th32ProcessID && !g.procs[i].ended &&
                (g.procs[i].created == created || !created || !g.procs[i].created)) { found = i; break; }
        if (found >= 0) {
            g.procs[found].seen = TRUE;
        } else if (g.nprocs < LAB_MAX_PROCS) {
            LabProc *p = &g.procs[g.nprocs++];
            DWORD len = MAX_PATH;
            ZeroMemory(p, sizeof(*p));
            p->pid = pe.th32ProcessID;
            p->ppid = pe.th32ParentProcessID;
            p->created = created;
            p->existing = baseline;
            p->seen = TRUE;
            p->start_ms = baseline ? 0 : now_ms();
            if (!(h && QueryFullProcessImageNameW(h, 0, p->image, &len)))
                wcsncpy_s(p->image, MAX_PATH, pe.szExeFile, _TRUNCATE);
            if (h && !baseline) p->cmd = process_cmdline(h);
        }
        if (h) CloseHandle(h);
    } while (Process32NextW(snap, &pe));
    for (i = 0; i < g.nprocs; i++)
        if (!g.procs[i].seen && !g.procs[i].ended) { g.procs[i].ended = TRUE; g.procs[i].end_ms = now_ms(); }
    LeaveCriticalSection(&g.cs);
    CloseHandle(snap);
}

static void add_conn(BOOL udp, DWORD la, WORD lp, DWORD ra, WORD rp, DWORD state, DWORD pid)
{
    int i;
    for (i = 0; i < g.nconns; i++) {
        LabConn *c = &g.conns[i];
        if (c->udp == udp && c->laddr == la && c->lport == lp && c->raddr == ra &&
            c->rport == rp && c->pid == pid) { c->state = state; return; }
    }
    if (g.nconns < LAB_MAX_CONNS) {
        LabConn *c = &g.conns[g.nconns++];
        c->udp = udp; c->laddr = la; c->lport = lp; c->raddr = ra; c->rport = rp;
        c->state = state; c->pid = pid; c->ms = now_ms();
    }
}

static void poll_network(void)
{
    DWORD size = 0, i;
    MIB_TCPTABLE_OWNER_PID *tcp;
    MIB_UDPTABLE_OWNER_PID *udp;
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    tcp = (MIB_TCPTABLE_OWNER_PID *)malloc(size + 4096);
    if (tcp && GetExtendedTcpTable(tcp, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        EnterCriticalSection(&g.cs);
        for (i = 0; i < tcp->dwNumEntries; i++) {
            MIB_TCPROW_OWNER_PID *r = &tcp->table[i];
            if (r->dwState == MIB_TCP_STATE_LISTEN && r->dwRemoteAddr == 0) {
                add_conn(FALSE, r->dwLocalAddr, ntohs((u_short)r->dwLocalPort), 0, 0, r->dwState, r->dwOwningPid);
                continue;
            }
            add_conn(FALSE, r->dwLocalAddr, ntohs((u_short)r->dwLocalPort),
                     r->dwRemoteAddr, ntohs((u_short)r->dwRemotePort), r->dwState, r->dwOwningPid);
        }
        LeaveCriticalSection(&g.cs);
    }
    free(tcp);
    size = 0;
    GetExtendedUdpTable(NULL, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    udp = (MIB_UDPTABLE_OWNER_PID *)malloc(size + 4096);
    if (udp && GetExtendedUdpTable(udp, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
        EnterCriticalSection(&g.cs);
        for (i = 0; i < udp->dwNumEntries; i++)
            add_conn(TRUE, udp->table[i].dwLocalAddr, ntohs((u_short)udp->table[i].dwLocalPort),
                     0, 0, 0, udp->table[i].dwOwningPid);
        LeaveCriticalSection(&g.cs);
    }
    free(udp);
}

static DWORD WINAPI monitor_thread(LPVOID unused)
{
    (void)unused;
    do {
        poll_processes(FALSE);
        poll_network();
    } while (WaitForSingleObject(g.stop, 500) == WAIT_TIMEOUT);
    poll_processes(FALSE);
    poll_network();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry snapshot                                                    */
/* ------------------------------------------------------------------ */

static const struct { HKEY root; const wchar_t *name; const wchar_t *sub; } REG_ROOTS[] = {
    { HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE" },
    { HKEY_LOCAL_MACHINE, L"HKLM", L"SYSTEM\\CurrentControlSet\\Services" },
    { HKEY_LOCAL_MACHINE, L"HKLM", L"SYSTEM\\CurrentControlSet\\Control\\Session Manager" },
    { HKEY_USERS,         L"HKU",  L"" },
};

static wchar_t g_vname[16384];
static BYTE g_vdata[65536];

static void snap_add(RegSnap *s, const wchar_t *path, const wchar_t *name, UINT64 val)
{
    size_t pl = wcslen(path), nl = wcslen(name);
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        RegEnt *ne = (RegEnt *)realloc(s->e, cap * sizeof(RegEnt));
        if (!ne) return;
        s->e = ne;
        s->cap = cap;
    }
    if (s->pool_n + pl + nl + 2 > s->pool_cap) {
        size_t cap = s->pool_cap ? s->pool_cap * 2 : (1u << 20);
        wchar_t *np;
        while (cap < s->pool_n + pl + nl + 2) cap *= 2;
        np = (wchar_t *)realloc(s->pool, cap * sizeof(wchar_t));
        if (!np) return;
        s->pool = np;
        s->pool_cap = cap;
    }
    s->e[s->n].id = fnv64(name, nl * 2, fnv64(L"\\", 2, fnv64(path, pl * 2, FNV_INIT)));
    s->e[s->n].val = val;
    s->e[s->n].str = (UINT32)s->pool_n;
    memcpy(s->pool + s->pool_n, path, (pl + 1) * sizeof(wchar_t));
    memcpy(s->pool + s->pool_n + pl + 1, name, (nl + 1) * sizeof(wchar_t));
    s->pool_n += pl + nl + 2;
    s->n++;
}

/* Key marker value name: a key's own creation or deletion. */
#define KEY_MARK L"\x01"

static void snap_walk(RegSnap *s, HKEY root, const wchar_t *path, const wchar_t *sub, int depth)
{
    HKEY k;
    DWORD i;
    wchar_t child[256];
    if (depth > 48) return;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return;
    snap_add(s, path, KEY_MARK, 0);
    for (i = 0; ; i++) {
        DWORD nlen = _countof(g_vname), dlen = sizeof(g_vdata), type = 0;
        LONG rc = RegEnumValueW(k, i, g_vname, &nlen, NULL, &type, g_vdata, &dlen);
        UINT64 v;
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc == ERROR_MORE_DATA) {
            nlen = _countof(g_vname); dlen = 0;
            if (RegEnumValueW(k, i, g_vname, &nlen, NULL, &type, NULL, &dlen) != ERROR_SUCCESS) continue;
            v = fnv64(&dlen, sizeof(dlen), fnv64(&type, sizeof(type), FNV_INIT));
        } else if (rc != ERROR_SUCCESS) {
            continue;
        } else {
            v = fnv64(g_vdata, dlen, fnv64(&type, sizeof(type), FNV_INIT));
        }
        g_vname[nlen < _countof(g_vname) ? nlen : _countof(g_vname) - 1] = L'\0';
        snap_add(s, path, g_vname, v | 1);   /* never 0: 0 marks a key */
    }
    for (i = 0; ; i++) {
        DWORD clen = _countof(child);
        wchar_t *csub, *cpath;
        size_t sl, pl;
        LONG rc = RegEnumKeyExW(k, i, child, &clen, NULL, NULL, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        sl = wcslen(sub) + clen + 2;
        pl = wcslen(path) + clen + 2;
        csub = (wchar_t *)malloc(sl * sizeof(wchar_t));
        cpath = (wchar_t *)malloc(pl * sizeof(wchar_t));
        if (csub && cpath) {
            swprintf_s(csub, sl, sub[0] ? L"%s\\%s" : L"%s%s", sub, child);
            swprintf_s(cpath, pl, L"%s\\%s", path, child);
            snap_walk(s, root, cpath, csub, depth + 1);
        }
        free(csub);
        free(cpath);
    }
    RegCloseKey(k);
}

static int cmp_ent(const void *a, const void *b)
{
    UINT64 x = ((const RegEnt *)a)->id, y = ((const RegEnt *)b)->id;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void snap_take(RegSnap *s)
{
    size_t r;
    for (r = 0; r < _countof(REG_ROOTS); r++) {
        wchar_t path[512];
        swprintf_s(path, _countof(path), REG_ROOTS[r].sub[0] ? L"%s\\%s" : L"%s%s",
                   REG_ROOTS[r].name, REG_ROOTS[r].sub);
        snap_walk(s, REG_ROOTS[r].root, path, REG_ROOTS[r].sub, 0);
    }
    if (s->n) qsort(s->e, s->n, sizeof(RegEnt), cmp_ent);
}

static void snap_free(RegSnap *s)
{
    free(s->e);
    free(s->pool);
    ZeroMemory(s, sizeof(*s));
}

static BOOL reg_noise(const wchar_t *path)
{
    static const wchar_t *noise[] = {
        L"\\Explorer\\", L"\\Services\\bam\\", L"\\Perflib", L"\\Windows Defender",
        L"\\Tracing\\", L"\\WINEVT\\", L"\\Notifications\\", L"SoftwareProtectionPlatform",
        L"\\DeliveryOptimization", L"\\UserAssist\\", L"\\CloudStore\\", L"\\WindowsUpdate\\",
        L"\\Diagnostics\\", L"\\Autologger\\", L"\\Microsoft\\Cryptography\\RNG",
        L"\\Windows\\CurrentVersion\\Shell Extensions\\", L"\\SystemCertificates\\",
        L"\\AppSandbox", L"\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\Microsoft",
        L"\\Internet Settings\\", L"\\Microsoft\\Windows\\CurrentVersion\\Store",
        L"\\Session Manager\\Memory Management", L"\\W32Time\\", L"\\Microsoft\\SQMClient",
        L"\\Microsoft\\Windows\\CurrentVersion\\Search", L"\\Microsoft\\Office\\",
    };
    size_t i;
    for (i = 0; i < _countof(noise); i++) if (wcontains_i(path, noise[i])) return TRUE;
    return FALSE;
}

static BOOL reg_persistence(const wchar_t *path)
{
    static const wchar_t *places[] = {
        L"\\CurrentVersion\\Run", L"\\CurrentVersion\\RunOnce", L"\\CurrentVersion\\Policies\\Explorer\\Run",
        L"\\Windows NT\\CurrentVersion\\Winlogon", L"\\Image File Execution Options\\",
        L"\\CurrentControlSet\\Services\\", L"\\Schedule\\TaskCache\\", L"\\Windows NT\\CurrentVersion\\Windows",
        L"\\Session Manager\\BootExecute", L"\\Classes\\exefile\\", L"\\Environment",
        L"\\Active Setup\\Installed Components\\", L"\\ShellServiceObjectDelayLoad",
        L"\\Explorer\\Shell Folders", L"\\Browser Helper Objects\\", L"\\AppInit_DLLs",
    };
    size_t i;
    for (i = 0; i < _countof(places); i++) if (wcontains_i(path, places[i])) return TRUE;
    return FALSE;
}

/* "HKLM\..." -> root + subkey for re-reading a value. */
static HKEY split_root(const wchar_t *path, const wchar_t **sub)
{
    if (_wcsnicmp(path, L"HKLM\\", 5) == 0) { *sub = path + 5; return HKEY_LOCAL_MACHINE; }
    if (_wcsnicmp(path, L"HKU\\", 4) == 0) { *sub = path + 4; return HKEY_USERS; }
    *sub = L"";
    return NULL;
}

static void value_text(const wchar_t *path, const wchar_t *name, Buf *b)
{
    const wchar_t *sub;
    HKEY root = split_root(path, &sub), k;
    DWORD type = 0, len = sizeof(g_vdata), i;
    char tmp[64];
    if (!root || RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) {
        buf_s(b, ",\"type\":\"\",\"data\":\"\"");
        return;
    }
    if (RegQueryValueExW(k, name, NULL, &type, g_vdata, &len) != ERROR_SUCCESS) len = 0;
    RegCloseKey(k);
    switch (type) {
    case REG_SZ: case REG_EXPAND_SZ: case REG_MULTI_SZ: {
        wchar_t *w = (wchar_t *)g_vdata;
        DWORD chars = len / 2;
        if (chars >= _countof(g_vname)) chars = _countof(g_vname) - 1;
        for (i = 0; i < chars; i++) g_vname[i] = w[i] ? w[i] : L' ';
        g_vname[chars] = L'\0';
        buf_s(b, type == REG_SZ ? ",\"type\":\"REG_SZ\",\"data\":"
                 : type == REG_EXPAND_SZ ? ",\"type\":\"REG_EXPAND_SZ\",\"data\":"
                 : ",\"type\":\"REG_MULTI_SZ\",\"data\":");
        buf_jw(b, g_vname, 400);
        break;
    }
    case REG_DWORD:
        buf_f(b, ",\"type\":\"REG_DWORD\",\"data\":\"0x%08lx (%lu)\"",
              len >= 4 ? *(DWORD *)g_vdata : 0, len >= 4 ? *(DWORD *)g_vdata : 0);
        break;
    case REG_QWORD:
        buf_f(b, ",\"type\":\"REG_QWORD\",\"data\":\"0x%016llx\"", len >= 8 ? *(ULONGLONG *)g_vdata : 0);
        break;
    default:
        buf_f(b, ",\"type\":\"%s\",\"data\":\"", type == REG_BINARY ? "REG_BINARY" : "REG_OTHER");
        for (i = 0; i < len && i < 48; i++) { sprintf_s(tmp, sizeof(tmp), "%02x ", g_vdata[i]); buf_s(b, tmp); }
        if (len > 48) buf_f(b, "... (%lu bytes)", len);
        buf_s(b, "\"");
    }
}

static void reg_entry(Buf *b, BOOL *first, const RegSnap *s, const RegEnt *e,
                      const char *action, BOOL current)
{
    const wchar_t *path = s->pool + e->str;
    const wchar_t *name = path + wcslen(path) + 1;
    BOOL key = wcscmp(name, KEY_MARK) == 0;
    buf_s(b, *first ? "\n" : ",\n");
    *first = FALSE;
    buf_s(b, "{\"path\":");
    buf_jw(b, path, 1024);
    buf_s(b, ",\"value\":");
    buf_jw(b, key ? L"" : name, 512);
    buf_f(b, ",\"kind\":\"%s\",\"action\":\"%s\",\"noise\":%s,\"persistence\":%s",
          key ? "key" : "value", action, reg_noise(path) ? "true" : "false",
          reg_persistence(path) ? "true" : "false");
    if (current && !key) value_text(path, name, b);
    buf_s(b, "}");
}

static int reg_diff(Buf *b, const RegSnap *a, const RegSnap *z)
{
    size_t i = 0, j = 0;
    int count = 0;
    BOOL first = TRUE;
    while ((i < a->n || j < z->n) && count < LAB_MAX_REG) {
        if (j >= z->n || (i < a->n && a->e[i].id < z->e[j].id)) {
            reg_entry(b, &first, a, &a->e[i], "deleted", FALSE); i++; count++;
        } else if (i >= a->n || z->e[j].id < a->e[i].id) {
            reg_entry(b, &first, z, &z->e[j], "added", TRUE); j++; count++;
        } else {
            if (a->e[i].val != z->e[j].val) { reg_entry(b, &first, z, &z->e[j], "changed", TRUE); count++; }
            i++; j++;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Files (NTFS change journal)                                          */
/* ------------------------------------------------------------------ */

static BOOL file_noise(const wchar_t *path)
{
    static const wchar_t *noise[] = {
        L"\\Windows\\Prefetch\\", L"\\Windows\\Logs\\", L"\\System32\\winevt\\", L"\\Windows\\SoftwareDistribution\\",
        L"\\ProgramData\\Microsoft\\Windows Defender\\", L"\\Windows\\AppSandbox\\", L"\\ProgramData\\AppSandbox\\",
        L"\\System32\\config\\", L"\\System32\\LogFiles\\", L"\\$Extend\\", L"\\System Volume Information\\",
        L"\\Windows\\ServiceProfiles\\", L"\\AppData\\Local\\Microsoft\\Windows\\Explorer\\",
        L"\\ProgramData\\Microsoft\\Diagnosis\\", L"\\Windows\\Temp\\MpCmdRun", L"\\System32\\sru\\",
        L"\\AppData\\Local\\ConnectedDevicesPlatform\\", L"\\Microsoft\\Windows\\WebCache\\",
        L"\\ProgramData\\USOPrivate\\", L"\\ProgramData\\USOShared\\", L"\\Windows\\WinSxS\\",
        L"\\AppData\\Local\\Packages\\", L"\\ntuser.dat", L"\\UsrClass.dat", L".etl", L"\\pagefile.sys",
    };
    size_t i;
    for (i = 0; i < _countof(noise); i++) if (wcontains_i(path, noise[i])) return TRUE;
    return FALSE;
}

static HANDLE open_volume(void)
{
    return CreateFileW(L"\\\\.\\C:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL journal_begin(void)
{
    HANDLE v = open_volume();
    USN_JOURNAL_DATA_V0 jd;
    DWORD got;
    BOOL ok;
    if (v == INVALID_HANDLE_VALUE) return FALSE;
    ok = DeviceIoControl(v, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &jd, sizeof(jd), &got, NULL);
    if (!ok) {
        CREATE_USN_JOURNAL_DATA cj = { 64ull << 20, 8ull << 20 };
        DeviceIoControl(v, FSCTL_CREATE_USN_JOURNAL, &cj, sizeof(cj), NULL, 0, &got, NULL);
        ok = DeviceIoControl(v, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &jd, sizeof(jd), &got, NULL);
    }
    if (ok) { g.journal = jd.UsnJournalID; g.usn0 = jd.NextUsn; }
    CloseHandle(v);
    return ok;
}

static void dir_path(HANDLE v, DWORDLONG frn, wchar_t *out, size_t chars)
{
    FILE_ID_DESCRIPTOR id;
    HANDLE h;
    out[0] = L'\0';
    ZeroMemory(&id, sizeof(id));
    id.dwSize = sizeof(id);
    id.Type = FileIdType;
    id.FileId.QuadPart = (LONGLONG)frn;
    h = OpenFileById(v, &id, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL, FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) return;
    if (GetFinalPathNameByHandleW(h, out, (DWORD)chars, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) {
        if (wcsncmp(out, L"\\\\?\\", 4) == 0) memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof(wchar_t));
    } else {
        out[0] = L'\0';
    }
    CloseHandle(h);
}

static int journal_collect(Buf *b, BOOL *truncated)
{
    HANDLE v = open_volume();
    USN_JOURNAL_DATA_V0 jd;
    READ_USN_JOURNAL_DATA_V0 rd;
    FileEnt *files;
    BYTE *buf;
    DWORD got;
    USN stop;
    int used = 0, count = 0;
    size_t i;
    BOOL first = TRUE;
    *truncated = FALSE;
    if (v == INVALID_HANDLE_VALUE) return 0;
    if (!DeviceIoControl(v, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &jd, sizeof(jd), &got, NULL) ||
        jd.UsnJournalID != g.journal) { CloseHandle(v); return 0; }
    stop = jd.NextUsn;
    files = (FileEnt *)calloc(LAB_FILE_SLOTS, sizeof(FileEnt));
    buf = (BYTE *)malloc(1 << 16);
    if (!files || !buf) { free(files); free(buf); CloseHandle(v); return 0; }

    ZeroMemory(&rd, sizeof(rd));
    rd.StartUsn = g.usn0;
    rd.ReasonMask = 0xFFFFFFFF;
    rd.UsnJournalID = g.journal;
    while (rd.StartUsn < stop &&
           DeviceIoControl(v, FSCTL_READ_USN_JOURNAL, &rd, sizeof(rd), buf, 1 << 16, &got, NULL) &&
           got > sizeof(USN)) {
        DWORD off = sizeof(USN);
        while (off < got) {
            USN_RECORD_V2 *r = (USN_RECORD_V2 *)(buf + off);
            if (r->RecordLength == 0) break;
            if (r->MajorVersion == 2 && r->Usn < stop) {
                FILETIME ft;
                ULONGLONG ms;
                size_t slot = (size_t)(r->FileReferenceNumber * 2654435761u) & (LAB_FILE_SLOTS - 1);
                ft.dwLowDateTime = r->TimeStamp.LowPart;
                ft.dwHighDateTime = (DWORD)r->TimeStamp.HighPart;
                ms = (ULONGLONG)(r->TimeStamp.QuadPart - g.t0_time.QuadPart) / 10000ull;
                while (files[slot].used && files[slot].frn != r->FileReferenceNumber)
                    slot = (slot + 1) & (LAB_FILE_SLOTS - 1);
                if (!files[slot].used) {
                    if (used >= LAB_MAX_FILES) { *truncated = TRUE; off += r->RecordLength; continue; }
                    files[slot].used = TRUE;
                    files[slot].frn = r->FileReferenceNumber;
                    files[slot].first_ms = ms;
                    used++;
                }
                files[slot].reasons |= r->Reason;
                files[slot].parent = r->ParentFileReferenceNumber;
                files[slot].attributes = r->FileAttributes;
                files[slot].last_ms = ms;
                {
                    size_t n = r->FileNameLength / 2;
                    if (n >= _countof(files[slot].name)) n = _countof(files[slot].name) - 1;
                    memcpy(files[slot].name, (BYTE *)r + r->FileNameOffset, n * 2);
                    files[slot].name[n] = L'\0';
                }
                (void)ft;
            }
            off += r->RecordLength;
        }
        rd.StartUsn = *(USN *)buf;
    }

    for (i = 0; i < LAB_FILE_SLOTS; i++) {
        FileEnt *f = &files[i];
        wchar_t dir[MAX_PATH * 2], full[MAX_PATH * 2 + 130];
        DWORD r;
        const char *action;
        if (!f->used) continue;
        r = f->reasons;
        if ((r & USN_REASON_FILE_CREATE) && (r & USN_REASON_FILE_DELETE)) action = "temporary";
        else if (r & USN_REASON_FILE_DELETE) action = "deleted";
        else if (r & USN_REASON_FILE_CREATE) action = "created";
        else if (r & (USN_REASON_RENAME_NEW_NAME | USN_REASON_RENAME_OLD_NAME)) action = "renamed";
        else if (r & (USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION |
                      USN_REASON_NAMED_DATA_OVERWRITE | USN_REASON_NAMED_DATA_EXTEND |
                      USN_REASON_NAMED_DATA_TRUNCATION)) action = "modified";
        else action = "attributes";
        dir_path(v, f->parent, dir, _countof(dir));
        swprintf_s(full, _countof(full), L"%s\\%s", dir[0] ? dir : L"?", f->name);
        buf_s(b, first ? "\n" : ",\n");
        first = FALSE;
        buf_s(b, "{\"path\":");
        buf_jw(b, full, 1024);
        buf_f(b, ",\"action\":\"%s\",\"dir\":%s,\"time\":%llu,\"noise\":%s}", action,
              (f->attributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false",
              f->first_ms, file_noise(full) ? "true" : "false");
        count++;
    }
    free(files);
    free(buf);
    CloseHandle(v);
    return count;
}

/* ------------------------------------------------------------------ */
/* Start / stop / fetch                                                 */
/* ------------------------------------------------------------------ */

static void reset_state(void)
{
    int i;
    for (i = 0; i < g.nprocs; i++) free(g.procs[i].cmd);
    free(g.procs); free(g.conns);
    g.procs = NULL; g.conns = NULL; g.nprocs = g.nconns = 0;
    snap_free(&g.reg0);
}

void lab_start(char *reply, size_t reply_size)
{
    FILETIME ft;
    if (InterlockedCompareExchange(&g.running, 1, 0) != 0) {
        strcpy_s(reply, reply_size, "error:an analysis is already recording");
        return;
    }
    reset_state();
    g.procs = (LabProc *)calloc(LAB_MAX_PROCS, sizeof(LabProc));
    g.conns = (LabConn *)calloc(LAB_MAX_CONNS, sizeof(LabConn));
    if (!g.procs || !g.conns) {
        InterlockedExchange(&g.running, 0);
        strcpy_s(reply, reply_size, "error:out of memory");
        return;
    }
    CreateDirectoryW(L"C:\\ProgramData\\AppSandbox", NULL);
    CreateDirectoryW(LAB_DIR, NULL);
    LOG("Lab: taking the baseline (registry, processes, network)...");
    snap_take(&g.reg0);
    g.t0 = GetTickCount64();
    GetSystemTimeAsFileTime(&ft);
    g.t0_time.LowPart = ft.dwLowDateTime;
    g.t0_time.HighPart = (LONG)ft.dwHighDateTime;
    GetSystemTime(&g.started);
    if (!journal_begin()) LOG("Lab: the NTFS change journal is unavailable; file changes won't be recorded.");
    poll_processes(TRUE);
    poll_network();
    g.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g.thread = CreateThread(NULL, 0, monitor_thread, NULL, 0, NULL);
    LOG("Lab: recording (%zu registry entries in the baseline).", g.reg0.n);
    strcpy_s(reply, reply_size, "ok");
}

static void ipv4(DWORD a, char *out, size_t n)
{
    BYTE *b = (BYTE *)&a;
    sprintf_s(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void lab_stop(char *reply, size_t reply_size)
{
    RegSnap reg1;
    Buf b = { 0 };
    SYSTEMTIME stopped;
    wchar_t computer[64];
    DWORD clen = _countof(computer);
    int i, nreg, nfiles;
    BOOL truncated = FALSE;
    FILE *f;

    if (!g.running) { strcpy_s(reply, reply_size, "error:no analysis is recording"); return; }
    SetEvent(g.stop);
    if (g.thread) { WaitForSingleObject(g.thread, 10000); CloseHandle(g.thread); g.thread = NULL; }
    CloseHandle(g.stop);
    g.stop = NULL;
    GetSystemTime(&stopped);
    LOG("Lab: building the report...");

    computer[0] = L'\0';
    GetComputerNameW(computer, &clen);
    buf_f(&b, "{\"started\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\"stopped\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\"durationMs\":%llu,\"computer\":",
          g.started.wYear, g.started.wMonth, g.started.wDay, g.started.wHour, g.started.wMinute, g.started.wSecond,
          stopped.wYear, stopped.wMonth, stopped.wDay, stopped.wHour, stopped.wMinute, stopped.wSecond, now_ms());
    buf_jw(&b, computer, 64);

    buf_s(&b, ",\n\"processes\":[");
    EnterCriticalSection(&g.cs);
    for (i = 0; i < g.nprocs; i++) {
        LabProc *p = &g.procs[i];
        buf_s(&b, i ? ",\n" : "\n");
        buf_f(&b, "{\"pid\":%lu,\"ppid\":%lu,\"start\":%llu,\"end\":%llu,\"existing\":%s,\"ended\":%s,\"image\":",
              p->pid, p->ppid, p->start_ms, p->end_ms, p->existing ? "true" : "false", p->ended ? "true" : "false");
        buf_jw(&b, p->image, MAX_PATH);
        buf_s(&b, ",\"cmd\":");
        buf_jw(&b, p->cmd ? p->cmd : L"", 2048);
        buf_s(&b, "}");
    }
    buf_s(&b, "],\n\"network\":[");
    for (i = 0; i < g.nconns; i++) {
        LabConn *c = &g.conns[i];
        char la[32], ra[48] = "";
        ipv4(c->laddr, la, sizeof(la));
        if (!c->udp && (c->raddr || c->rport)) {
            char a[32];
            ipv4(c->raddr, a, sizeof(a));
            sprintf_s(ra, sizeof(ra), "%s:%u", a, c->rport);
        }
        buf_s(&b, i ? ",\n" : "\n");
        buf_f(&b, "{\"proto\":\"%s\",\"local\":\"%s:%u\",\"remote\":\"%s\",\"state\":%lu,\"pid\":%lu,\"time\":%llu}",
              c->udp ? "UDP" : "TCP", la, c->lport, ra, c->state, c->pid, c->ms);
    }
    LeaveCriticalSection(&g.cs);

    buf_s(&b, "],\n\"files\":[");
    nfiles = journal_collect(&b, &truncated);
    buf_f(&b, "],\n\"filesTruncated\":%s,\n\"registry\":[", truncated ? "true" : "false");
    ZeroMemory(&reg1, sizeof(reg1));
    snap_take(&reg1);
    nreg = reg_diff(&b, &g.reg0, &reg1);
    buf_f(&b, "],\n\"registryTruncated\":%s}\n", nreg >= LAB_MAX_REG ? "true" : "false");
    snap_free(&reg1);

    free(g.report);
    g.report = b.p;
    g.report_len = b.n;
    if (_wfopen_s(&f, LAB_REPORT, L"wb") == 0 && f) { fwrite(b.p, 1, b.n, f); fclose(f); }
    reset_state();
    InterlockedExchange(&g.running, 0);
    LOG("Lab: report ready (%d processes, %d file and %d registry changes, %zu bytes).",
        i, nfiles, nreg, b.n);
    sprintf_s(reply, reply_size, "ok:%zu", g.report_len);
}

void lab_chunk(size_t offset, char *reply, size_t reply_size)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n, i, o = 3;
    const BYTE *p;
    if (!g.report) { strcpy_s(reply, reply_size, "error:no report"); return; }
    strcpy_s(reply, reply_size, "ok:");
    if (offset >= g.report_len) return;
    n = g.report_len - offset;
    if (n > LAB_CHUNK_BYTES) n = LAB_CHUNK_BYTES;
    p = (const BYTE *)g.report + offset;
    for (i = 0; i < n && o + 5 < reply_size; i += 3) {
        unsigned v = (unsigned)p[i] << 16;
        if (i + 1 < n) v |= (unsigned)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        reply[o++] = t[(v >> 18) & 63];
        reply[o++] = t[(v >> 12) & 63];
        reply[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        reply[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    reply[o] = '\0';
}
