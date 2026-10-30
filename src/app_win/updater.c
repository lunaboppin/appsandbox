/*
 * updater.c -- Find, download and verify App Sandbox updates from GitHub Releases.
 *
 * The app runs elevated, so whatever this file accepts gets run as administrator.
 * Two independent checks stand between a download and that:
 *
 *   1. SHA-256 against the digest GitHub reports for the asset (catches corruption
 *      and a swapped download).
 *   2. An intact Authenticode signature by a pinned certificate (update_config.h).
 *      This is the real gate: the digest comes from the same place as the file,
 *      the signing key does not. Pinning rather than chain trust also means a
 *      test-signed release, whose root no other PC trusts, can still be verified,
 *      while a valid signature from anyone else cannot pass.
 *
 * The installer's file version must also equal the advertised version, so an old
 * validly signed installer can't be replayed under a newer asset name.
 */

#include "updater.h"
#include "update_config.h"

#include <winhttp.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

#define UPD_STR2(x) L ## #x
#define UPD_STR(x)  UPD_STR2(x)
#ifndef ASB_VERSION_FULL
#define ASB_VERSION_FULL 0.0.0.0
#endif

#if defined(_M_ARM64)
#define UPD_ARCH "arm64"
#else
#define UPD_ARCH "x64"
#endif

#define UPD_MAX_LISTING_BYTES (8u * 1024 * 1024)
#define UPD_JSON_MAX_DEPTH 32

#define UPD_FAIL(...) do { if (err && err_chars) swprintf_s(err, err_chars, __VA_ARGS__); } while (0)

const wchar_t *updater_current_version(void)
{
    return UPD_STR(ASB_VERSION_FULL);
}

/* ---- Versions ---- */

BOOL updater_parse_version(const wchar_t *text, WORD out[4])
{
    int part = 0;
    const wchar_t *p = text;

    if (!p || !*p) return FALSE;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (;;) {
        DWORD value = 0;
        int digits = 0;
        while (*p >= L'0' && *p <= L'9') {
            value = value * 10 + (DWORD)(*p - L'0');
            if (value > 65535) return FALSE;
            p++;
            digits++;
        }
        if (!digits) return FALSE;
        out[part++] = (WORD)value;
        if (*p == L'\0') break;
        if (*p != L'.' || part == 4) return FALSE;
        p++;
    }
    return part >= 3;
}

int updater_version_compare(const wchar_t *a, const wchar_t *b)
{
    WORD va[4], vb[4];
    BOOL ok_a = updater_parse_version(a, va);
    BOOL ok_b = updater_parse_version(b, vb);
    int i;

    if (!ok_a || !ok_b) return ok_a ? 1 : (ok_b ? -1 : 0);
    for (i = 0; i < 4; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

/* ---- Minimal strict JSON reader (the listing is untrusted input) ---- */

typedef struct {
    const char *p;
    const char *end;
} JsonCur;

static void js_ws(JsonCur *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n'))
        c->p++;
}

static int js_hex(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Read a string. A value too long for out comes back empty, which every field we
   use treats as invalid. Non-ASCII escapes become '?': nothing we read needs them. */
static BOOL js_read_string(JsonCur *c, char *out, size_t cap)
{
    size_t n = 0;
    BOOL overflow = FALSE;

    if (c->p >= c->end || *c->p != '"') return FALSE;
    c->p++;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (cap) out[overflow ? 0 : n] = '\0';
            return TRUE;
        }
        if ((unsigned char)ch < 0x20) return FALSE;
        if (ch == '\\') {
            if (c->p >= c->end) return FALSE;
            ch = *c->p++;
            switch (ch) {
            case '"': case '\\': case '/': break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u': {
                int i, value = 0;
                if (c->end - c->p < 4) return FALSE;
                for (i = 0; i < 4; i++) {
                    int h = js_hex(*c->p++);
                    if (h < 0) return FALSE;
                    value = value * 16 + h;
                }
                ch = value < 0x80 ? (char)value : '?';
                break;
            }
            default: return FALSE;
            }
        }
        if (n + 1 < cap) out[n++] = ch;
        else overflow = TRUE;
    }
    return FALSE;
}

