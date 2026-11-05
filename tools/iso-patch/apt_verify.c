/*
 * apt_verify.c - see apt_verify.h.
 *
 * Only what Ubuntu's InRelease uses: a clearsigned message whose signature is a
 * v4 OpenPGP signature packet, RSA, over the canonical text (type 0x01), with
 * SHA-256, SHA-384 or SHA-512. Anything else is refused.
 */
#include <windows.h>
#include <bcrypt.h>
#include <string.h>
#include <stdlib.h>
#include "apt_verify.h"

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

/* Ubuntu Archive Automatic Signing Key (2018) <ftpmaster@ubuntu.com>
   F6EC B376 2474 EDA9 D21B  7022 8719 20D1 991B C93C, RSA 4096, e = 65537.
   Checked against keyserver.ubuntu.com and the archive's own signatures. */
static const unsigned char UBUNTU_2018_FPR[20] = {
    0xf6, 0xec, 0xb3, 0x76, 0x24, 0x74, 0xed, 0xa9, 0xd2, 0x1b,
    0x70, 0x22, 0x87, 0x19, 0x20, 0xd1, 0x99, 0x1b, 0xc9, 0x3c,
};
static const unsigned char UBUNTU_2018_N[512] = {
    0xef, 0xfc, 0x6c, 0x72, 0xb7, 0x1f, 0xcb, 0x71, 0x25, 0xd8, 0xb8, 0xcd, 0x0c, 0xc0, 0xaa, 0x23,
    0x6c, 0x1c, 0x9e, 0xf3, 0x5b, 0x34, 0x1b, 0x59, 0xc4, 0xc7, 0xe9, 0x73, 0xe9, 0x50, 0x14, 0xa4,
    0x85, 0x19, 0x9d, 0xb9, 0x2a, 0x75, 0x70, 0x47, 0x0b, 0xe7, 0x70, 0xac, 0x64, 0xbf, 0x09, 0xe7,
    0x8b, 0xb8, 0x08, 0xcf, 0x44, 0xb5, 0x3c, 0x02, 0x8c, 0x44, 0xfe, 0x38, 0xef, 0x65, 0x5a, 0x7c,
    0xc4, 0x51, 0x84, 0x58, 0x76, 0x1d, 0x92, 0x5a, 0x97, 0x19, 0x9f, 0xe0, 0x25, 0xf3, 0xf9, 0x77,
    0x77, 0xc8, 0x50, 0x1b, 0x59, 0x1d, 0x91, 0x09, 0x97, 0xc0, 0x7c, 0x9b, 0xda, 0x4c, 0x1d, 0xff,
    0xc0, 0x41, 0x07, 0x6c, 0x0b, 0xe6, 0x33, 0x8b, 0x34, 0x86, 0xe6, 0xde, 0x4c, 0x86, 0x7a, 0x2d,
    0xc3, 0x4e, 0x38, 0x2d, 0x7b, 0x5d, 0x10, 0x49, 0x31, 0xda, 0xde, 0x89, 0xcf, 0x43, 0x86, 0xae,
    0x1f, 0xb9, 0x22, 0x8c, 0x6a, 0x5f, 0xba, 0x59, 0x8a, 0xae, 0x82, 0xbf, 0x5f, 0x41, 0xa2, 0x16,
    0x94, 0x8a, 0x82, 0x8c, 0x76, 0x9e, 0xc4, 0x4b, 0xa4, 0x58, 0x7c, 0xde, 0xe8, 0x97, 0xa1, 0xd2,
    0x2c, 0x59, 0x6b, 0x31, 0x7b, 0x55, 0x7e, 0x1f, 0xe2, 0x8e, 0x93, 0x7d, 0x8f, 0x76, 0x61, 0x54,
    0x65, 0x5e, 0x44, 0x2f, 0x24, 0x28, 0x74, 0x2c, 0x27, 0x93, 0xe4, 0x21, 0xb9, 0xaf, 0xc4, 0x18,
    0x94, 0x87, 0xb4, 0x89, 0x99, 0xf6, 0x54, 0xc7, 0x42, 0x10, 0x84, 0xd3, 0x1a, 0x0c, 0x75, 0xdf,
    0x75, 0x90, 0x06, 0x36, 0xd9, 0xe1, 0xcf, 0x33, 0x51, 0x79, 0xbd, 0x45, 0xa8, 0xd2, 0xd2, 0x56,
    0x4a, 0xd2, 0xfc, 0xf9, 0xec, 0x01, 0x0c, 0xcc, 0x84, 0x6d, 0x41, 0x0e, 0x6d, 0x95, 0x39, 0x21,
    0x7a, 0xe2, 0x37, 0x9b, 0x29, 0x77, 0xdf, 0x16, 0xa3, 0x39, 0x2d, 0x74, 0x50, 0x4d, 0xea, 0x93,
    0x2e, 0xc8, 0xd4, 0x6d, 0xba, 0xea, 0x47, 0xab, 0x3f, 0x18, 0x23, 0xbc, 0x50, 0x5e, 0xe3, 0x7d,
    0x48, 0xfa, 0x23, 0xbb, 0x5a, 0x2f, 0x28, 0x26, 0xb0, 0x73, 0xbf, 0x24, 0x3e, 0x23, 0xa4, 0xa4,
    0x42, 0xd2, 0x06, 0xe9, 0x50, 0x17, 0xda, 0x88, 0x9c, 0x8b, 0xbe, 0xe7, 0xa9, 0xc7, 0x79, 0x16,
    0xa2, 0xa2, 0xf7, 0xb0, 0xdd, 0x0b, 0x86, 0x53, 0x08, 0xf3, 0x4f, 0x9f, 0x03, 0xb1, 0x93, 0xbe,
    0x83, 0xb1, 0xe2, 0xda, 0x6a, 0x56, 0x5c, 0xe5, 0x13, 0xa4, 0xda, 0x8d, 0x8b, 0xbe, 0x8d, 0xf5,
    0xb7, 0x42, 0x93, 0x85, 0x4b, 0x97, 0xb0, 0x10, 0xc7, 0x4b, 0xdb, 0xa8, 0x73, 0xc6, 0xc6, 0x60,
    0xfe, 0x07, 0x99, 0xbd, 0x36, 0xc0, 0xad, 0xc3, 0xfe, 0x3a, 0xc2, 0x4a, 0x46, 0x68, 0x6f, 0xe2,
    0x43, 0x68, 0xe8, 0x0c, 0x9d, 0xc8, 0x74, 0x3f, 0xdd, 0x95, 0x7f, 0x7f, 0x75, 0xfd, 0x99, 0x3d,
    0xff, 0x48, 0xf2, 0xdb, 0x25, 0xab, 0xa6, 0x92, 0x0a, 0x77, 0x63, 0x37, 0x7a, 0xb7, 0x93, 0xde,
    0x06, 0xef, 0x99, 0x42, 0x4f, 0xe6, 0x37, 0x95, 0x8d, 0x36, 0xe6, 0xa2, 0x84, 0xd1, 0x15, 0xee,
    0x59, 0x5b, 0xd5, 0x98, 0x6f, 0x63, 0x41, 0x71, 0xbb, 0xd0, 0x55, 0x77, 0xf0, 0x4d, 0x97, 0x4a,
    0xf3, 0xbb, 0x1a, 0x77, 0xac, 0x88, 0xa7, 0x07, 0x64, 0xd7, 0xd9, 0x20, 0xa0, 0xef, 0x01, 0x39,
    0xc5, 0x79, 0x30, 0x5e, 0xe4, 0x3f, 0xd9, 0xe4, 0xc3, 0x13, 0x4b, 0xf4, 0x1e, 0x51, 0xa7, 0xb6,
    0x4b, 0x99, 0x8c, 0x6a, 0x30, 0x0d, 0x99, 0x31, 0x1d, 0x94, 0x12, 0xc5, 0x95, 0x4e, 0xcd, 0xd6,
    0x42, 0x45, 0x56, 0x97, 0xfd, 0x61, 0x05, 0x2e, 0x92, 0x9a, 0xd8, 0x04, 0x29, 0xc3, 0x94, 0x49,
    0xad, 0x0e, 0x28, 0x67, 0xf3, 0x9f, 0x89, 0xf5, 0xf2, 0x27, 0x33, 0xf6, 0xee, 0x8d, 0x37, 0xc1,
};
static const unsigned char UBUNTU_2018_E[3] = { 0x01, 0x00, 0x01 };

