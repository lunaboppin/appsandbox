/*
 * apt_verify.h - check an Ubuntu archive's signed InRelease, the way apt does.
 *
 * The package lists and every .deb hash chain back to InRelease, which is
 * signed with the Ubuntu archive key compiled in here. Without this, whoever
 * can change the download (a proxy, rogue Wi-Fi) chooses the packages.
 */
#pragma once
#include <stddef.h>

/* Verifies a clearsigned InRelease against the Ubuntu archive key. On success,
 * returns 0 and points *text / *text_len at the signed text inside data (no copy). */
int apt_inrelease_verify(const char *data, size_t len, const char **text, size_t *text_len);

/* Finds path (e.g. "main/binary-amd64/Packages.xz") in the signed text's SHA256
 * section. Returns 0 with the lowercase hex hash (65 bytes) and the size. */
int apt_release_sha256(const char *text, size_t text_len, const char *path,
                       char hex_out[65], unsigned long long *size_out);

/* A Packages "Filename:" a mirror may name: pool/... with plain path segments only. */
int apt_filename_ok(const char *name, size_t len);