/* Advance to the next key of an object; *done is TRUE at the closing brace. */
static BOOL js_next_key(JsonCur *c, BOOL *first, char *key, size_t cap, BOOL *done)
{
    *done = FALSE;
    js_ws(c);
    if (c->p >= c->end) return FALSE;
    if (*c->p == '}') { c->p++; *done = TRUE; return TRUE; }
    if (!*first) {
        if (*c->p != ',') return FALSE;
        c->p++;
        js_ws(c);
    }
    *first = FALSE;
    if (!js_read_string(c, key, cap)) return FALSE;
    js_ws(c);
    if (c->p >= c->end || *c->p != ':') return FALSE;
    c->p++;
    js_ws(c);
    return TRUE;
}

/* Advance to the next element of an array; *done is TRUE at the closing bracket. */
static BOOL js_next_element(JsonCur *c, BOOL *first, BOOL *done)
{
    *done = FALSE;
    js_ws(c);
    if (c->p >= c->end) return FALSE;
    if (*c->p == ']') { c->p++; *done = TRUE; return TRUE; }
    if (!*first) {
        if (*c->p != ',') return FALSE;
        c->p++;
        js_ws(c);
    }
    *first = FALSE;
    return c->p < c->end;
}

static BOOL js_skip_literal(JsonCur *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || strncmp(c->p, lit, n) != 0) return FALSE;
    c->p += n;
    return TRUE;
}

static BOOL js_skip_value(JsonCur *c, int depth)
{
    char key[8];
    BOOL first = TRUE, done = FALSE;

    if (depth > UPD_JSON_MAX_DEPTH) return FALSE;
    js_ws(c);
    if (c->p >= c->end) return FALSE;
    switch (*c->p) {
    case '"':
        return js_read_string(c, key, sizeof(key));
    case '{':
        c->p++;
        for (;;) {
            if (!js_next_key(c, &first, key, sizeof(key), &done)) return FALSE;
            if (done) return TRUE;
            if (!js_skip_value(c, depth + 1)) return FALSE;
        }
    case '[':
        c->p++;
        for (;;) {
            if (!js_next_element(c, &first, &done)) return FALSE;
            if (done) return TRUE;
            if (!js_skip_value(c, depth + 1)) return FALSE;
        }
    case 't': return js_skip_literal(c, "true");
    case 'f': return js_skip_literal(c, "false");
    case 'n': return js_skip_literal(c, "null");
    default:
        if (*c->p != '-' && !(*c->p >= '0' && *c->p <= '9')) return FALSE;
        while (c->p < c->end && strchr("+-.eE0123456789", *c->p)) c->p++;
        return TRUE;
    }
}

static BOOL js_read_bool(JsonCur *c, BOOL *out)
{
    js_ws(c);
    if (js_skip_literal(c, "true")) { *out = TRUE; return TRUE; }
    if (js_skip_literal(c, "false")) { *out = FALSE; return TRUE; }
    return js_skip_value(c, 0);
}

static BOOL js_read_size(JsonCur *c, ULONGLONG *out)
{
    ULONGLONG value = 0;

    js_ws(c);
    if (c->p >= c->end || *c->p < '0' || *c->p > '9') return js_skip_value(c, 0);
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        if (value > UPDATER_MAX_INSTALLER_BYTES) value = UPDATER_MAX_INSTALLER_BYTES + 1;
        else value = value * 10 + (ULONGLONG)(*c->p - '0');
        c->p++;
    }
    *out = value;
    return TRUE;
}

/* A string field that may legitimately be null. */
static BOOL js_read_field(JsonCur *c, char *out, size_t cap)
{
    out[0] = '\0';
    js_ws(c);
    if (c->p < c->end && *c->p == '"') return js_read_string(c, out, cap);
    return js_skip_value(c, 0);
}