static const char BEGIN_MSG[] = "-----BEGIN PGP SIGNED MESSAGE-----\n";
static const char BEGIN_SIG[] = "\n-----BEGIN PGP SIGNATURE-----\n";
static const char END_SIG[]   = "-----END PGP SIGNATURE-----";

/* ---- Hashing ---- */

typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Hash;

static int hash_begin(Hash *x, LPCWSTR algo)
{
    x->alg = NULL;
    x->h = NULL;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&x->alg, algo, NULL, 0))) return -1;
    if (!NT_SUCCESS(BCryptCreateHash(x->alg, &x->h, NULL, 0, NULL, 0, 0))) return -1;
    return 0;
}

static int hash_add(Hash *x, const void *p, size_t n)
{
    return NT_SUCCESS(BCryptHashData(x->h, (PUCHAR)p, (ULONG)n, 0)) ? 0 : -1;
}

static void hash_end(Hash *x)
{
    if (x->h) BCryptDestroyHash(x->h);
    if (x->alg) BCryptCloseAlgorithmProvider(x->alg, 0);
}

/* ---- Armor ---- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* The signature block's base64 body, from after the armor headers' empty line up to
   the "=" checksum line. */
static size_t unarmor(const char *p, const char *end, unsigned char *out, size_t cap)
{
    const char *body = NULL, *q;
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;

    for (q = p; q < end; ) {
        const char *nl = memchr(q, '\n', (size_t)(end - q));
        const char *le = nl ? nl : end;
        if (le == q || (le - q == 1 && *q == '\r')) { body = nl ? nl + 1 : end; break; }
        if (!nl) break;
        q = nl + 1;
    }
    if (!body) return 0;
    for (q = body; q < end; q++) {
        int v;
        if (*q == '=' && (q == body || q[-1] == '\n')) break;   /* checksum line */
        v = b64_val(*q);
        if (v < 0) continue;                                    /* line ends, padding */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return 0;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    return n;
}

/* ---- The signature ---- */

static int rsa_verify(LPCWSTR hash_algo, const unsigned char *digest, ULONG digest_len,
                      const unsigned char *sig, size_t sig_len)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    unsigned char blob[sizeof(BCRYPT_RSAKEY_BLOB) + sizeof(UBUNTU_2018_E) + sizeof(UBUNTU_2018_N)];
    unsigned char padded[sizeof(UBUNTU_2018_N)];
    BCRYPT_RSAKEY_BLOB *hdr = (BCRYPT_RSAKEY_BLOB *)blob;
    BCRYPT_PKCS1_PADDING_INFO pad;
    int ok = -1;

    if (sig_len > sizeof(padded)) return -1;
    memset(padded, 0, sizeof(padded));
    memcpy(padded + sizeof(padded) - sig_len, sig, sig_len);
    hdr->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    hdr->BitLength = sizeof(UBUNTU_2018_N) * 8;
    hdr->cbPublicExp = sizeof(UBUNTU_2018_E);
    hdr->cbModulus = sizeof(UBUNTU_2018_N);
    hdr->cbPrime1 = 0;
    hdr->cbPrime2 = 0;
    memcpy(blob + sizeof(*hdr), UBUNTU_2018_E, sizeof(UBUNTU_2018_E));
    memcpy(blob + sizeof(*hdr) + sizeof(UBUNTU_2018_E), UBUNTU_2018_N, sizeof(UBUNTU_2018_N));
    pad.pszAlgId = hash_algo;
    if (NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, NULL, 0)) &&
        NT_SUCCESS(BCryptImportKeyPair(alg, NULL, BCRYPT_RSAPUBLIC_BLOB, &key, blob, sizeof(blob), 0)) &&
        NT_SUCCESS(BCryptVerifySignature(key, &pad, (PUCHAR)digest, digest_len,
                                         padded, sizeof(padded), BCRYPT_PAD_PKCS1)))
        ok = 0;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* Does a subpacket area name our key as the issuer (fingerprint subpacket 33, or
   key id subpacket 16)? It only picks the key; the signature still decides. */
