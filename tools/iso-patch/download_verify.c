/* download_verify.c - fetch a file over HTTPS and check it against a known SHA-256.
 *
 * Writes to <out>.part and renames only after the hash matches, so a file at <out>
 * has been verified at some point. An existing <out> is hashed again rather than
 * trusted, and replaced if it no longer matches.
 */
#include "download_verify.h"
#include "iso_patch_log.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#define DV_BUF_BYTES (1024 * 1024)

/* Progress is split: the download gets 0-90 and the final hash 90-99. */
static int dv_sha256_file(const wchar_t *path, wchar_t hex[65])
{
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file;
    BYTE digest[32];
    BYTE *buf;
    DWORD got = 0;
    int rc = -1;

    buf = (BYTE *)malloc(DV_BUF_BYTES);
    if (!buf) return -1;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file != INVALID_HANDLE_VALUE &&
        BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0))) {
        BOOL ok = TRUE;
        while (ok && ReadFile(file, buf, DV_BUF_BYTES, &got, NULL) && got)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf, got, 0));
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            for (int i = 0; i < 32; i++)
                swprintf_s(hex + i * 2, 65 - i * 2, L"%02x", digest[i]);
            rc = 0;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(buf);
    return rc;
}

static int dv_fetch(const wchar_t *url, const wchar_t *out_path, const wchar_t *label)
{
    URL_COMPONENTS uc;
    wchar_t host[256], path[2048];
    HINTERNET session = NULL, conn = NULL, req = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE *buf = NULL;
    DWORD status = 0, size = sizeof(status);
    ULONGLONG total = 0, done = 0;
    wchar_t len_text[32];
    DWORD len_size = sizeof(len_text);
    DWORD last_report = 0;
    int rc = -1;

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        log_err(L"Refusing a download that isn't HTTPS: %s", url);
        return -1;
    }

    session = WinHttpOpen(L"AppSandbox-iso-patch/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    /* Slow mirrors: fail on silence, not on length. */
    WinHttpSetTimeouts(session, 30000, 30000, 60000, 120000);
    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) goto done;
    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto done;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        log_msg(L"Download failed (%lu): %s", GetLastError(), url);
        goto done;
    }
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        log_msg(L"Download failed (HTTP %lu): %s", status, url);
        goto done;
    }
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            len_text, &len_size, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(len_text, NULL, 10);

    file = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    buf = (BYTE *)malloc(DV_BUF_BYTES);
    if (file == INVALID_HANDLE_VALUE || !buf) goto done;

    for (;;) {
        DWORD got = 0, wrote = 0;
        if (!WinHttpReadData(req, buf, DV_BUF_BYTES, &got)) {
            log_msg(L"Download stopped (%lu): %s", GetLastError(), url);
            goto done;
        }
        if (got == 0) break;
        if (!WriteFile(file, buf, got, &wrote, NULL) || wrote != got) {
            log_err(L"Could not write the download (%lu): %s", GetLastError(), out_path);
            goto done;
        }
        done += got;
        if (total && GetTickCount() - last_report > 1000) {
            log_progress((int)(done * 90 / total), label);
            last_report = GetTickCount();
        }
    }
    if (total && done != total) {
        log_msg(L"Download ended early (%llu of %llu bytes): %s", done, total, url);
        goto done;
    }
    rc = 0;

done:
    free(buf);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return rc;
}

int do_download_verify(const wchar_t *const *urls, int url_count,
                       const wchar_t *sha256, const wchar_t *out_path, const wchar_t *label)
{
    wchar_t part[MAX_PATH], actual[65];
    BOOL mismatch = FALSE;

    if (!sha256 || wcslen(sha256) != 64) {
        log_err(L"--download requires a 64-character --sha256");
        return 1;
    }
    if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES) {
        log_progress(90, L"Verifying the download");
        if (dv_sha256_file(out_path, actual) == 0 && _wcsicmp(actual, sha256) == 0) {
            log_msg(L"Already downloaded and verified: %s", out_path);
            log_progress(99, L"Verified");
            log_done(out_path);
            return 0;
        }
        log_msg(L"Cached file failed verification, downloading again: %s", out_path);
        DeleteFileW(out_path);
    }

    swprintf_s(part, MAX_PATH, L"%s.part", out_path);
    for (int i = 0; i < url_count; i++) {
        if (dv_fetch(urls[i], part, label) != 0) continue;
        log_progress(90, L"Verifying the download");
        if (dv_sha256_file(part, actual) != 0) continue;
        if (_wcsicmp(actual, sha256) != 0) {
            log_msg(L"SHA-256 mismatch from %s: got %s", urls[i], actual);
            mismatch = TRUE;
            continue;
        }
        DeleteFileW(out_path);
        if (!MoveFileW(part, out_path)) {
            log_err(L"Could not move the download into place (%lu)", GetLastError());
            return 1;
        }
        log_progress(99, L"Verified");
        log_done(out_path);
        return 0;
    }
    DeleteFileW(part);
    log_err(mismatch ? L"The download doesn't match its published SHA-256."
                     : L"The download failed. Check the host's internet connection.");
    return 1;
}