/* ---- Release selection ---- */

typedef struct {
    char      name[128];
    char      url[1024];
    char      digest[80];
    char      state[16];
    ULONGLONG size;
} ParsedAsset;

typedef struct {
    char        tag[128];
    char        html_url[512];
    BOOL        draft;
    BOOL        prerelease;
    BOOL        has_asset;
    ParsedAsset asset;
} ParsedRelease;

/* Our asset is exactly AppSandbox-<version>-win-<arch>-setup.exe. */
static BOOL asset_name_version(const char *name, char *version, size_t cap)
{
    static const char prefix[] = "AppSandbox-";
    static const char suffix[] = "-win-" UPD_ARCH "-setup.exe";
    size_t len = strlen(name), pre = sizeof(prefix) - 1, suf = sizeof(suffix) - 1, ver;

    if (len <= pre + suf || strncmp(name, prefix, pre) != 0) return FALSE;
    if (strcmp(name + len - suf, suffix) != 0) return FALSE;
    ver = len - pre - suf;
    if (ver + 1 > cap) return FALSE;
    memcpy(version, name + pre, ver);
    version[ver] = '\0';
    return TRUE;
}

static BOOL parse_asset(JsonCur *c, ParsedAsset *a)
{
    char key[32];
    BOOL first = TRUE, done = FALSE;

    ZeroMemory(a, sizeof(*a));
    js_ws(c);
    if (c->p >= c->end || *c->p != '{') return js_skip_value(c, 0);
    c->p++;
    for (;;) {
        BOOL ok;
        if (!js_next_key(c, &first, key, sizeof(key), &done)) return FALSE;
        if (done) return TRUE;
        if (strcmp(key, "name") == 0) ok = js_read_field(c, a->name, sizeof(a->name));
        else if (strcmp(key, "browser_download_url") == 0) ok = js_read_field(c, a->url, sizeof(a->url));
        else if (strcmp(key, "digest") == 0) ok = js_read_field(c, a->digest, sizeof(a->digest));
        else if (strcmp(key, "state") == 0) ok = js_read_field(c, a->state, sizeof(a->state));
        else if (strcmp(key, "size") == 0) ok = js_read_size(c, &a->size);
        else ok = js_skip_value(c, 1);
        if (!ok) return FALSE;
    }
}

static BOOL parse_release(JsonCur *c, ParsedRelease *r)
{
    char key[32];
    BOOL first = TRUE, done = FALSE;

    ZeroMemory(r, sizeof(*r));
    js_ws(c);
    if (c->p >= c->end || *c->p != '{') return FALSE;
    c->p++;
    for (;;) {
        BOOL ok;
        if (!js_next_key(c, &first, key, sizeof(key), &done)) return FALSE;
        if (done) return TRUE;
        if (strcmp(key, "tag_name") == 0) ok = js_read_field(c, r->tag, sizeof(r->tag));
        else if (strcmp(key, "html_url") == 0) ok = js_read_field(c, r->html_url, sizeof(r->html_url));
        else if (strcmp(key, "draft") == 0) ok = js_read_bool(c, &r->draft);
        else if (strcmp(key, "prerelease") == 0) ok = js_read_bool(c, &r->prerelease);
        else if (strcmp(key, "assets") == 0) {
            BOOL el_first = TRUE, el_done = FALSE;
            char ver[32];
            js_ws(c);
            if (c->p >= c->end || *c->p != '[') { ok = js_skip_value(c, 1); }
            else {
                c->p++;
                ok = TRUE;
                for (;;) {
                    ParsedAsset a;
                    if (!js_next_element(c, &el_first, &el_done)) { ok = FALSE; break; }
                    if (el_done) break;
                    if (!parse_asset(c, &a)) { ok = FALSE; break; }
                    if (!r->has_asset && asset_name_version(a.name, ver, sizeof(ver))) {
                        r->asset = a;
                        r->has_asset = TRUE;
                    }
                }
            }
        }
        else ok = js_skip_value(c, 1);
        if (!ok) return FALSE;
    }
}