static int issuer_is_ours(const unsigned char *sp, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t n;
        unsigned char c = sp[i++];
        if (c < 192) {
            n = c;
        } else if (c < 255) {
            if (i >= len) return 0;
            n = ((size_t)(c - 192) << 8) + sp[i++] + 192;
        } else {
            if (len - i < 4) return 0;
            n = ((size_t)sp[i] << 24) | ((size_t)sp[i + 1] << 16) | ((size_t)sp[i + 2] << 8) | sp[i + 3];
            i += 4;
        }
        if (n == 0 || n > len - i) return 0;
        {
            unsigned char type = sp[i] & 0x7F;
            const unsigned char *d = sp + i + 1;
            size_t dn = n - 1;
            if (type == 33 && dn == 21 && d[0] == 4 && memcmp(d + 1, UBUNTU_2018_FPR, 20) == 0) return 1;
            if (type == 16 && dn == 8 && memcmp(d, UBUNTU_2018_FPR + 12, 8) == 0) return 1;
        }
        i += n;
    }
    return 0;
}

/* One signature packet's body, over the signed text. 0 = a good signature by our key. */
static int check_signature(const unsigned char *b, size_t n, const char *text, size_t text_len)
{
    LPCWSTR algo;
    ULONG digest_len;
    size_t hashed_len, unhashed_len, p, mpi_len;
    unsigned char digest[64], trailer[6];
    Hash h;
    int ok = -1, bad = 0;

    if (n < 6 || b[0] != 4 || b[1] != 0x01 || b[2] != 1) return -1;   /* v4, text, RSA */
    switch (b[3]) {
    case 8:  algo = BCRYPT_SHA256_ALGORITHM; digest_len = 32; break;
    case 9:  algo = BCRYPT_SHA384_ALGORITHM; digest_len = 48; break;
    case 10: algo = BCRYPT_SHA512_ALGORITHM; digest_len = 64; break;
    default: return -1;
    }
    hashed_len = ((size_t)b[4] << 8) | b[5];
    if (6 + hashed_len + 2 > n) return -1;
    p = 6 + hashed_len;
    unhashed_len = ((size_t)b[p] << 8) | b[p + 1];
    if (p + 2 + unhashed_len + 2 + 2 > n) return -1;
    if (!issuer_is_ours(b + 6, hashed_len) && !issuer_is_ours(b + p + 2, unhashed_len)) return -1;
    p += 2 + unhashed_len + 2;   /* + the digest's left 16 bits */
    mpi_len = ((((size_t)b[p] << 8) | b[p + 1]) + 7) / 8;
    p += 2;
    if (mpi_len == 0 || mpi_len > n - p) return -1;

    /* Canonical text: lines end in CRLF, trailing spaces and tabs dropped, the last
       line has no line ending. Then the signature's own hashed part and trailer. */
    if (hash_begin(&h, algo) == 0) {
        const char *line = text, *end = text + text_len;
        for (;;) {
            const char *nl = memchr(line, '\n', (size_t)(end - line));
            const char *te = nl ? nl : end;
            if (te > line && te[-1] == '\r') te--;
            while (te > line && (te[-1] == ' ' || te[-1] == '\t')) te--;
            if (line != text && hash_add(&h, "\r\n", 2) != 0) { bad = 1; break; }
            if (te > line && hash_add(&h, line, (size_t)(te - line)) != 0) { bad = 1; break; }
            if (!nl) break;
            line = nl + 1;
        }
        trailer[0] = 4;
        trailer[1] = 0xFF;
        trailer[2] = (unsigned char)((6 + hashed_len) >> 24);
        trailer[3] = (unsigned char)((6 + hashed_len) >> 16);
        trailer[4] = (unsigned char)((6 + hashed_len) >> 8);
        trailer[5] = (unsigned char)(6 + hashed_len);
        if (!bad && hash_add(&h, b, 6 + hashed_len) == 0 && hash_add(&h, trailer, 6) == 0 &&
            NT_SUCCESS(BCryptFinishHash(h.h, digest, digest_len, 0)))
            ok = rsa_verify(algo, digest, digest_len, b + p, mpi_len);
    }
    hash_end(&h);
    return ok;
}

