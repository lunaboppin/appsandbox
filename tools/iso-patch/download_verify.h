/* download_verify.h - fetch a file over HTTPS and check it against a known SHA-256. */
#ifndef DOWNLOAD_VERIFY_H
#define DOWNLOAD_VERIFY_H

#include <wchar.h>

/* Tries each URL in turn until one yields a file matching sha256 (64 hex chars),
 * which ends up at out_path. Reports PROGRESS:/ERROR:/DONE: like the rest of
 * iso-patch. Returns 0 on success. */
int do_download_verify(const wchar_t *const *urls, int url_count,
                       const wchar_t *sha256, const wchar_t *out_path, const wchar_t *label);

#endif