static void utf8_to_wide(const char *in, wchar_t *out, size_t out_chars)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)out_chars)) out[0] = L'\0';
}

/* "sha256:<64 hex>" -> lowercase hex. */
static BOOL digest_to_hex(const char *digest, wchar_t out[65])
{
    int i;

    if (strncmp(digest, "sha256:", 7) != 0 || strlen(digest) != 7 + 64) return FALSE;
    for (i = 0; i < 64; i++) {
        char ch = digest[7 + i];
        if (js_hex(ch) < 0) return FALSE;
        out[i] = towlower((wchar_t)ch);
    }
    out[64] = L'\0';
    return TRUE;
}

static BOOL has_prefix(const wchar_t *text, const wchar_t *prefix)
{
    return prefix && prefix[0] && _wcsnicmp(text, prefix, wcslen(prefix)) == 0;
}

/* Everything an update must have to be offered at all. */
static BOOL release_to_info(const ParsedRelease *r, const UpdateEndpoint *endpoint, UpdateInfo *info)
{
    char ver[32];

    ZeroMemory(info, sizeof(*info));
    if (!r->has_asset || r->draft) return FALSE;
    if (strcmp(r->asset.state, "uploaded") != 0) return FALSE;
    if (r->asset.size == 0 || r->asset.size > UPDATER_MAX_INSTALLER_BYTES) return FALSE;
    if (!asset_name_version(r->asset.name, ver, sizeof(ver))) return FALSE;
    utf8_to_wide(ver, info->version, _countof(info->version));
    {
        WORD parts[4];
        if (!updater_parse_version(info->version, parts)) return FALSE;
    }
    if (!digest_to_hex(r->asset.digest, info->sha256)) return FALSE;
    utf8_to_wide(r->asset.url, info->asset_url, _countof(info->asset_url));
    if (!has_prefix(info->asset_url, endpoint->asset_prefix)) return FALSE;
    utf8_to_wide(r->asset.name, info->asset_name, _countof(info->asset_name));
    utf8_to_wide(r->tag, info->tag, _countof(info->tag));
    /* The page is only linked to, so a bad URL drops the link, not the update. */
    utf8_to_wide(r->html_url, info->release_url, _countof(info->release_url));
    if (!has_prefix(info->release_url, endpoint->release_prefix)) info->release_url[0] = L'\0';
    info->size = r->asset.size;
    info->prerelease = r->prerelease;
    return TRUE;
}

UpdateCheckResult updater_select_release(const char *json, size_t len,
                                         const wchar_t *current, BOOL include_prerelease,
                                         const UpdateEndpoint *endpoint,
                                         UpdateInfo *out, wchar_t *err, size_t err_chars)
{
    JsonCur c;
    BOOL first = TRUE, done = FALSE, found = FALSE;
    ParsedRelease *r = (ParsedRelease *)malloc(sizeof(ParsedRelease));
    UpdateInfo *info = (UpdateInfo *)malloc(sizeof(UpdateInfo));
    UpdateCheckResult result = UPDATE_CHECK_ERROR;

    if (!r || !info) { UPD_FAIL(L"Out of memory."); goto done; }
    c.p = json;
    c.end = json + len;
    js_ws(&c);
    if (c.p >= c.end || *c.p != '[') { UPD_FAIL(L"The release list was not understood."); goto done; }
    c.p++;
    for (;;) {
        if (!js_next_element(&c, &first, &done) || (!done && !parse_release(&c, r))) {
            UPD_FAIL(L"The release list was not understood.");
            goto done;
        }
        if (done) break;
        if (r->prerelease && !include_prerelease) continue;
        if (!release_to_info(r, endpoint, info)) continue;
        if (updater_version_compare(info->version, current) <= 0) continue;
        if (!found || updater_version_compare(info->version, out->version) > 0) {
            *out = *info;
            found = TRUE;
        }
    }
    result = found ? UPDATE_CHECK_AVAILABLE : UPDATE_CHECK_NONE;
done:
    free(r);
    free(info);
    return result;
}