int apt_inrelease_verify(const char *data, size_t len, const char **text_out, size_t *text_len_out)
{
    const char *p, *end = data + len, *text, *sig = NULL, *sig_end = NULL, *q;
    unsigned char *pk = NULL;
    size_t pk_len, off = 0, canon_len = 0, signed_len;
    char *canon = NULL;
    int ok = -1;

    if (len < sizeof(BEGIN_MSG) || memcmp(data, BEGIN_MSG, sizeof(BEGIN_MSG) - 1) != 0) return -1;
    /* Armor headers ("Hash: SHA512") up to an empty line. */
    p = data + sizeof(BEGIN_MSG) - 1;
    while (p < end && *p != '\n') {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) return -1;
        p = nl + 1;
    }
    if (p >= end) return -1;
    text = p + 1;
    /* The signature starts at the first line that is "-----BEGIN PGP SIGNATURE-----";
       dash escaping keeps any line of the text from being one. */
    for (q = text - 1; q && (size_t)(end - q) >= sizeof(BEGIN_SIG) - 1; ) {
        if (memcmp(q, BEGIN_SIG, sizeof(BEGIN_SIG) - 1) == 0) { sig = q; break; }
        q = memchr(q + 1, '\n', (size_t)(end - q - 1));
    }
    if (!sig) return -1;
    for (q = sig; (size_t)(end - q) >= sizeof(END_SIG) - 1; q++)
        if (memcmp(q, END_SIG, sizeof(END_SIG) - 1) == 0) { sig_end = q; break; }
    if (!sig_end) return -1;

    /* The signed text, dash escaping ("- " at a line's start) undone. */
    canon = (char *)malloc((size_t)(sig - text) + 1);
    pk = (unsigned char *)malloc(8192);
    if (!canon || !pk) goto done;
    for (q = text; q < sig; ) {
        const char *nl = memchr(q, '\n', (size_t)(sig - q));
        const char *le = nl ? nl + 1 : sig;
        if (le - q >= 2 && q[0] == '-' && q[1] == ' ') q += 2;
        memcpy(canon + canon_len, q, (size_t)(le - q));
        canon_len += (size_t)(le - q);
        q = le;
    }
    /* The line ending before the signature line isn't part of what was signed. */
    signed_len = canon_len;

    pk_len = unarmor(sig + sizeof(BEGIN_SIG) - 1, sig_end, pk, 8192);
    while (ok != 0 && off + 2 <= pk_len) {
        unsigned char tag = pk[off];
        size_t hl, bl, type;
        if (!(tag & 0x80)) break;
        if (tag & 0x40) {
            unsigned char c = pk[off + 1];
            type = tag & 0x3F;
            if (c < 192) { hl = 2; bl = c; }
            else if (c < 224) {
                if (off + 3 > pk_len) break;
                hl = 3; bl = ((size_t)(c - 192) << 8) + pk[off + 2] + 192;
            } else if (c == 255) {
                if (off + 6 > pk_len) break;
                hl = 6;
                bl = ((size_t)pk[off + 2] << 24) | ((size_t)pk[off + 3] << 16) |
                     ((size_t)pk[off + 4] << 8) | pk[off + 5];
            } else break;   /* partial lengths aren't used for signatures */
        } else {
            type = (tag >> 2) & 0x0F;
            if ((tag & 3) == 0) { hl = 2; bl = pk[off + 1]; }
            else if ((tag & 3) == 1) {
                if (off + 3 > pk_len) break;
                hl = 3; bl = ((size_t)pk[off + 1] << 8) | pk[off + 2];
            } else if ((tag & 3) == 2) {
                if (off + 5 > pk_len) break;
                hl = 5;
                bl = ((size_t)pk[off + 1] << 24) | ((size_t)pk[off + 2] << 16) |
                     ((size_t)pk[off + 3] << 8) | pk[off + 4];
            } else break;
        }
        if (bl > pk_len - off - hl) break;
        /* One good signature by our key is enough; other keys' signatures are skipped. */
        if (type == 2 && check_signature(pk + off + hl, bl, canon, signed_len) == 0) ok = 0;
        off += hl + bl;
    }
done:
    free(pk);
    free(canon);
    if (ok == 0) {
        /* Ubuntu never dash-escapes, so the text in place is the text that was signed;
           a file that needed unescaping is refused rather than handed on escaped. */
        if (canon_len != (size_t)(sig - text)) return -1;
        *text_out = text;
        *text_len_out = (size_t)(sig - text);
    }
    return ok;
}

int apt_release_sha256(const char *text, size_t text_len, const char *path,
                       char hex_out[65], unsigned long long *size_out)
{
    const char *p = text, *end = text + text_len;
    size_t plen = strlen(path);
    int in_sha256 = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t n = (size_t)(le - p);
        if (n && p[0] != ' ') {
            in_sha256 = n >= 7 && memcmp(p, "SHA256:", 7) == 0;
        } else if (in_sha256 && n > 66 + plen) {
            /* " <64 hex> <spaces><size> <path>" */
            const char *h = p + 1, *s;
            char *num_end;
            unsigned long long size;
            int i;
            for (i = 0; i < 64; i++)
                if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) break;
            if (i == 64 && h[64] == ' ') {
                s = h + 64;
                while (s < le && *s == ' ') s++;
                size = _strtoui64(s, &num_end, 10);
                if (num_end > s && *num_end == ' ' && (size_t)(le - (num_end + 1)) == plen &&
                    memcmp(num_end + 1, path, plen) == 0) {
                    memcpy(hex_out, h, 64);
                    hex_out[64] = 0;
                    if (size_out) *size_out = size;
                    return 0;
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return -1;
}

int apt_filename_ok(const char *name, size_t len)
{
    size_t i, seg = 5;
    if (len < 6 || memcmp(name, "pool/", 5) != 0) return 0;
    for (i = 5; i <= len; i++) {
        char c = i < len ? name[i] : '/';
        if (c == '/') {
            size_t sl = i - seg;
            /* No empty, "." or ".." segments. */
            if (sl == 0 || (sl == 1 && name[seg] == '.') ||
                (sl == 2 && name[seg] == '.' && name[seg + 1] == '.'))
                return 0;
            seg = i + 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '+' || c == '~' || c == '%'))
            return 0;
    }
    return 1;
}