/* ---- HTTP ---- */

typedef struct {
    HINTERNET session, connect, request;
} HttpReq;

static void http_close(HttpReq *h)
{
    if (h->request) WinHttpCloseHandle(h->request);
    if (h->connect) WinHttpCloseHandle(h->connect);
    if (h->session) WinHttpCloseHandle(h->session);
    ZeroMemory(h, sizeof(*h));
}

/* HTTPS only. Plain HTTP is allowed solely to 127.0.0.1, for the mock feed in tests. */
static BOOL http_get(const wchar_t *url, const wchar_t *headers, HttpReq *h, wchar_t *err, size_t err_chars)
{
    URL_COMPONENTS uc;
    wchar_t host[256], path[2048], agent[64];
    DWORD status = 0, size = sizeof(status);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    BOOL secure;

    ZeroMemory(h, sizeof(*h));
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    /* WinHttpCrackUrl leaves the query in lpszExtraInfo unless asked to keep it
       with the path: requesting no separate buffer does that. */
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { UPD_FAIL(L"Invalid update URL."); return FALSE; }
    secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(uc.nScheme == INTERNET_SCHEME_HTTP && wcscmp(host, L"127.0.0.1") == 0)) {
        UPD_FAIL(L"Refusing a non-HTTPS update URL.");
        return FALSE;
    }

    swprintf_s(agent, _countof(agent), L"AppSandbox/%s", updater_current_version());
    h->session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (h->session) h->connect = WinHttpConnect(h->session, host, uc.nPort, 0);
    if (h->connect) h->request = WinHttpOpenRequest(h->connect, L"GET", path, NULL,
                                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    secure ? WINHTTP_FLAG_SECURE : 0);
    if (!h->request) {
        UPD_FAIL(L"Could not start the request (error %lu).", GetLastError());
        http_close(h);
        return FALSE;
    }
    WinHttpSetOption(h->request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpSetTimeouts(h->request, 15000, 15000, 30000, 60000);
    if (!WinHttpSendRequest(h->request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                            headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h->request, NULL)) {
        UPD_FAIL(L"Could not reach the update server (error %lu).", GetLastError());
        http_close(h);
        return FALSE;
    }
    if (!WinHttpQueryHeaders(h->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        if (status == 403 || status == 429)
            UPD_FAIL(L"GitHub's rate limit was reached. Try again later.");
        else
            UPD_FAIL(L"The update server answered HTTP %lu.", status);
        http_close(h);
        return FALSE;
    }
    return TRUE;
}

UpdateCheckResult updater_check(const UpdateEndpoint *endpoint, const wchar_t *current,
                                BOOL include_prerelease, UpdateInfo *out,
                                wchar_t *err, size_t err_chars)
{
    HttpReq h;
    char *body;
    size_t len = 0;
    UpdateCheckResult result = UPDATE_CHECK_ERROR;

    if (!http_get(endpoint->api_url,
                  L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                  &h, err, err_chars))
        return UPDATE_CHECK_ERROR;

    body = (char *)malloc(UPD_MAX_LISTING_BYTES);
    if (!body) {
        UPD_FAIL(L"Out of memory.");
    } else {
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(h.request, body + len, (DWORD)(UPD_MAX_LISTING_BYTES - len), &got)) {
                UPD_FAIL(L"Reading the release list failed (error %lu).", GetLastError());
                goto done;
            }
            if (got == 0) break;
            len += got;
            if (len >= UPD_MAX_LISTING_BYTES) {
                UPD_FAIL(L"The release list is too large.");
                goto done;
            }
        }
        result = updater_select_release(body, len, current, include_prerelease, endpoint, out, err, err_chars);
    }
done:
    free(body);
    http_close(&h);
    return result;
}

BOOL updater_download(const UpdateInfo *info, const wchar_t *dest,
                      UpdaterProgressFn progress, void *ctx, volatile LONG *cancel,
                      wchar_t *err, size_t err_chars)
{
    HttpReq h;
    HANDLE file;
    wchar_t part[MAX_PATH];
    BYTE *buf = NULL;
    ULONGLONG total = 0;
    int last_pct = -1;
    BOOL ok = FALSE;

    if (!has_prefix(info->asset_url, L"https://") && !has_prefix(info->asset_url, L"http://127.0.0.1")) {
        UPD_FAIL(L"Refusing a non-HTTPS download.");
        return FALSE;
    }
    if (FAILED(swprintf_s(part, _countof(part), L"%s.part", dest))) return FALSE;
    if (!http_get(info->asset_url, NULL, &h, err, err_chars)) return FALSE;

    file = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        UPD_FAIL(L"Could not create the download file (error %lu).", GetLastError());
        http_close(&h);
        return FALSE;
    }
    buf = (BYTE *)malloc(256 * 1024);
    if (!buf) { UPD_FAIL(L"Out of memory."); goto done; }

    for (;;) {
        DWORD got = 0, wrote = 0;
        if (cancel && InterlockedCompareExchange(cancel, 0, 0)) {
            UPD_FAIL(L"Cancelled.");
            goto done;
        }
        if (!WinHttpReadData(h.request, buf, 256 * 1024, &got)) {
            UPD_FAIL(L"The download failed (error %lu).", GetLastError());
            goto done;
        }
        if (got == 0) break;
        total += got;
        if (total > info->size) {
            UPD_FAIL(L"The download is larger than the release says it is.");
            goto done;
        }
        if (!WriteFile(file, buf, got, &wrote, NULL) || wrote != got) {
            UPD_FAIL(L"Writing the download failed (error %lu).", GetLastError());
            goto done;
        }
        if (progress) {
            int pct = (int)(total * 100 / info->size);
            if (pct != last_pct) { progress(pct, ctx); last_pct = pct; }
        }
    }
    if (total != info->size) {
        UPD_FAIL(L"The download stopped early.");
        goto done;
    }
    ok = TRUE;
done:
    free(buf);
    CloseHandle(file);
    http_close(&h);
    if (ok && !MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING)) {
        UPD_FAIL(L"Could not move the download into place (error %lu).", GetLastError());
        ok = FALSE;
    }
    if (!ok) DeleteFileW(part);
    return ok;
}

/* ---- Verification ---- */

static BOOL sha256_of_handle(HANDLE file, wchar_t out[65])
{
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE digest[32];
    BYTE *buf = (BYTE *)malloc(256 * 1024);
    LARGE_INTEGER zero;
    BOOL ok = FALSE;
    int i;

    zero.QuadPart = 0;
    if (!buf || !SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) goto done;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0))) goto done;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buf, 256 * 1024, &got, NULL)) goto done;
        if (got == 0) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buf, got, 0))) goto done;
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) goto done;
    for (i = 0; i < 32; i++) swprintf_s(out + i * 2, 3, L"%02x", digest[i]);
    ok = TRUE;
done:
    if (hash) BCryptDestroyHash(hash);
    free(buf);
    return ok;
}

BOOL updater_read_signer(const wchar_t *path, HANDLE file, wchar_t out[65], LONG *status)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    CRYPT_PROVIDER_DATA *prov;
    CRYPT_PROVIDER_SGNR *signer;
    CRYPT_PROVIDER_CERT *cert;
    BYTE hash[32];
    DWORD hash_len = sizeof(hash);
    BOOL ok = FALSE;
    int i;

    ZeroMemory(&fi, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path;
    fi.hFile = file;
    ZeroMemory(&wd, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    *status = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);

    prov = wd.hWVTStateData ? WTHelperProvDataFromStateData(wd.hWVTStateData) : NULL;
    signer = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : NULL;
    cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : NULL;
    if (cert && cert->pCert &&
        CertGetCertificateContextProperty(cert->pCert, CERT_SHA256_HASH_PROP_ID, hash, &hash_len) &&
        hash_len == sizeof(hash)) {
        for (i = 0; i < 32; i++) swprintf_s(out + i * 2, 3, L"%02X", hash[i]);
        ok = TRUE;
    }

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return ok;
}

/* The two statuses that mean "the signature itself is good, but Windows doesn't
   trust the root": a test-signed release on any PC but the one that made it. Bad
   digests, bad signatures, no signature and explicit distrust are never in here. */
static BOOL signature_intact(LONG status)
{
    return status == ERROR_SUCCESS ||
           status == CERT_E_UNTRUSTEDROOT ||
           status == CERT_E_CHAINING;
}

static BOOL file_version_equals(const wchar_t *path, const wchar_t *version)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path, &handle);
    BYTE *data;
    VS_FIXEDFILEINFO *fixed = NULL;
    UINT fixed_len = 0;
    WORD want[4];
    BOOL ok = FALSE;

    if (!size || !updater_parse_version(version, want)) return FALSE;
    data = (BYTE *)malloc(size);
    if (data && GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\", (void **)&fixed, &fixed_len) && fixed && fixed_len >= sizeof(*fixed)) {
        ok = HIWORD(fixed->dwFileVersionMS) == want[0] && LOWORD(fixed->dwFileVersionMS) == want[1] &&
             HIWORD(fixed->dwFileVersionLS) == want[2] && LOWORD(fixed->dwFileVersionLS) == want[3];
    }
    free(data);
    return ok;
}

BOOL updater_verify_installer(const wchar_t *path, const wchar_t *expected_sha256,
                              const wchar_t *version,
                              const wchar_t *const *pins, int pin_count,
                              HANDLE *held, wchar_t *err, size_t err_chars)
{
    wchar_t actual[65], signer[65];
    LONG status = 0;
    BOOL pinned = FALSE, ok = FALSE;
    int i;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        UPD_FAIL(L"Could not open the download (error %lu).", GetLastError());
        return FALSE;
    }
    if (!expected_sha256 || wcslen(expected_sha256) != 64) {
        UPD_FAIL(L"The release gave no SHA-256 for this file.");
        goto done;
    }
    if (!sha256_of_handle(file, actual)) {
        UPD_FAIL(L"Could not hash the download.");
        goto done;
    }
    if (_wcsicmp(actual, expected_sha256) != 0) {
        UPD_FAIL(L"The download's SHA-256 does not match the release.");
        goto done;
    }
    if (!updater_read_signer(path, file, signer, &status) || !signature_intact(status)) {
        UPD_FAIL(L"The download has no valid signature (0x%08lX).", (unsigned long)status);
        goto done;
    }
    for (i = 0; i < pin_count; i++) {
        if (_wcsicmp(signer, pins[i]) == 0) pinned = TRUE;
    }
    if (!pinned) {
        UPD_FAIL(L"The download is signed by an unknown publisher (%.16s...).", signer);
        goto done;
    }
    if (!file_version_equals(path, version)) {
        UPD_FAIL(L"The installer's version is not the one the release advertises.");
        goto done;
    }
    ok = TRUE;
done:
    if (ok && held) *held = file;
    else CloseHandle(file);
    return ok;
}

/* ---- Install state ---- */

BOOL updater_install_location(wchar_t *out, size_t out_chars)
{
    wchar_t key[256];
    DWORD size = (DWORD)(out_chars * sizeof(wchar_t));

    swprintf_s(key, _countof(key),
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s_is1", ASB_INSTALLER_APP_ID);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallLocation", RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS;
}

BOOL updater_is_installed(void)
{
    wchar_t location[MAX_PATH], exe[MAX_PATH], *slash;
    size_t n;

    if (!updater_install_location(location, _countof(location))) return FALSE;
    if (!GetModuleFileNameW(NULL, exe, _countof(exe))) return FALSE;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    n = wcslen(location);
    if (n && location[n - 1] == L'\\') location[n - 1] = L'\0';
    return _wcsicmp(location, exe) == 0;
}

/* ProgramData lets ordinary users create folders, so a folder of this name may
   already exist and belong to anyone: take ownership and replace its ACL. */
#define UPD_DIR_SDDL L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"

BOOL updater_updates_dir(wchar_t *out, size_t out_chars)
{
    wchar_t base[MAX_PATH], parent[MAX_PATH];
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID owner = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE, ok = FALSE;

    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(parent, _countof(parent), L"%s\\AppSandbox", base);
    CreateDirectoryW(parent, NULL);
    swprintf_s(out, out_chars, L"%s\\updates", parent);
    if (!CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(UPD_DIR_SDDL, SDDL_REVISION_1, &sd, NULL) &&
        GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
        GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) {
        ok = SetNamedSecurityInfoW(out, SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                   PROTECTED_DACL_SECURITY_INFORMATION,
                                   owner, NULL, dacl, NULL) == ERROR_SUCCESS;
    }
    if (sd) LocalFree(sd);
    return ok;
}

void updater_clean_updates_dir(void)
{
    wchar_t dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (!updater_updates_dir(dir, _countof(dir))) return;
    swprintf_s(pattern, _countof(pattern), L"%s\\AppSandbox-*-setup.exe*", dir);
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        swprintf_s(path, _countof(path), L"%s\\%s", dir, fd.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

BOOL updater_launch_installer(const wchar_t *path, wchar_t *err, size_t err_chars)
{
    wchar_t dir[MAX_PATH], *slash;
    wchar_t cmd[MAX_PATH * 3];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    wcscpy_s(dir, _countof(dir), path);
    slash = wcsrchr(dir, L'\\');
    if (!slash) { UPD_FAIL(L"Bad installer path."); return FALSE; }
    *slash = L'\0';

    /* /SILENT shows progress but asks nothing. /ASBUPDATE=1 is the installer's cue
       to wait for this process and to reopen the app afterwards. */
    swprintf_s(cmd, _countof(cmd),
               L"\"%s\" /SILENT /SUPPRESSMSGBOXES /NORESTART /ASBUPDATE=1 /LOG=\"%s\\setup.log\"",
               path, dir);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        UPD_FAIL(L"Could not start the installer (error %lu).", GetLastError());
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* ---- Preferences ---- */

static BOOL prefs_path(wchar_t *out, size_t out_chars)
{
    wchar_t dir[MAX_PATH];
    if (!updater_updates_dir(dir, _countof(dir))) return FALSE;
    swprintf_s(out, out_chars, L"%s\\settings.cfg", dir);
    return TRUE;
}

void updater_load_prefs(BOOL *auto_check, BOOL *include_prerelease)
{
    wchar_t path[MAX_PATH];

    *auto_check = TRUE;
    *include_prerelease = FALSE;
    if (!prefs_path(path, _countof(path))) return;
    *auto_check = GetPrivateProfileIntW(L"Update", L"AutoCheck", 1, path) != 0;
    *include_prerelease = GetPrivateProfileIntW(L"Update", L"IncludePrereleases", 0, path) != 0;
}

void updater_save_prefs(BOOL auto_check, BOOL include_prerelease)
{
    wchar_t path[MAX_PATH];

    if (!prefs_path(path, _countof(path))) return;
    WritePrivateProfileStringW(L"Update", L"AutoCheck", auto_check ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"Update", L"IncludePrereleases", include_prerelease ? L"1" : L"0", path);
}
