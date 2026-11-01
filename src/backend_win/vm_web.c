/*
 * vm_web.c -- browser access to a VM (see vm_web.h for what it is and what protects it).
 *
 * Threads: an accept thread per listener; one thread per HTTP connection, which
 * becomes the WebSocket reader on upgrade. A session adds a writer, a frame receiver,
 * an encoder, audio, and two clipboard threads. The guest channels take one client
 * at a time, so while a session runs the display window lets go of them
 * (VmInstance.web_session_active).
 */

#define COBJMACROS
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_web.h"
#include "vm_web_encoder.h"
#include "vm_stream.h"
#include "vm_agent.h"
#include "asb_core.h"
#include "game_stream.h"
#include "ui.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "advapi32.lib")

#define WEB_PORT_FIRST   47880   /* loopback HTTP */
#define WEB_LAN_PORT_FIRST 47881 /* network HTTPS */
#define WEB_PORT_TRIES   20
#define WEB_MAX_CONNS    64
#define WEB_MAX_HEADER   8192
#define WEB_MAX_MESSAGE  (4 * 1024 * 1024)   /* largest message from a browser */
#define WEB_SESSION_MS   (12ull * 60 * 60 * 1000)
#define WEB_MAX_SESSIONS 32
#define LOGIN_FAILS_MAX  5
#define LOGIN_WINDOW_MS  60000
#define FIREWALL_RULE    L"App Sandbox browser access"
#define CERT_SUBJECT     L"CN=App Sandbox Browser Access"
#define CERT_CONTAINER   L"AppSandboxBrowserAccess"

/* Client -> server */
#define MSG_INPUT   0x10
#define MSG_RESIZE  0x11
#define MSG_CLIP    0x12
#define MSG_KEYREQ  0x13
#define MSG_BITRATE 0x14
/* Server -> client */
#define MSG_VIDEO   1
#define MSG_AUDIO   2
#define MSG_CURSOR  3
#define MSG_CURSOR_STATE 4
#define MSG_CLIP_OUT 5
#define MSG_STATUS  6

/* Guest wire protocols (see vm_display_idd.c and vm_clipboard.c) */
#define FRAME_MAGIC      0x52465341u
#define FRAME_NV12_MAGIC 0x3231564Eu
#define CURSOR_MAGIC     0x52435341u
#define RESIZE_MAGIC     0x5A525341u
#define INPUT_MAGIC      0x4E495341u
#define INPUT_READY_MAGIC 0x59445249u
#define AUDIO_MAGIC      0x31415341u
#define CLIP_MAGIC       0x504C4341u
#define CLIP_READY_MAGIC 0x59444C43u
#define CLIP_FORMAT_LIST 1
#define CLIP_DATA_REQ    2
#define CLIP_DATA_RESP   3
#define CLIP_SYNC_ENABLE 12
#define CLIP_TEXT_FORMAT 13   /* CF_UNICODETEXT */
#define CLIP_MAX_TEXT    (2 * 1024 * 1024)
#define MIN_W 640
#define MIN_H 480
#define MAX_SIDE 8192
#define MAX_PIXELS (7680u * 4320u)
#define ALIGN2(x) (((x) + 1) & ~1u)
#define VIDEO_QUEUE_MAX 3
#define AUDIO_QUEUE_BYTES (512 * 1024)

#pragma pack(push, 1)
typedef struct { UINT32 magic, width, height, stride; UINT64 seq; UINT32 rects; } FrameHeader;
typedef struct { UINT32 magic; INT32 x, y; UINT32 visible, shape_updated, shape_id, width, height, pitch, xhot, yhot, type, shape_size; } CursorHeader;
typedef struct { UINT32 magic, type, p1, p2, p3; } InputPacket;
typedef struct { UINT32 magic, rate; UINT16 channels, bits, tag, block; } AudioHeader;
typedef struct { UINT32 magic, msg_type, format, size; } ClipHeader;
typedef struct { UINT32 magic, width, height; } ResizeMsg;
#pragma pack(pop)

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1
typedef struct { ADDRESS_FAMILY Family; USHORT Reserved; GUID VmId; GUID ServiceId; } SOCKADDR_HV;

/* ---- Connection, plain or TLS ---- */

typedef struct {
    SOCKET s;
    BOOL   tls;
    CtxtHandle ctx;
    BOOL   have_ctx;
    SecPkgContext_StreamSizes sizes;
    BYTE  *in;          /* encrypted bytes not yet decrypted */
    DWORD  in_len, in_cap;
    BYTE  *plain;       /* decrypted bytes not yet read */
    DWORD  plain_len, plain_off;
    BYTE  *send_buf;
    SRWLOCK send_lock;  /* writers; also covers EncryptMessage */
    CRITICAL_SECTION tls_cs;   /* EncryptMessage against DecryptMessage on one context */
} WebConn;

static CredHandle g_tls_cred;
static BOOL g_tls_ready;
static char g_fingerprint[100];

/* ---- Per-VM access and sessions ---- */

typedef struct WebSession WebSession;

typedef struct {
    UINT64 vm_id;
    char   name[800];
    char   key[65];
    BOOL   lan;
    WebSession *session;
} WebVm;

typedef struct {
    char   token[65];
    UINT64 vm_id;
    BOOL   tls;
    ULONGLONG expires;
} WebCookie;

typedef struct {
    ULONG ip;
    int   fails;
    ULONGLONG since;
} LoginLimit;

static SRWLOCK    g_lock = SRWLOCK_INIT;
static WebVm      g_vms_web[ASB_MAX_VMS];
static WebCookie  g_cookies[WEB_MAX_SESSIONS];
static LoginLimit g_limits[32];
static SOCKET     g_listen_local = INVALID_SOCKET, g_listen_lan = INVALID_SOCKET;
static int        g_port_local, g_port_lan;
static volatile LONG g_conns;
static BOOL       g_dashboard, g_dashboard_lan;
static volatile LONG g_shutdown;

/* ---- Small helpers ---- */

static BOOL random_hex(char *out, int hex_chars)
{
    BYTE raw[64];
    int i;
    if (hex_chars / 2 > (int)sizeof(raw)) return FALSE;
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, raw, hex_chars / 2, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return FALSE;
    for (i = 0; i < hex_chars / 2; i++) sprintf_s(out + i * 2, 3, "%02x", raw[i]);
    SecureZeroMemory(raw, sizeof(raw));
    return TRUE;
}

static BOOL equal_secret(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned char diff = (unsigned char)(la != lb);
    if (la != lb || la == 0) return FALSE;
    for (i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static VmInstance *vm_by_id(UINT64 id)
{
    int i, n = asb_vm_count();
    for (i = 0; i < n; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (v && v->unique_id == id) return v;
    }
    return NULL;
}

static WebVm *web_vm_find_name(const char *name)
{
    int i;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (g_vms_web[i].vm_id && _stricmp(g_vms_web[i].name, name) == 0) return &g_vms_web[i];
    return NULL;
}

static WebVm *web_vm_find_id(UINT64 id)
{
    int i;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (g_vms_web[i].vm_id == id) return &g_vms_web[i];
    return NULL;
}

/* In place. Rejects what could never be a name, including an encoded NUL. */
static BOOL url_decode(char *s)
{
    char *in = s, *out = s;
    while (*in) {
        if (*in == '%') {
            char hex[3];
            unsigned long v;
            char *end;
            if (!in[1] || !in[2]) return FALSE;
            hex[0] = in[1]; hex[1] = in[2]; hex[2] = 0;
            v = strtoul(hex, &end, 16);
            if (*end || v == 0) return FALSE;
            *out++ = (char)v;
            in += 3;
        } else {
            *out++ = *in++;
        }
    }
    *out = 0;
    return TRUE;
}

static int url_encode(const char *s, char *out, int cap)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (n + 4 >= cap) break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
    return n;
}

static void run_hidden(const wchar_t *command)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t buf[512];
    wcsncpy_s(buf, 512, command, _TRUNCATE);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* ---- TLS (only on the network listener) ---- */

static PCCERT_CONTEXT tls_find_cert(HCERTSTORE store)
{
    PCCERT_CONTEXT c = NULL;
    SYSTEMTIME st;
    FILETIME now;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &now);
    while ((c = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W,
                                           L"App Sandbox Browser Access", c)) != NULL) {
        if (CompareFileTime(&now, &c->pCertInfo->NotAfter) < 0) return c;
    }
    return NULL;
}

/* A self-signed certificate kept for this PC, so its fingerprint stays the same and
   a device only has to check it once. */
static PCCERT_CONTEXT tls_create_cert(HCERTSTORE store)
{
    HCRYPTPROV prov = 0;
    HCRYPTKEY key = 0;
    BYTE name_blob[256];
    DWORD name_size = sizeof(name_blob);
    CERT_NAME_BLOB subject;
    CRYPT_KEY_PROV_INFO info;
    CRYPT_ALGORITHM_IDENTIFIER alg;
    SYSTEMTIME start, end;
    PCCERT_CONTEXT cert = NULL, stored = NULL;

    if (!CryptAcquireContextW(&prov, CERT_CONTAINER, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_MACHINE_KEYSET | CRYPT_NEWKEYSET) &&
        !CryptAcquireContextW(&prov, CERT_CONTAINER, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_MACHINE_KEYSET))
        return NULL;
    if (CryptGenKey(prov, AT_KEYEXCHANGE, (2048u << 16), &key)) CryptDestroyKey(key);
    CryptReleaseContext(prov, 0);

    if (!CertStrToNameW(X509_ASN_ENCODING, CERT_SUBJECT, CERT_X500_NAME_STR, NULL,
                        name_blob, &name_size, NULL))
        return NULL;
    subject.cbData = name_size;
    subject.pbData = name_blob;
    ZeroMemory(&info, sizeof(info));
    info.pwszContainerName = CERT_CONTAINER;
    info.pwszProvName = MS_ENH_RSA_AES_PROV_W;
    info.dwProvType = PROV_RSA_AES;
    info.dwFlags = CRYPT_MACHINE_KEYSET;
    info.dwKeySpec = AT_KEYEXCHANGE;
    ZeroMemory(&alg, sizeof(alg));
    alg.pszObjId = szOID_RSA_SHA256RSA;
    GetSystemTime(&start);
    end = start;
    end.wYear += 5;
    cert = CertCreateSelfSignCertificate(0, &subject, 0, &info, &alg, &start, &end, NULL);
    if (!cert) return NULL;
    CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, &stored);
    CertFreeCertificateContext(cert);
    return stored;
}

static BOOL tls_init(void)
{
    HCERTSTORE store;
    PCCERT_CONTEXT cert;
    SCHANNEL_CRED cred;
    TimeStamp ts;
    BYTE hash[32];
    DWORD hash_size = sizeof(hash), i;
    SECURITY_STATUS ss;

    if (g_tls_ready) return TRUE;
    store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                          CERT_SYSTEM_STORE_LOCAL_MACHINE, L"MY");
    if (!store) return FALSE;
    cert = tls_find_cert(store);
    if (!cert) cert = tls_create_cert(store);
    if (!cert) {
        ui_log(L"Browser access: could not create a certificate (0x%08lX).", GetLastError());
        CertCloseStore(store, 0);
        return FALSE;
    }
    if (CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, NULL, cert->pbCertEncoded,
                              cert->cbCertEncoded, hash, &hash_size)) {
        int pos = 0;
        for (i = 0; i < hash_size; i++)
            pos += sprintf_s(g_fingerprint + pos, sizeof(g_fingerprint) - pos, "%s%02X", i ? ":" : "", hash[i]);
    }
    ZeroMemory(&cred, sizeof(cred));
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.cCreds = 1;
    cred.paCred = &cert;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    cred.dwFlags = SCH_USE_STRONG_CRYPTO;
    ss = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_INBOUND, NULL, &cred,
                                   NULL, NULL, &g_tls_cred, &ts);
    CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);
    if (ss != SEC_E_OK) {
        ui_log(L"Browser access: TLS credentials failed (0x%08lX).", (unsigned long)ss);
        return FALSE;
    }
    g_tls_ready = TRUE;
    return TRUE;
}

static BOOL raw_send_all(SOCKET s, const void *data, DWORD len)
{
    const char *p = (const char *)data;
    while (len) {
        int n = send(s, p, (int)(len > 1048576 ? 1048576 : len), 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= (DWORD)n;
    }
    return TRUE;
}

static BOOL tls_handshake(WebConn *c)
{
    SecBufferDesc in_desc, out_desc;
    SecBuffer in_bufs[2], out_buf;
    SECURITY_STATUS ss = SEC_I_CONTINUE_NEEDED;
    DWORD flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                  ASC_REQ_STREAM | ASC_REQ_ALLOCATE_MEMORY, out_flags = 0;
    TimeStamp ts;

    c->in_cap = 32768;
    c->in = (BYTE *)malloc(c->in_cap);
    if (!c->in) return FALSE;
    c->in_len = 0;
    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        if (c->in_len == 0 || ss == SEC_E_INCOMPLETE_MESSAGE) {
            int n;
            if (c->in_len >= c->in_cap) return FALSE;
            n = recv(c->s, (char *)c->in + c->in_len, (int)(c->in_cap - c->in_len), 0);
            if (n <= 0) return FALSE;
            c->in_len += (DWORD)n;
        }
        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[0].cbBuffer = c->in_len;
        in_bufs[0].pvBuffer = c->in;
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        in_bufs[1].cbBuffer = 0;
        in_bufs[1].pvBuffer = NULL;
        in_desc.ulVersion = SECBUFFER_VERSION;
        in_desc.cBuffers = 2;
        in_desc.pBuffers = in_bufs;
        out_buf.BufferType = SECBUFFER_TOKEN;
        out_buf.cbBuffer = 0;
        out_buf.pvBuffer = NULL;
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = &out_buf;
        ss = AcceptSecurityContext(&g_tls_cred, c->have_ctx ? &c->ctx : NULL, &in_desc, flags,
                                   0, c->have_ctx ? NULL : &c->ctx, &out_desc, &out_flags, &ts);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) continue;
        c->have_ctx = TRUE;
        if (out_buf.pvBuffer) {
            BOOL ok = out_buf.cbBuffer == 0 || raw_send_all(c->s, out_buf.pvBuffer, out_buf.cbBuffer);
            FreeContextBuffer(out_buf.pvBuffer);
            if (!ok) return FALSE;
        }
        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer) {
                memmove(c->in, c->in + c->in_len - in_bufs[1].cbBuffer, in_bufs[1].cbBuffer);
                c->in_len = in_bufs[1].cbBuffer;
            } else {
                c->in_len = 0;
            }
        }
    }
    if (ss != SEC_E_OK) return FALSE;
    if (QueryContextAttributesW(&c->ctx, SECPKG_ATTR_STREAM_SIZES, &c->sizes) != SEC_E_OK) return FALSE;
    c->send_buf = (BYTE *)malloc(c->sizes.cbHeader + c->sizes.cbMaximumMessage + c->sizes.cbTrailer);
    c->plain = (BYTE *)malloc(c->in_cap);
    return c->send_buf && c->plain;
}

static int conn_recv(WebConn *c, void *buf, int cap)
{
    if (!c->tls) return recv(c->s, (char *)buf, cap, 0);
    for (;;) {
        SecBufferDesc desc;
        SecBuffer bufs[4];
        SECURITY_STATUS ss;
        int i;
        if (c->plain_len) {
            int n = (int)c->plain_len < cap ? (int)c->plain_len : cap;
            memcpy(buf, c->plain + c->plain_off, n);
            c->plain_off += n;
            c->plain_len -= n;
            return n;
        }
        if (c->in_len) {
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].cbBuffer = c->in_len;
            bufs[0].pvBuffer = c->in;
            for (i = 1; i < 4; i++) {
                bufs[i].BufferType = SECBUFFER_EMPTY;
                bufs[i].cbBuffer = 0;
                bufs[i].pvBuffer = NULL;
            }
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = bufs;
            EnterCriticalSection(&c->tls_cs);
            ss = DecryptMessage(&c->ctx, &desc, 0, NULL);
            LeaveCriticalSection(&c->tls_cs);
            if (ss == SEC_E_OK) {
                DWORD extra = 0;
                BYTE *extra_ptr = NULL;
                c->plain_len = 0;
                c->plain_off = 0;
                for (i = 0; i < 4; i++) {
                    if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer) {
                        memcpy(c->plain, bufs[i].pvBuffer, bufs[i].cbBuffer);
                        c->plain_len = bufs[i].cbBuffer;
                    } else if (bufs[i].BufferType == SECBUFFER_EXTRA) {
                        extra = bufs[i].cbBuffer;
                        extra_ptr = (BYTE *)bufs[i].pvBuffer;
                    }
                }
                if (extra) memmove(c->in, extra_ptr, extra);
                c->in_len = extra;
                continue;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) return -1;   /* closed, renegotiation or bad data */
        }
        {
            int n;
            if (c->in_len >= c->in_cap) return -1;
            n = recv(c->s, (char *)c->in + c->in_len, (int)(c->in_cap - c->in_len), 0);
            if (n <= 0) return n;
            c->in_len += (DWORD)n;
        }
    }
}

/* Serialised by send_lock. */
static BOOL conn_send(WebConn *c, const void *data, DWORD len)
{
    const BYTE *p = (const BYTE *)data;
    BOOL ok = TRUE;
    AcquireSRWLockExclusive(&c->send_lock);
    if (!c->tls) {
        ok = raw_send_all(c->s, data, len);
    } else {
        while (len && ok) {
            DWORD chunk = len < c->sizes.cbMaximumMessage ? len : c->sizes.cbMaximumMessage;
            SecBufferDesc desc;
            SecBuffer bufs[4];
            SECURITY_STATUS ss;
            memcpy(c->send_buf + c->sizes.cbHeader, p, chunk);
            bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
            bufs[0].cbBuffer = c->sizes.cbHeader;
            bufs[0].pvBuffer = c->send_buf;
            bufs[1].BufferType = SECBUFFER_DATA;
            bufs[1].cbBuffer = chunk;
            bufs[1].pvBuffer = c->send_buf + c->sizes.cbHeader;
            bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
            bufs[2].cbBuffer = c->sizes.cbTrailer;
            bufs[2].pvBuffer = c->send_buf + c->sizes.cbHeader + chunk;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            bufs[3].cbBuffer = 0;
            bufs[3].pvBuffer = NULL;
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = bufs;
            EnterCriticalSection(&c->tls_cs);
            ss = EncryptMessage(&c->ctx, 0, &desc, 0);
            LeaveCriticalSection(&c->tls_cs);
            ok = ss == SEC_E_OK &&
                 raw_send_all(c->s, c->send_buf, bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
            p += chunk;
            len -= chunk;
        }
    }
    ReleaseSRWLockExclusive(&c->send_lock);
    return ok;
}

static void conn_free(WebConn *c)
{
    if (c->s != INVALID_SOCKET) closesocket(c->s);
    if (c->have_ctx) DeleteSecurityContext(&c->ctx);
    DeleteCriticalSection(&c->tls_cs);
    free(c->in);
    free(c->plain);
    free(c->send_buf);
    free(c);
}

/* ---- Guest channels ---- */

static SOCKET hv_connect(const GUID *runtime_id, const wchar_t *os_type, unsigned port, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV a;
    u_long nb = 1;
    fd_set wr, er;
    struct timeval tv;
    DWORD none = 0;
    static const GUID zero = { 0 };

    if (memcmp(runtime_id, &zero, sizeof(GUID)) == 0) return INVALID_SOCKET;
    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return s;
    ioctlsocket(s, FIONBIO, &nb);
    ZeroMemory(&a, sizeof(a));
    a.Family = AF_HYPERV;
    a.VmId = *runtime_id;
    hcs_service_guid(os_type, port, &a.ServiceId);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    FD_ZERO(&wr); FD_ZERO(&er);
    FD_SET(s, &wr); FD_SET(s, &er);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(0, NULL, &wr, &er, &tv) <= 0 || FD_ISSET(s, &er)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&none, sizeof(none));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&none, sizeof(none));
    return s;
}

/* The session's stop flag, on threads that read guest channels. shutdown() from another
   thread doesn't wake a recv on a Hyper-V socket, so those threads look before reading. */
static __declspec(thread) volatile LONG *t_stop;

static BOOL recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n;
        if (t_stop) {
            fd_set rd;
            struct timeval tv = { 0, 250000 };
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            n = select(0, &rd, NULL, NULL, &tv);
            if (n < 0 || *t_stop) return FALSE;
            if (n == 0) continue;
        }
        n = recv(s, p, len, 0);
        if (n <= 0) return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

/* ---- Session ---- */

typedef struct QItem {
    struct QItem *next;
    DWORD size;
    BOOL  video;
    BOOL  last;   /* close the connection once this has gone out */
    BYTE  data[1];
} QItem;

struct WebSession {
    UINT64  vm_id;
    GUID    runtime_id;
    wchar_t os_type[32];
    WebConn *conn;
    volatile LONG stop;

    CRITICAL_SECTION q_cs;
    CONDITION_VARIABLE q_cv;
    QItem  *q_head, *q_tail;
    LONG    q_video;
    DWORD   q_bytes;

    SRWLOCK frame_lock;
    BYTE   *bgra;         /* what the guest last sent as BGRA; rects land here */
    BYTE   *nv12;         /* what the encoder reads */
    UINT    width, height;
    UINT32  enc_width, enc_height;
    volatile LONG dirty;
    HANDLE  frame_event;
    volatile LONG frame_count;

    CRITICAL_SECTION sock_cs;
    SOCKET  frame_sock, input_sock, audio_sock, clip_write, clip_read;
    volatile LONG resize_w, resize_h;
    volatile LONG want_key;
    volatile LONG bitrate_mbps;
    volatile LONG suppress_echo;
    UINT32  cursor_visible;
    BOOL    cursor_known;

    HANDLE  threads[6];
    int     thread_count;
};

/* ---- Writing WebSocket messages ---- */

static void q_push(WebSession *ws, QItem *item)
{
    item->next = NULL;
    EnterCriticalSection(&ws->q_cs);
    if (ws->q_tail) ws->q_tail->next = item; else ws->q_head = item;
    ws->q_tail = item;
    ws->q_bytes += item->size;
    if (item->video) ws->q_video++;
    WakeConditionVariable(&ws->q_cv);
    LeaveCriticalSection(&ws->q_cs);
}

/* A frame header in front of the payload, unmasked, as servers send them. */
static QItem *ws_build(BYTE kind, const void *head, DWORD head_size, const void *body, DWORD body_size)
{
    DWORD payload = 1 + head_size + body_size, hdr;
    QItem *item = (QItem *)malloc(sizeof(QItem) + payload + 10);
    BYTE *p;
    if (!item) return NULL;
    p = item->data;
    *p++ = 0x82;   /* FIN, binary */
    if (payload < 126) {
        *p++ = (BYTE)payload;
    } else if (payload < 65536) {
        *p++ = 126;
        *p++ = (BYTE)(payload >> 8);
        *p++ = (BYTE)payload;
    } else {
        int i;
        *p++ = 127;
        for (i = 7; i >= 0; i--) *p++ = i >= 4 ? 0 : (BYTE)(payload >> (i * 8));
    }
    hdr = (DWORD)(p - item->data);
    *p++ = kind;
    if (head_size) { memcpy(p, head, head_size); p += head_size; }
    if (body_size) memcpy(p, body, body_size);
    item->size = hdr + payload;
    item->video = FALSE;
    item->last = FALSE;
    return item;
}

static void ws_send(WebSession *ws, BYTE kind, const void *head, DWORD head_size, const void *body, DWORD body_size)
{
    QItem *item = ws_build(kind, head, head_size, body, body_size);
    if (item) q_push(ws, item);
}

static DWORD WINAPI writer_thread(LPVOID param)
{
    WebSession *ws = (WebSession *)param;
    while (!ws->stop) {
        QItem *item;
        EnterCriticalSection(&ws->q_cs);
        while (!ws->q_head && !ws->stop) SleepConditionVariableCS(&ws->q_cv, &ws->q_cs, 500);
        item = ws->q_head;
        if (item) {
            ws->q_head = item->next;
            if (!ws->q_head) ws->q_tail = NULL;
            ws->q_bytes -= item->size;
            if (item->video) ws->q_video--;
        }
        LeaveCriticalSection(&ws->q_cs);
        if (item) {
            BOOL ok = conn_send(ws->conn, item->data, item->size) && !item->last;
            free(item);
            if (!ok) { InterlockedExchange(&ws->stop, 1); shutdown(ws->conn->s, SD_BOTH); }
        }
    }
    return 0;
}

/* ---- Frames: guest -> NV12 ---- */

/* Same conversion as the display driver's (full-range BT.601, chroma the 2x2 average),
   so its NV12 frames and these look alike. Rect in even coordinates. */
static void bgra_rect_to_nv12(const WebSession *ws, UINT left, UINT top, UINT right, UINT bottom)
{
    const UINT w = ws->width, h = ws->height;
    const BYTE *src = ws->bgra;
    BYTE *yp = ws->nv12, *uvp = ws->nv12 + w * h;
    UINT x, y;
#define LUM(px) (BYTE)((77 * (px)[2] + 150 * (px)[1] + 29 * (px)[0] + 128) >> 8)
    for (y = top; y < bottom; y += 2) {
        const BYTE *r0 = src + (size_t)y * w * 4;
        const BYTE *r1 = r0 + (size_t)w * 4;
        BYTE *y0 = yp + (size_t)y * w, *y1 = y0 + w;
        BYTE *uv = uvp + (size_t)(y / 2) * w;
        for (x = left; x < right; x += 2) {
            const BYTE *a = r0 + (size_t)x * 4, *c = r1 + (size_t)x * 4;
            int sb, sg, sr, u, v;
            y0[x] = LUM(a); y0[x + 1] = LUM(a + 4);
            y1[x] = LUM(c); y1[x + 1] = LUM(c + 4);
            sb = (a[0] + a[4] + c[0] + c[4]) >> 2;
            sg = (a[1] + a[5] + c[1] + c[5]) >> 2;
            sr = (a[2] + a[6] + c[2] + c[6]) >> 2;
            u = ((-43 * sr - 85 * sg + 128 * sb + 128) >> 8) + 128;
            v = ((128 * sr - 107 * sg - 21 * sb + 128) >> 8) + 128;
            uv[x] = (BYTE)(u < 0 ? 0 : u > 255 ? 255 : u);
            uv[x + 1] = (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
#undef LUM
}

/* The session's picture is always even-sized (what the codec needs): a guest display
   of odd size loses its last column or row here. */
static BOOL frame_size_set(WebSession *ws, UINT guest_w, UINT guest_h)
{
    UINT w = guest_w & ~1u, h = guest_h & ~1u;
    if (w == ws->width && h == ws->height) return TRUE;
    {
        BYTE *bgra = (BYTE *)calloc((size_t)(w + 2) * (h + 2), 4);
        BYTE *nv12 = (BYTE *)malloc((size_t)w * h * 3 / 2);
        if (!bgra || !nv12) { free(bgra); free(nv12); return FALSE; }
        memset(nv12, 16, (size_t)w * h);
        memset(nv12 + (size_t)w * h, 128, (size_t)w * h / 2);
        free(ws->bgra);
        free(ws->nv12);
        ws->bgra = bgra;
        ws->nv12 = nv12;
        ws->width = w;
        ws->height = h;
    }
    return TRUE;
}

static BOOL read_cursor(WebSession *ws, SOCKET s)
{
    CursorHeader c;
    BYTE state[12];
    if (!recv_all(s, (BYTE *)&c + 4, sizeof(c) - 4)) return FALSE;
    if (c.shape_updated && c.shape_size) {
        BYTE *shape;
        if (c.shape_size > 256u * 256 * 4 * 2) return FALSE;
        shape = (BYTE *)malloc(c.shape_size);
        if (!shape) return FALSE;
        if (!recv_all(s, shape, (int)c.shape_size)) { free(shape); return FALSE; }
        /* To RGBA for a CSS cursor. A masked cursor's alpha is its AND mask: 0 is
           opaque; 0xFF XORs with the screen, which a browser cursor can't do, so a
           black outline stands in for inverting pixels. */
        if (c.width && c.height && c.width <= 256 && c.height <= 256 && c.pitch >= c.width * 4 &&
            (UINT64)c.pitch * c.height <= c.shape_size) {
            DWORD size = c.width * c.height * 4;
            BYTE *rgba = (BYTE *)malloc(size);
            if (rgba) {
                UINT x, y;
                UINT32 head[4];
                for (y = 0; y < c.height; y++) {
                    const BYTE *src = shape + (size_t)y * c.pitch;
                    BYTE *dst = rgba + (size_t)y * c.width * 4;
                    for (x = 0; x < c.width; x++, src += 4, dst += 4) {
                        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
                        if (c.type == 1)
                            dst[3] = src[3] == 0 ? 255 : ((src[0] | src[1] | src[2]) ? 255 : 0);
                        else
                            dst[3] = src[3];
                        if (c.type == 1 && src[3] != 0 && dst[3]) dst[0] = dst[1] = dst[2] = 0;
                    }
                }
                head[0] = c.width; head[1] = c.height; head[2] = c.xhot; head[3] = c.yhot;
                ws_send(ws, MSG_CURSOR, head, sizeof(head), rgba, size);
                free(rgba);
            }
        }
        free(shape);
    }
    if (!ws->cursor_known || ws->cursor_visible != c.visible) {
        ws->cursor_known = TRUE;
        ws->cursor_visible = c.visible;
        memcpy(state, &c.visible, 4);
        memcpy(state + 4, &c.x, 4);
        memcpy(state + 8, &c.y, 4);
        ws_send(ws, MSG_CURSOR_STATE, state, sizeof(state), NULL, 0);
    }
    return TRUE;
}

static BOOL read_frame(WebSession *ws, SOCKET s, UINT32 magic, BYTE **buf, DWORD *cap)
{
    FrameHeader h;
    RECT rects[64];
    UINT32 size, i;
    const BOOL nv12 = magic == FRAME_NV12_MAGIC;
    if (!recv_all(s, (BYTE *)&h + 4, sizeof(h) - 4)) return FALSE;
    if (!h.width || !h.height || h.width > MAX_SIDE || h.height > MAX_SIDE ||
        (UINT64)h.width * h.height > MAX_PIXELS || h.rects > 64 ||
        h.width < 2 || h.height < 2 ||
        h.stride < h.width * (nv12 ? 1 : 4))
        return FALSE;
    if (nv12 && (h.rects || h.stride != ALIGN2(h.width))) return FALSE;
    if (h.rects && !recv_all(s, rects, (int)(h.rects * sizeof(RECT)))) return FALSE;
    if (!recv_all(s, &size, 4)) return FALSE;
    if ((UINT64)size > (UINT64)MAX_PIXELS * 4) return FALSE;
    if (nv12 && size != ALIGN2(h.width) * ALIGN2(h.height) / 2 * 3) return FALSE;
    if (size > *cap) {
        BYTE *bigger = (BYTE *)realloc(*buf, size);
        if (!bigger) return FALSE;
        *buf = bigger;
        *cap = size;
    }
    if (size && !recv_all(s, *buf, (int)size)) return FALSE;

    AcquireSRWLockExclusive(&ws->frame_lock);
    if (frame_size_set(ws, h.width, h.height)) {
        const UINT w = ws->width, fh = ws->height;
        if (nv12) {
            const UINT aw = ALIGN2(h.width), ah = ALIGN2(h.height);
            UINT y;
            for (y = 0; y < fh; y++) memcpy(ws->nv12 + (size_t)y * w, *buf + (size_t)y * aw, w);
            for (y = 0; y < fh / 2; y++)
                memcpy(ws->nv12 + (size_t)w * fh + (size_t)y * w, *buf + (size_t)aw * ah + (size_t)y * aw, w);
        } else if (h.rects == 0) {
            UINT y, rows = h.stride ? size / h.stride : 0;
            for (y = 0; y < fh && y < rows; y++)
                memcpy(ws->bgra + (size_t)y * w * 4, *buf + (size_t)y * h.stride, (size_t)w * 4);
            bgra_rect_to_nv12(ws, 0, 0, w, fh);
        } else {
            const BYTE *src = *buf;
            DWORD used = 0;
            for (i = 0; i < h.rects; i++) {
                LONG l = rects[i].left, t = rects[i].top, r = rects[i].right, b = rects[i].bottom;
                LONG cl, ct, cr, cb, y;
                DWORD row_bytes;
                /* Rows come as the guest sent them, clamped to its frame. */
                if (l < 0) l = 0;
                if (t < 0) t = 0;
                if (r > (LONG)h.width) r = (LONG)h.width;
                if (b > (LONG)h.height) b = (LONG)h.height;
                if (l >= r || t >= b) continue;
                row_bytes = (DWORD)(r - l) * 4;
                if (used + (UINT64)row_bytes * (DWORD)(b - t) > size) break;
                cr = r > (LONG)w ? (LONG)w : r;
                cb = b > (LONG)fh ? (LONG)fh : b;
                for (y = t; y < cb; y++)
                    if (l < cr)
                        memcpy(ws->bgra + (size_t)y * w * 4 + (size_t)l * 4,
                               src + (size_t)(y - t) * row_bytes, (size_t)(cr - l) * 4);
                src += (size_t)row_bytes * (DWORD)(b - t);
                used += row_bytes * (DWORD)(b - t);
                cl = l & ~1L;
                ct = t & ~1L;
                cr = (cr + 1) & ~1L;
                cb = (cb + 1) & ~1L;
                if (cl < cr && ct < cb) bgra_rect_to_nv12(ws, (UINT)cl, (UINT)ct, (UINT)cr, (UINT)cb);
            }
        }
        InterlockedExchange(&ws->dirty, 1);
        InterlockedIncrement(&ws->frame_count);
    }
    ReleaseSRWLockExclusive(&ws->frame_lock);
    SetEvent(ws->frame_event);
    return TRUE;
}

static void send_resize(WebSession *ws)
{
    ResizeMsg m;
    m.magic = RESIZE_MAGIC;
    m.width = (UINT32)ws->resize_w;
    m.height = (UINT32)ws->resize_h;
    if (!m.width || !m.height) return;
    EnterCriticalSection(&ws->sock_cs);
    if (ws->frame_sock != INVALID_SOCKET)
        send(ws->frame_sock, (const char *)&m, sizeof(m), 0);
    LeaveCriticalSection(&ws->sock_cs);
}

/* Tell the agent to start the helpers in the console session, as the window does. */
static void wake_helpers(UINT64 vm_id)
{
    VmInstance *v = vm_by_id(vm_id);
    if (v && v->agent_online && !v->agent_initializing)
        vm_agent_send(v, "idd_connect", NULL, 0, 30000);
}

static DWORD WINAPI frame_thread(LPVOID param)
{
    WebSession *ws = (WebSession *)param;
    DWORD cap = 1920 * 1080 * 4;
    BYTE *buf = (BYTE *)malloc(cap);
    BOOL helpers_woken = FALSE;

    t_stop = &ws->stop;
    while (!ws->stop && buf) {
        SOCKET s;
        VmInstance *v = vm_by_id(ws->vm_id);
        if (!v) break;
        ws->runtime_id = v->runtime_id;
        if (!helpers_woken && v->agent_online && !v->agent_initializing) {
            wake_helpers(ws->vm_id);
            helpers_woken = TRUE;
        }
        if (!v->running) helpers_woken = FALSE;
        s = hv_connect(&ws->runtime_id, ws->os_type, 2, 3000);
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }
        EnterCriticalSection(&ws->sock_cs);
        ws->frame_sock = s;
        LeaveCriticalSection(&ws->sock_cs);
        send_resize(ws);
        while (!ws->stop) {
            UINT32 magic;
            if (!recv_all(s, &magic, 4)) break;
            if (magic == CURSOR_MAGIC) {
                if (!read_cursor(ws, s)) break;
            } else if (magic == FRAME_MAGIC || magic == FRAME_NV12_MAGIC) {
                if (!read_frame(ws, s, magic, &buf, &cap)) break;
            } else {
                break;
            }
        }
        EnterCriticalSection(&ws->sock_cs);
        ws->frame_sock = INVALID_SOCKET;
        LeaveCriticalSection(&ws->sock_cs);
        closesocket(s);
        if (!ws->stop) Sleep(500);
    }
    free(buf);
    return 0;
}

/* ---- Encoding ---- */

static void on_encoded(void *user, const BYTE *data, DWORD size, BOOL key)
{
    WebSession *ws = (WebSession *)user;
    BYTE head[9];
    QItem *item;
    head[0] = key ? 1 : 0;
    memcpy(head + 1, &ws->enc_width, 4);
    memcpy(head + 5, &ws->enc_height, 4);
    item = ws_build(MSG_VIDEO, head, sizeof(head), data, size);
    if (item) {
        item->video = TRUE;
        q_push(ws, item);
    }
}

/* Roughly what it takes at 60 fps for a desktop and moving pictures to look right. */
static UINT auto_bitrate(UINT w, UINT h)
{
    UINT64 bits = (UINT64)w * h * 12;
    if (bits < 4000000) bits = 4000000;
    if (bits > 80000000) bits = 80000000;
    return (UINT)bits;
}

static DWORD WINAPI encode_thread(LPVOID param)
{
    WebSession *ws = (WebSession *)param;
    WebEncoder *enc = NULL;
    UINT enc_w = 0, enc_h = 0;
    LONG last_mbps = 0;
    BYTE *copy = NULL;
    size_t copy_cap = 0;
    ULONGLONG last_submit = 0, last_status = 0;
    LONG frames_at_status = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (!ws->stop) {
        ULONGLONG now = GetTickCount64();
        LONG video_queued;

        if (now - last_status >= 2000) {
            VmInstance *v = vm_by_id(ws->vm_id);
            char status[256];
            LONG frames = ws->frame_count;
            double fps = last_status ? (frames - frames_at_status) * 1000.0 / (double)(now - last_status) : 0;
            int n = sprintf_s(status, sizeof(status),
                "{\"running\":%s,\"online\":%s,\"width\":%u,\"height\":%u,\"guestFps\":%.0f,\"encoder\":\"%s\"}",
                v && v->running ? "true" : "false", v && v->agent_online ? "true" : "false",
                ws->width, ws->height, fps,
                enc ? (web_encoder_is_hardware(enc) ? "gpu" : "software") : "none");
            last_status = now;
            frames_at_status = frames;
            ws_send(ws, MSG_STATUS, status, (DWORD)n, NULL, 0);
        }

        if (ws->dirty && (ws->width != enc_w || ws->height != enc_h)) {
            if (enc) web_encoder_destroy(enc);
            enc_w = ws->width;
            enc_h = ws->height;
            ws->enc_width = enc_w;
            ws->enc_height = enc_h;
            last_mbps = ws->bitrate_mbps;
            enc = web_encoder_create(enc_w, enc_h, 60,
                                     last_mbps ? (UINT)last_mbps * 1000000u : auto_bitrate(enc_w, enc_h),
                                     on_encoded, ws);
            if (enc) {
                ui_log(L"Browser access: encoding %ux%u with %s.", enc_w, enc_h,
                       web_encoder_is_hardware(enc) ? L"the host GPU" : L"the software encoder");
                web_encoder_force_key(enc);
            } else {
                ui_log(L"Browser access: no H.264 encoder could start at %ux%u.", enc_w, enc_h);
                {
                    static const char no_encoder[] = "{\"error\":\"No video encoder could start on this PC.\"}";
                    ws_send(ws, MSG_STATUS, no_encoder, sizeof(no_encoder) - 1, NULL, 0);
                }
                enc_w = enc_h = 0;
                Sleep(2000);
                continue;
            }
        }

        if (enc) {
            if (!web_encoder_pump(enc)) {
                ui_log(L"Browser access: the encoder failed; starting it again.");
                web_encoder_destroy(enc);
                enc = NULL;
                enc_w = enc_h = 0;
                InterlockedExchange(&ws->dirty, 1);
                continue;
            }
            if (last_mbps != ws->bitrate_mbps) {
                last_mbps = ws->bitrate_mbps;
                web_encoder_set_bitrate(enc, last_mbps ? (UINT)last_mbps * 1000000u : auto_bitrate(enc_w, enc_h));
            }
            if (InterlockedExchange(&ws->want_key, 0)) {
                web_encoder_force_key(enc);
                InterlockedExchange(&ws->dirty, 1);
            }
            EnterCriticalSection(&ws->q_cs);
            video_queued = ws->q_video;
            LeaveCriticalSection(&ws->q_cs);
            /* Behind on the network: skip input frames rather than queue encoded ones, so
               there is never old video in line and no reference frame is ever lost. */
            if (ws->dirty && video_queued < VIDEO_QUEUE_MAX && web_encoder_ready(enc) &&
                now - last_submit >= 14) {
                size_t size;
                InterlockedExchange(&ws->dirty, 0);
                AcquireSRWLockShared(&ws->frame_lock);
                size = (size_t)ws->width * ws->height * 3 / 2;
                if (ws->width == enc_w && ws->height == enc_h) {
                    if (size > copy_cap) {
                        BYTE *bigger = (BYTE *)realloc(copy, size);
                        if (bigger) { copy = bigger; copy_cap = size; }
                    }
                    if (size <= copy_cap && copy) memcpy(copy, ws->nv12, size);
                } else {
                    size = 0;
                }
                ReleaseSRWLockShared(&ws->frame_lock);
                if (size && copy && size <= copy_cap) {
                    last_submit = now;
                    if (!web_encoder_submit(enc, copy)) {
                        web_encoder_destroy(enc);
                        enc = NULL;
                        enc_w = enc_h = 0;
                        InterlockedExchange(&ws->dirty, 1);
                        continue;
                    }
                }
            }
        }
        WaitForSingleObject(ws->frame_event, enc ? 2 : 50);
    }
    if (enc) web_encoder_destroy(enc);
    free(copy);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

/* ---- Audio ---- */

static DWORD WINAPI audio_thread(LPVOID param)
{
    WebSession *ws = (WebSession *)param;
    DWORD cap = 65536;
    BYTE *buf = (BYTE *)malloc(cap);
    short *pcm = (short *)malloc(cap);
    DWORD retry_ms = 1000;

    t_stop = &ws->stop;
    while (!ws->stop && buf && pcm) {
        AudioHeader h;
        DWORD waited;
        SOCKET s = hv_connect(&ws->runtime_id, ws->os_type, 4, 1000);
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }
        EnterCriticalSection(&ws->sock_cs);
        ws->audio_sock = s;
        LeaveCriticalSection(&ws->sock_cs);
        if (recv_all(s, &h, sizeof(h)) && h.magic == AUDIO_MAGIC && h.channels >= 1 && h.channels <= 8 &&
            (h.bits == 16 || h.bits == 32)) {
            BYTE head[5];
            UINT32 rate = h.rate;
            retry_ms = 1000;
            memcpy(head, &rate, 4);
            head[4] = (BYTE)(h.channels > 2 ? 2 : h.channels);
            while (!ws->stop) {
                UINT32 size, frames, i, out_ch = head[4], bytes_per = h.channels * h.bits / 8;
                DWORD queued;
                if (!recv_all(s, &size, 4) || size > 1048576) break;
                if (size > cap) {
                    BYTE *b = (BYTE *)realloc(buf, size);
                    short *p = (short *)realloc(pcm, size);
                    if (b) buf = b;
                    if (p) pcm = p;
                    if (!b || !p) break;
                    cap = size;
                }
                if (size && !recv_all(s, buf, (int)size)) break;
                EnterCriticalSection(&ws->q_cs);
                queued = ws->q_bytes;
                LeaveCriticalSection(&ws->q_cs);
                if (queued > AUDIO_QUEUE_BYTES) continue;   /* far behind: late sound is no use */
                frames = bytes_per ? size / bytes_per : 0;
                for (i = 0; i < frames; i++) {
                    UINT32 c;
                    for (c = 0; c < out_ch; c++) {
                        const BYTE *src = buf + i * bytes_per + c * (h.bits / 8);
                        int v;
                        if (h.bits == 16) {
                            v = *(const short *)src;
                        } else if (h.tag == 3) {
                            float f = *(const float *)src;
                            v = (int)(f * 32767.0f);
                        } else {
                            v = *(const int *)src >> 16;
                        }
                        pcm[i * out_ch + c] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                }
                if (frames) ws_send(ws, MSG_AUDIO, head, sizeof(head), pcm, frames * out_ch * 2);
            }
        } else if (retry_ms < 10000) {
            retry_ms *= 2;   /* no audio device in the guest: each try writes to its log */
        }
        EnterCriticalSection(&ws->sock_cs);
        ws->audio_sock = INVALID_SOCKET;
        LeaveCriticalSection(&ws->sock_cs);
        closesocket(s);
        for (waited = 0; waited < retry_ms && !ws->stop; waited += 200) Sleep(200);
    }
    free(buf);
    free(pcm);
    return 0;
}

/* ---- Clipboard, text only, and never the host's ---- */

static BOOL clip_send_list(WebSession *ws)
{
    ClipHeader h;
    UINT32 list[3];
    BOOL ok = FALSE;
    h.magic = CLIP_MAGIC;
    h.msg_type = CLIP_FORMAT_LIST;
    h.format = 0;
    h.size = sizeof(list);
    list[0] = 1;
    list[1] = CLIP_TEXT_FORMAT;
    list[2] = 0;
    EnterCriticalSection(&ws->sock_cs);
    if (ws->clip_write != INVALID_SOCKET) {
        InterlockedExchange(&ws->suppress_echo, 1);
        ok = raw_send_all(ws->clip_write, &h, sizeof(h)) && raw_send_all(ws->clip_write, list, sizeof(list));
    }
    LeaveCriticalSection(&ws->sock_cs);
    return ok;
}

static SRWLOCK g_clip_lock = SRWLOCK_INIT;

typedef struct { WebSession *ws; wchar_t *text; DWORD bytes; } ClipOut;
static ClipOut g_clip_out[ASB_MAX_VMS];

static ClipOut *clip_out_for(WebSession *ws)
{
    int i, free_slot = -1;
    for (i = 0; i < ASB_MAX_VMS; i++) {
        if (g_clip_out[i].ws == ws) return &g_clip_out[i];
        if (!g_clip_out[i].ws && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    g_clip_out[free_slot].ws = ws;
    return &g_clip_out[free_slot];
}

static void clip_from_browser(WebSession *ws, const char *utf8, DWORD len)
{
    int chars;
    wchar_t *w;
    ClipOut *o;
    if (len > CLIP_MAX_TEXT) return;
    chars = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    w = (wchar_t *)malloc(((size_t)chars + 1) * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, w, chars);
    w[chars] = 0;
    AcquireSRWLockExclusive(&g_clip_lock);
    o = clip_out_for(ws);
    if (o) {
        free(o->text);
        o->text = w;
        o->bytes = ((DWORD)chars + 1) * sizeof(wchar_t);
        w = NULL;
    }
    ReleaseSRWLockExclusive(&g_clip_lock);
    free(w);
    clip_send_list(ws);
}

static DWORD WINAPI clip_writer_thread(LPVOID param)
{
    WebSession *ws = (WebSession *)param;
    t_stop = &ws->stop;
    while (!ws->stop) {
        UINT32 ready = 0;
        ClipHeader h;
        BYTE enable = 1;
        SOCKET s = hv_connect(&ws->runtime_id, ws->os_type, 5, 2000);
        if (s == INVALID_SOCKET) { Sleep(1500); continue; }
        if (!recv_all(s, &ready, 4) || ready != CLIP_READY_MAGIC) {
            closesocket(s);
            Sleep(1500);
            continue;
        }
        EnterCriticalSection(&ws->sock_cs);
        ws->clip_write = s;
        h.magic = CLIP_MAGIC;
        h.msg_type = CLIP_SYNC_ENABLE;
        h.format = 0;
        h.size = 1;
        raw_send_all(s, &h, sizeof(h));
        raw_send_all(s, &enable, 1);
        LeaveCriticalSection(&ws->sock_cs);
        while (!ws->stop) {
            if (!recv_all(s, &h, sizeof(h)) || h.magic != CLIP_MAGIC) break;
            if (h.size) {
                /* Nothing the guest sends on this channel carries data we need. */
                BYTE skip[256];
                UINT32 left = h.size;
                BOOL ok = TRUE;
                while (left && ok) {
                    int chunk = left > sizeof(skip) ? (int)sizeof(skip) : (int)left;
                    ok = recv_all(s, skip, chunk);
                    left -= chunk;
                }
                if (!ok) break;
            }
            if (h.msg_type == CLIP_DATA_REQ) {
                ClipHeader r;
                ClipOut *o;
                r.magic = CLIP_MAGIC;
                r.msg_type = CLIP_DATA_RESP;
                r.format = h.format;
                r.size = 0;
                AcquireSRWLockShared(&g_clip_lock);
                o = NULL;
                {
                    int i;
                    for (i = 0; i < ASB_MAX_VMS; i++)
                        if (g_clip_out[i].ws == ws) o = &g_clip_out[i];
                }
                EnterCriticalSection(&ws->sock_cs);
                if (o && o->text && h.format == CLIP_TEXT_FORMAT) {
                    r.size = o->bytes;
                    raw_send_all(s, &r, sizeof(r));
                    raw_send_all(s, o->text, o->bytes);
                } else {
                    raw_send_all(s, &r, sizeof(r));
                }
                LeaveCriticalSection(&ws->sock_cs);
                ReleaseSRWLockShared(&g_clip_lock);
            }
        }
        EnterCriticalSection(&ws->sock_cs);
        ws->clip_write = INVALID_SOCKET;
        LeaveCriticalSection(&ws->sock_cs);
        closesocket(s);
        if (!ws->stop) Sleep(1500);
    }
    return 0;
}

static DWORD WINAPI clip_reader_thread(LPVOID param)
{
    WebSession *ws = (WebSession *)param;
    t_stop = &ws->stop;
    while (!ws->stop) {
        UINT32 ready = 0;
        ClipHeader h;
        SOCKET s = hv_connect(&ws->runtime_id, ws->os_type, 6, 2000);
        if (s == INVALID_SOCKET) { Sleep(1500); continue; }
        if (!recv_all(s, &ready, 4) || ready != CLIP_READY_MAGIC) {
            closesocket(s);
            Sleep(1500);
            continue;
        }
        EnterCriticalSection(&ws->sock_cs);
        ws->clip_read = s;
        LeaveCriticalSection(&ws->sock_cs);
        while (!ws->stop) {
            if (!recv_all(s, &h, sizeof(h)) || h.magic != CLIP_MAGIC || h.size > CLIP_MAX_TEXT * 4) break;
            if (h.msg_type == CLIP_FORMAT_LIST) {
                BYTE *list;
                UINT32 count, i, off = 4;
                BOOL has_text = FALSE;
                if (h.size < 4 || h.size > 16384) break;
                list = (BYTE *)malloc(h.size);
                if (!list) break;
                if (!recv_all(s, list, (int)h.size)) { free(list); break; }
                count = *(UINT32 *)list;
                for (i = 0; i < count && off + 8 <= h.size; i++) {
                    UINT32 id = *(UINT32 *)(list + off), name_len = *(UINT32 *)(list + off + 4);
                    off += 8;
                    if (name_len > h.size - off) break;
                    off += name_len;
                    if (id == CLIP_TEXT_FORMAT) has_text = TRUE;
                }
                free(list);
                if (InterlockedExchange(&ws->suppress_echo, 0)) continue;   /* our own text coming back */
                if (has_text) {
                    ClipHeader r;
                    r.magic = CLIP_MAGIC;
                    r.msg_type = CLIP_DATA_REQ;
                    r.format = CLIP_TEXT_FORMAT;
                    r.size = 0;
                    if (!raw_send_all(s, &r, sizeof(r))) break;
                }
            } else if (h.msg_type == CLIP_DATA_RESP) {
                BYTE *data = h.size ? (BYTE *)malloc((size_t)h.size + 2) : NULL;
                if (h.size && !data) break;
                if (h.size && !recv_all(s, data, (int)h.size)) { free(data); break; }
                if (data && h.format == CLIP_TEXT_FORMAT && h.size <= CLIP_MAX_TEXT * 2) {
                    int need;
                    char *utf8;
                    data[h.size] = 0;
                    data[h.size + 1] = 0;
                    need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)data, -1, NULL, 0, NULL, NULL);
                    utf8 = need > 0 ? (char *)malloc(need) : NULL;
                    if (utf8) {
                        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)data, -1, utf8, need, NULL, NULL);
                        ws_send(ws, MSG_CLIP_OUT, NULL, 0, utf8, (DWORD)need - 1);
                        free(utf8);
                    }
                }
                free(data);
            } else if (h.size) {
                BYTE skip[256];
                UINT32 left = h.size;
                BOOL ok = TRUE;
                while (left && ok) {
                    int chunk = left > sizeof(skip) ? (int)sizeof(skip) : (int)left;
                    ok = recv_all(s, skip, chunk);
                    left -= chunk;
                }
                if (!ok) break;
            }
        }
        EnterCriticalSection(&ws->sock_cs);
        ws->clip_read = INVALID_SOCKET;
        LeaveCriticalSection(&ws->sock_cs);
        closesocket(s);
        if (!ws->stop) Sleep(1500);
    }
    return 0;
}

/* ---- Input ---- */

static void input_send(WebSession *ws, const InputPacket *p)
{
    SOCKET s;
    EnterCriticalSection(&ws->sock_cs);
    s = ws->input_sock;
    if (s == INVALID_SOCKET) {
        UINT32 ready = 0;
        LeaveCriticalSection(&ws->sock_cs);
        DWORD wait_ms = 2000, none = 0;
        s = hv_connect(&ws->runtime_id, ws->os_type, 3, 500);
        if (s == INVALID_SOCKET) return;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait_ms, sizeof(wait_ms));
        if (!recv_all(s, &ready, 4) || ready != INPUT_READY_MAGIC) { closesocket(s); return; }
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&none, sizeof(none));
        EnterCriticalSection(&ws->sock_cs);
        ws->input_sock = s;
    }
    if (send(s, (const char *)p, sizeof(*p), 0) != (int)sizeof(*p)) {
        closesocket(s);
        ws->input_sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&ws->sock_cs);
}

/* Reads client messages until the browser goes away. */
static void handle_client_message(WebSession *ws, const BYTE *m, DWORD len)
{
    if (len < 1) return;
    switch (m[0]) {
    case MSG_INPUT:
        if (len >= 17) {
            InputPacket p;
            UINT32 v[4];
            memcpy(v, m + 1, 16);
            if (v[0] > 5) return;   /* INPUT_MOUSE_MOVE..INPUT_MOUSE_HWHEEL */
            p.magic = INPUT_MAGIC;
            p.type = v[0];
            p.p1 = v[1];
            p.p2 = v[2];
            p.p3 = v[3];
            input_send(ws, &p);
        }
        break;
    case MSG_RESIZE:
        if (len >= 9) {
            UINT32 w, h;
            memcpy(&w, m + 1, 4);
            memcpy(&h, m + 5, 4);
            w &= ~1u;
            h &= ~1u;
            if (w < MIN_W) w = MIN_W;
            if (h < MIN_H) h = MIN_H;
            if (w > MAX_SIDE) w = MAX_SIDE;
            if (h > MAX_SIDE) h = MAX_SIDE;
            if ((UINT64)w * h > MAX_PIXELS) h = (MAX_PIXELS / w) & ~1u;
            InterlockedExchange(&ws->resize_w, (LONG)w);
            InterlockedExchange(&ws->resize_h, (LONG)h);
            send_resize(ws);
        }
        break;
    case MSG_CLIP:
        clip_from_browser(ws, (const char *)m + 1, len - 1);
        break;
    case MSG_KEYREQ:
        InterlockedExchange(&ws->want_key, 1);
        SetEvent(ws->frame_event);
        break;
    case MSG_BITRATE:
        if (len >= 5) {
            UINT32 mbps;
            memcpy(&mbps, m + 1, 4);
            if (mbps <= 200) InterlockedExchange(&ws->bitrate_mbps, (LONG)mbps);
        }
        break;
    default:
        break;
    }
}

static void ws_read_loop(WebSession *ws)
{
    WebConn *c = ws->conn;
    BYTE *msg = (BYTE *)malloc(65536);
    DWORD msg_cap = 65536, msg_len = 0;
    BYTE rx[16384];
    DWORD have = 0;

    while (msg && !ws->stop) {
        /* One frame at a time from a buffered reader. */
        DWORD need = 2, pos;
        UINT64 payload;
        BYTE mask[4];
        BYTE fin, opcode;
        UINT64 i;
        #define FILL(n) while (have < (n)) { int got = conn_recv(c, rx + have, (int)(sizeof(rx) - have)); if (got <= 0) goto done; have += (DWORD)got; }
        FILL(2);
        fin = rx[0] & 0x80;
        opcode = rx[0] & 0x0F;
        if (!(rx[1] & 0x80)) break;   /* a browser must mask */
        payload = rx[1] & 0x7F;
        need = 2;
        if (payload == 126) need += 2;
        else if (payload == 127) need += 8;
        need += 4;
        FILL(need);
        pos = 2;
        if (payload == 126) { payload = ((UINT64)rx[2] << 8) | rx[3]; pos = 4; }
        else if (payload == 127) {
            int b;
            payload = 0;
            for (b = 0; b < 8; b++) payload = (payload << 8) | rx[2 + b];
            pos = 10;
        }
        memcpy(mask, rx + pos, 4);
        pos += 4;
        if (msg_len + payload > WEB_MAX_MESSAGE) break;
        if (msg_len + payload > msg_cap) {
            BYTE *bigger = (BYTE *)realloc(msg, (size_t)(msg_len + payload));
            if (!bigger) break;
            msg = bigger;
            msg_cap = (DWORD)(msg_len + payload);
        }
        /* Payload: first what's buffered, then read straight in. */
        {
            DWORD buffered = have - pos, take = payload < buffered ? (DWORD)payload : buffered;
            DWORD filled = take;
            memcpy(msg + msg_len, rx + pos, take);
            memmove(rx, rx + pos + take, have - pos - take);
            have = have - pos - take;
            while (filled < payload) {
                int got = conn_recv(c, msg + msg_len + filled, (int)(payload - filled));
                if (got <= 0) goto done;
                filled += (DWORD)got;
            }
        }
        for (i = 0; i < payload; i++) msg[msg_len + i] ^= mask[i & 3];
        if (opcode == 8) break;   /* close */
        if (opcode == 9 || opcode == 10) continue;   /* browsers don't ping unprompted */
        msg_len += (DWORD)payload;
        if (fin) {
            handle_client_message(ws, msg, msg_len);
            msg_len = 0;
        }
        #undef FILL
    }
done:
    free(msg);
}

static void close_if_open(WebSession *ws, SOCKET *s)
{
    EnterCriticalSection(&ws->sock_cs);
    if (*s != INVALID_SOCKET) shutdown(*s, SD_BOTH);
    LeaveCriticalSection(&ws->sock_cs);
}

static void session_run(WebConn *conn, UINT64 vm_id)
{
    WebSession *ws = (WebSession *)calloc(1, sizeof(*ws));
    VmInstance *v;
    WebVm *entry;
    BOOL stuck = FALSE;
    int i;

    if (!ws) return;
    v = vm_by_id(vm_id);
    if (!v) { free(ws); return; }
    ws->vm_id = vm_id;
    ws->runtime_id = v->runtime_id;
    wcsncpy_s(ws->os_type, 32, v->os_type, _TRUNCATE);
    ws->conn = conn;
    InitializeCriticalSection(&ws->q_cs);
    InitializeConditionVariable(&ws->q_cv);
    InitializeSRWLock(&ws->frame_lock);
    InitializeCriticalSection(&ws->sock_cs);
    ws->frame_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ws->frame_sock = ws->input_sock = ws->audio_sock = ws->clip_write = ws->clip_read = INVALID_SOCKET;

    /* The newest browser wins: tell an earlier one, and wait for it to let go of
       the guest's channels, which serve one client at a time. A stalled one gives
       up in its send timeout. */
    AcquireSRWLockExclusive(&g_lock);
    entry = web_vm_find_id(vm_id);
    if (entry && entry->session) {
        static const char replaced[] = "{\"replaced\":true}";
        QItem *item = ws_build(MSG_STATUS, replaced, sizeof(replaced) - 1, NULL, 0);
        if (item) {
            item->last = TRUE;
            q_push(entry->session, item);
        } else {
            InterlockedExchange(&entry->session->stop, 1);
            shutdown(entry->session->conn->s, SD_BOTH);
        }
        entry->session = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (i = 0; i < 240; i++) {
        v = vm_by_id(vm_id);
        if (!v || !v->web_session_active) break;
        Sleep(50);
    }
    AcquireSRWLockExclusive(&g_lock);
    entry = web_vm_find_id(vm_id);
    if (entry) entry->session = ws;
    /* The display window drops its connections when it sees this. */
    v = vm_by_id(vm_id);
    if (v && entry) InterlockedIncrement(&v->web_session_active);
    ReleaseSRWLockExclusive(&g_lock);
    if (!entry || !v) goto cleanup;
    ui_log(L"Browser access: a browser connected to \"%s\".", v->name);
    Sleep(300);

    ws->threads[ws->thread_count++] = CreateThread(NULL, 0, writer_thread, ws, 0, NULL);
    ws->threads[ws->thread_count++] = CreateThread(NULL, 0, frame_thread, ws, 0, NULL);
    ws->threads[ws->thread_count++] = CreateThread(NULL, 0, encode_thread, ws, 0, NULL);
    ws->threads[ws->thread_count++] = CreateThread(NULL, 0, audio_thread, ws, 0, NULL);
    ws->threads[ws->thread_count++] = CreateThread(NULL, 0, clip_writer_thread, ws, 0, NULL);
    ws->threads[ws->thread_count++] = CreateThread(NULL, 0, clip_reader_thread, ws, 0, NULL);

    ws_read_loop(ws);

    InterlockedExchange(&ws->stop, 1);
    shutdown(conn->s, SD_BOTH);
    close_if_open(ws, &ws->frame_sock);
    close_if_open(ws, &ws->audio_sock);
    close_if_open(ws, &ws->clip_write);
    close_if_open(ws, &ws->clip_read);
    SetEvent(ws->frame_event);
    EnterCriticalSection(&ws->q_cs);
    WakeAllConditionVariable(&ws->q_cv);
    LeaveCriticalSection(&ws->q_cs);
    for (i = 0; i < ws->thread_count; i++) {
        if (ws->threads[i]) {
            /* A thread parked in a connect gives up within its timeout. */
            if (WaitForSingleObject(ws->threads[i], 10000) != WAIT_OBJECT_0) stuck = TRUE;
            CloseHandle(ws->threads[i]);
        }
    }
    if (ws->input_sock != INVALID_SOCKET) closesocket(ws->input_sock);

    AcquireSRWLockExclusive(&g_lock);
    entry = web_vm_find_id(vm_id);
    if (entry && entry->session == ws) entry->session = NULL;
    v = vm_by_id(vm_id);
    if (v) InterlockedDecrement(&v->web_session_active);
    ReleaseSRWLockExclusive(&g_lock);

    if (stuck) {
        /* Still running on this memory: losing it beats freeing it under a thread. */
        ui_log(L"Browser access: a session thread didn't stop in time.");
        return;
    }

cleanup:
    {
        QItem *q = ws->q_head;
        while (q) { QItem *next = q->next; free(q); q = next; }
    }
    AcquireSRWLockExclusive(&g_clip_lock);
    {
        ClipOut *o = NULL;
        for (i = 0; i < ASB_MAX_VMS; i++) if (g_clip_out[i].ws == ws) o = &g_clip_out[i];
        if (o) { free(o->text); o->text = NULL; o->ws = NULL; }
    }
    ReleaseSRWLockExclusive(&g_clip_lock);
    free(ws->bgra);
    free(ws->nv12);
    CloseHandle(ws->frame_event);
    DeleteCriticalSection(&ws->q_cs);
    DeleteCriticalSection(&ws->sock_cs);
    free(ws);
}

/* ---- HTTP ---- */

static const char g_security_headers[] =
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Cache-Control: no-store\r\n"
    "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data:; connect-src 'self'; worker-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n";

static void http_reply(WebConn *c, const char *status, const char *type, const char *extra,
                       const void *body, DWORD body_len)
{
    char head[1400];
    int n = sprintf_s(head, sizeof(head),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n%s%s\r\n",
        status, type, (unsigned long)body_len, g_security_headers, extra ? extra : "");
    conn_send(c, head, (DWORD)n);
    if (body_len) conn_send(c, body, body_len);
}

static void http_text(WebConn *c, const char *status, const char *text)
{
    http_reply(c, status, "text/plain; charset=utf-8", NULL, text, (DWORD)strlen(text));
}

/* The value of a header, into out. Names compare case-insensitively. */
static BOOL http_header(const char *headers, const char *name, char *out, int cap)
{
    size_t n = strlen(name);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if ((size_t)(eol - p) > n && _strnicmp(p, name, n) == 0 && p[n] == ':') {
            const char *v = p + n + 1;
            int len;
            while (*v == ' ' || *v == '\t') v++;
            len = (int)(eol - v);
            if (len >= cap) return FALSE;
            memcpy(out, v, len);
            out[len] = 0;
            return TRUE;
        }
        p = eol + 2;
    }
    return FALSE;
}

/* Guards against DNS rebinding: on loopback only the loopback names count, and on the
   network only an address, or this PC's own name, never a name someone else controls. */
static BOOL host_allowed(const char *host, BOOL tls)
{
    char name[128];
    const char *colon = strrchr(host, ':');
    size_t len = colon ? (size_t)(colon - host) : strlen(host);
    int port = colon ? atoi(colon + 1) : 0;
    char computer[64] = "";
    DWORD size = sizeof(computer);
    size_t i;
    BOOL dotted = TRUE;

    if (len == 0 || len >= sizeof(name)) return FALSE;
    memcpy(name, host, len);
    name[len] = 0;
    if (!tls) return port == g_port_local && (strcmp(name, "127.0.0.1") == 0 || _stricmp(name, "localhost") == 0);
    if (port != g_port_lan) return FALSE;
    for (i = 0; i < len; i++) if (!((name[i] >= '0' && name[i] <= '9') || name[i] == '.')) dotted = FALSE;
    if (dotted) return TRUE;
    GetComputerNameA(computer, &size);
    if (_stricmp(name, computer) == 0) return TRUE;
    {
        char local[80];
        sprintf_s(local, sizeof(local), "%s.local", computer);
        return _stricmp(name, local) == 0;
    }
}

/* A request from a page must come from one of ours. */
static BOOL origin_allowed(const char *headers, const char *host, BOOL tls)
{
    char origin[200], expect[200];
    if (!http_header(headers, "Origin", origin, sizeof(origin))) return TRUE;   /* no page involved */
    sprintf_s(expect, sizeof(expect), "%s://%s", tls ? "https" : "http", host);
    return _stricmp(origin, expect) == 0;
}

static void serve_file(WebConn *c, const wchar_t *leaf, const char *type)
{
    wchar_t path[MAX_PATH], *slash;
    HANDLE f;
    DWORD size, got = 0;
    BYTE *data;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(path, MAX_PATH, L"web\\");
    wcscat_s(path, MAX_PATH, leaf);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { http_text(c, "404 Not Found", "Not found"); return; }
    size = GetFileSize(f, NULL);
    data = (BYTE *)malloc(size ? size : 1);
    if (data && ReadFile(f, data, size, &got, NULL) && got == size)
        http_reply(c, "200 OK", type, NULL, data, size);
    else
        http_text(c, "500 Internal Server Error", "Could not read the file");
    free(data);
    CloseHandle(f);
}

static BOOL login_allowed(ULONG ip)
{
    int i;
    ULONGLONG now = GetTickCount64();
    BOOL ok = TRUE;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < 32; i++) {
        if (g_limits[i].ip == ip) {
            if (now - g_limits[i].since > LOGIN_WINDOW_MS) { g_limits[i].fails = 0; g_limits[i].since = now; }
            ok = g_limits[i].fails < LOGIN_FAILS_MAX;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

static void login_failed(ULONG ip)
{
    int i, oldest = 0;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < 32; i++) {
        if (g_limits[i].ip == ip) {
            g_limits[i].fails++;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (g_limits[i].since < g_limits[oldest].since) oldest = i;
    }
    g_limits[oldest].ip = ip;
    g_limits[oldest].fails = 1;
    g_limits[oldest].since = now;
    ReleaseSRWLockExclusive(&g_lock);
}

/* A session for this VM and its Set-Cookie header. */
static BOOL mint_vm_cookie(UINT64 vm_id, BOOL tls, char *cookie, size_t cookie_chars)
{
    char token[65];
    int i, slot = 0;
    ULONGLONG now = GetTickCount64(), oldest = ~0ull;
    if (!random_hex(token, 64)) return FALSE;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (g_cookies[i].expires < oldest) { oldest = g_cookies[i].expires; slot = i; }
    }
    strcpy_s(g_cookies[slot].token, sizeof(g_cookies[slot].token), token);
    g_cookies[slot].vm_id = vm_id;
    g_cookies[slot].tls = tls;
    g_cookies[slot].expires = now + WEB_SESSION_MS;
    ReleaseSRWLockExclusive(&g_lock);
    sprintf_s(cookie, cookie_chars, "Set-Cookie: asb_%llx=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n",
              (unsigned long long)vm_id, token, tls ? "; Secure" : "");
    SecureZeroMemory(token, sizeof(token));
    return TRUE;
}

/* POST /api/login: body "<vm name>\n<key>". */
static void handle_login(WebConn *c, const char *body, DWORD body_len, ULONG ip)
{
    char copy[1024], *nl, cookie[256], json[64];
    WebVm *entry;
    UINT64 vm_id = 0;

    if (!login_allowed(ip)) {
        Sleep(1000);
        http_text(c, "429 Too Many Requests", "Too many tries. Wait a minute and try again.");
        return;
    }
    if (body_len >= sizeof(copy)) { http_text(c, "400 Bad Request", "Bad request"); return; }
    memcpy(copy, body, body_len);
    copy[body_len] = 0;
    nl = strchr(copy, '\n');
    if (!nl) { http_text(c, "400 Bad Request", "Bad request"); return; }
    *nl = 0;

    AcquireSRWLockShared(&g_lock);
    entry = web_vm_find_name(copy);
    if (entry && equal_secret(entry->key, nl + 1) && (!c->tls || entry->lan))
        vm_id = entry->vm_id;
    ReleaseSRWLockShared(&g_lock);
    SecureZeroMemory(copy, sizeof(copy));
    if (!vm_id) {
        login_failed(ip);
        ui_log(L"Browser access: a login attempt failed.");
        http_text(c, "403 Forbidden", "That access key isn't right, or browser access is off for this VM.");
        return;
    }
    if (!mint_vm_cookie(vm_id, c->tls, cookie, sizeof(cookie))) { http_text(c, "500 Internal Server Error", "Random numbers failed"); return; }
    sprintf_s(json, sizeof(json), "{\"ok\":true}");
    http_reply(c, "200 OK", "application/json", cookie, json, (DWORD)strlen(json));
}

/* Does the Cookie header hold a live session for this VM on this kind of connection? */
static BOOL session_cookie_ok(const char *headers, UINT64 vm_id, BOOL tls)
{
    char cookies[1024], name[40], *p;
    char token[65] = "";
    BOOL ok = FALSE;
    int i;
    ULONGLONG now = GetTickCount64();
    if (!http_header(headers, "Cookie", cookies, sizeof(cookies))) return FALSE;
    sprintf_s(name, sizeof(name), "asb_%llx=", (unsigned long long)vm_id);
    p = strstr(cookies, name);
    if (!p || (p != cookies && p[-1] != ' ' && p[-1] != ';')) return FALSE;
    p += strlen(name);
    for (i = 0; i < 64 && p[i] && p[i] != ';'; i++) token[i] = p[i];
    token[i] = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (g_cookies[i].vm_id == vm_id && g_cookies[i].tls == tls && g_cookies[i].expires > now &&
            equal_secret(g_cookies[i].token, token)) {
            ok = TRUE;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

/* ---- The Game Streaming dashboard ----

   Same checks as the rest of the gateway: Host and Origin first, then its own sign-in.
   A domain password arrives here only in a POST body, and on the network only over
   HTTPS, because that is the only network listener. */

static BOOL app_cookie_token(const char *headers, char *token, size_t chars)
{
    char cookies[1024], *p;
    size_t i = 0;
    if (!http_header(headers, "Cookie", cookies, sizeof(cookies))) return FALSE;
    p = strstr(cookies, "asb_app=");
    if (!p || (p != cookies && p[-1] != ' ' && p[-1] != ';')) return FALSE;
    p += 8;
    while (i < chars - 1 && *p && *p != ';') token[i++] = *p++;
    token[i] = 0;
    return i == 64;
}

/* Split "a\nb\nc" in place. Returns how many fields. */
static int split_lines(char *text, char **fields, int max)
{
    int n = 0;
    fields[n++] = text;
    for (; *text && n < max; text++)
        if (*text == '\n') { *text = 0; fields[n++] = text + 1; }
    return n;
}

static void dashboard_login(WebConn *c, char *body, ULONG ip)
{
    char *f[2], token[65], cookie[200], message[400] = "";
    wchar_t user[24] = L"", password[128] = L"";
    if (!login_allowed(ip)) {
        Sleep(1000);
        http_text(c, "429 Too Many Requests", "Too many tries. Wait a minute and try again.");
        return;
    }
    if (split_lines(body, f, 2) != 2 ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, f[0], -1, user, _countof(user)) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, f[1], -1, password, _countof(password))) {
        http_text(c, "400 Bad Request", "Bad request");
        return;
    }
    if (FAILED(game_stream_sign_in(user, password, c->tls, token, sizeof(token), message, sizeof(message)))) {
        SecureZeroMemory(password, sizeof(password));
        login_failed(ip);
        ui_log(L"Game Streaming: a sign-in failed.");
        http_text(c, "403 Forbidden", message[0] ? message : "That sign-in wasn't accepted.");
        return;
    }
    SecureZeroMemory(password, sizeof(password));
    ui_log(L"Game Streaming: %s signed in.", user);
    sprintf_s(cookie, sizeof(cookie), "Set-Cookie: asb_app=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n",
              token, c->tls ? "; Secure" : "");
    SecureZeroMemory(token, sizeof(token));
    http_reply(c, "200 OK", "application/json", cookie, "{\"ok\":true}", 11);
}

static void dashboard_serve_catalog_file(WebConn *c, const char *leaf, const char *type)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    DWORD size, got = 0;
    BYTE *data;
    if (!game_stream_catalog_file(leaf, path, MAX_PATH)) { http_text(c, "404 Not Found", "Not found"); return; }
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { http_text(c, "404 Not Found", "Not found"); return; }
    size = GetFileSize(f, NULL);
    data = size < 8u * 1024u * 1024u ? (BYTE *)malloc(size ? size : 1) : NULL;
    if (data && ReadFile(f, data, size, &got, NULL) && got == size)
        http_reply(c, "200 OK", type, NULL, data, size);
    else
        http_text(c, "500 Internal Server Error", "Could not read the file");
    free(data);
    CloseHandle(f);
}

/* Routes under /api/app/ for a signed-in user; body may be NULL on a GET. */
static void dashboard_api(WebConn *c, const char *path, BOOL post, const char *headers, char *body, ULONG ip)
{
    char token[80] = "", message[500] = "";
    wchar_t user[24];

    if (post && strcmp(path, "/api/app/login") == 0) {
        dashboard_login(c, body, ip);
        return;
    }
    if (!app_cookie_token(headers, token, sizeof(token)) || !game_stream_signed_in(token, c->tls, user, _countof(user))) {
        http_text(c, "401 Unauthorized", "Sign in first.");
        return;
    }
    if (post && strcmp(path, "/api/app/logout") == 0) {
        game_stream_sign_out(token);
        sprintf_s(message, sizeof(message), "Set-Cookie: asb_app=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict%s\r\n",
                  c->tls ? "; Secure" : "");
        http_reply(c, "200 OK", "application/json", message, "{\"ok\":true}", 11);
    } else if (!post && strcmp(path, "/api/app/catalog") == 0) {
        dashboard_serve_catalog_file(c, "catalog.json", "application/json");
    } else if (!post && strncmp(path, "/api/app/art/", 13) == 0) {
        dashboard_serve_catalog_file(c, path + 13, "image/jpeg");
    } else if (!post && strcmp(path, "/api/app/session") == 0) {
        char json[2048];
        int n = game_stream_session_json(user, json, sizeof(json));
        http_reply(c, "200 OK", "application/json", NULL, json, (DWORD)n);
    } else if (post && strcmp(path, "/api/app/session") == 0) {
        /* "<browser|moonlight>\n<item>", plus the password when the kept one has lapsed. */
        char *f[3] = { NULL, NULL, NULL };
        wchar_t password[128] = L"";
        int n = split_lines(body, f, 3);
        HRESULT hr;
        if (n < 2) { http_text(c, "400 Bad Request", "Bad request"); return; }
        if (n == 3) {
            if (!login_allowed(ip)) {
                http_text(c, "429 Too Many Requests", "Too many tries. Wait a minute and try again.");
                return;
            }
            MultiByteToWideChar(CP_UTF8, 0, f[2], -1, password, _countof(password));
        }
        hr = game_stream_session_start(token, user, f[0], f[1], n == 3 ? password : NULL, message, sizeof(message));
        SecureZeroMemory(password, sizeof(password));
        if (hr == E_ACCESSDENIED) login_failed(ip);
        if (FAILED(hr)) {
            http_text(c, strcmp(message, "password") == 0 ? "401 Unauthorized" : "409 Conflict",
                      message[0] ? message : "The session couldn't start.");
            return;
        }
        http_reply(c, "200 OK", "application/json", NULL, "{\"ok\":true}", 11);
    } else if (post && strcmp(path, "/api/app/session/end") == 0) {
        game_stream_session_end(user);
        http_reply(c, "200 OK", "application/json", NULL, "{\"ok\":true}", 11);
    } else if (post && strcmp(path, "/api/app/session/pair") == 0) {
        if (FAILED(game_stream_session_pair(user, body, message, sizeof(message)))) {
            http_text(c, "409 Conflict", message[0] ? message : "Pairing failed. Check the PIN and try again from Moonlight.");
            return;
        }
        http_reply(c, "200 OK", "application/json", NULL, "{\"ok\":true}", 11);
    } else if (post && strcmp(path, "/api/app/session/open") == 0) {
        /* Browser view without a VM key: the dashboard's sign-in vouches for it. */
        char name[80] = "", cookie[200], json[160];
        UINT64 vm_id = game_stream_session_vm(user, name, sizeof(name));
        if (!vm_id) { http_text(c, "409 Conflict", "Your session isn't ready yet."); return; }
        if (!mint_vm_cookie(vm_id, c->tls, cookie, sizeof(cookie))) { http_text(c, "500 Internal Server Error", "Random numbers failed"); return; }
        sprintf_s(json, sizeof(json), "{\"path\":\"/vm/%s\"}", name);
        http_reply(c, "200 OK", "application/json", cookie, json, (DWORD)strlen(json));
    } else {
        http_text(c, "404 Not Found", "Not found");
    }
}

/* base64(SHA-1(key + GUID)), for the WebSocket handshake. */
static BOOL ws_accept_key(const char *key, char *out, DWORD cap)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BYTE hash[20];
    DWORD size = sizeof(hash);
    char joined[128];
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    BOOL ok = FALSE;
    sprintf_s(joined, sizeof(joined), "%s%s", key, guid);
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &h)) {
            if (CryptHashData(h, (const BYTE *)joined, (DWORD)strlen(joined), 0) &&
                CryptGetHashParam(h, HP_HASHVAL, hash, &size, 0))
                ok = CryptBinaryToStringA(hash, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &cap);
            CryptDestroyHash(h);
        }
        CryptReleaseContext(prov, 0);
    }
    return ok;
}

static DWORD WINAPI http_conn_thread(LPVOID param)
{
    WebConn *c = (WebConn *)param;
    char *req = (char *)malloc(WEB_MAX_HEADER + 1);
    DWORD have = 0;
    char *end = NULL, *line_end, method[8], path[1600], host[128] = "";
    struct sockaddr_in peer;
    int peer_len = sizeof(peer);
    ULONG ip = 0;
    DWORD timeout = 15000;
    int on = 1;

    setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    if (getpeername(c->s, (struct sockaddr *)&peer, &peer_len) == 0) ip = peer.sin_addr.s_addr;
    if (!req || (c->tls && !tls_handshake(c))) goto out;

    while (!end) {
        int n;
        if (have >= WEB_MAX_HEADER) { http_text(c, "431 Request Header Fields Too Large", "Too large"); goto out; }
        n = conn_recv(c, req + have, (int)(WEB_MAX_HEADER - have));
        if (n <= 0) goto out;
        have += (DWORD)n;
        req[have] = 0;
        end = strstr(req, "\r\n\r\n");
    }
    line_end = strstr(req, "\r\n");
    if (sscanf_s(req, "%7s %1599s", method, (unsigned)sizeof(method), path, (unsigned)sizeof(path)) != 2) goto out;
    {
        const char *headers = line_end + 2;
        BOOL get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
        if (!http_header(headers, "Host", host, sizeof(host)) || !host_allowed(host, c->tls)) {
            http_text(c, "403 Forbidden", "This address isn't allowed.");
            goto out;
        }
        if (!origin_allowed(headers, host, c->tls)) {
            http_text(c, "403 Forbidden", "Requests from other sites aren't allowed.");
            goto out;
        }

        if (get && strncmp(path, "/vm/", 4) == 0) {
            serve_file(c, L"remote.html", "text/html; charset=utf-8");
        } else if (get && strcmp(path, "/remote.js") == 0) {
            serve_file(c, L"remote.js", "text/javascript; charset=utf-8");
        } else if (get && strcmp(path, "/remote-audio.js") == 0) {
            serve_file(c, L"remote-audio.js", "text/javascript; charset=utf-8");
        } else if (get && strcmp(path, "/remote.css") == 0) {
            serve_file(c, L"remote.css", "text/css; charset=utf-8");
        } else if (get && g_dashboard && (!c->tls || g_dashboard_lan) && strcmp(path, "/") == 0) {
            serve_file(c, L"dash.html", "text/html; charset=utf-8");
        } else if (get && g_dashboard && (!c->tls || g_dashboard_lan) && strcmp(path, "/dash.js") == 0) {
            serve_file(c, L"dash.js", "text/javascript; charset=utf-8");
        } else if (get && g_dashboard && (!c->tls || g_dashboard_lan) && strcmp(path, "/dash.css") == 0) {
            serve_file(c, L"dash.css", "text/css; charset=utf-8");
        } else if (post && (strcmp(path, "/api/login") == 0 ||
                            (strncmp(path, "/api/app/", 9) == 0 && g_dashboard && (!c->tls || g_dashboard_lan)))) {
            char length[16];
            DWORD body_len, body_have;
            char *body = end + 4;
            if (!http_header(headers, "Content-Length", length, sizeof(length)) ||
                (body_len = (DWORD)atoi(length)) >= 1024) {
                http_text(c, "400 Bad Request", "Bad request");
                goto out;
            }
            body_have = have - (DWORD)(body - req);
            while (body_have < body_len) {
                int n = conn_recv(c, req + have, (int)(WEB_MAX_HEADER - have));
                if (n <= 0) goto out;
                have += (DWORD)n;
                body_have += (DWORD)n;
                if (have >= WEB_MAX_HEADER) goto out;
            }
            if (strcmp(path, "/api/login") == 0) {
                handle_login(c, body, body_len, ip);
            } else {
                body[body_len] = 0;
                dashboard_api(c, path, TRUE, headers, body, ip);
                SecureZeroMemory(body, body_len);
            }
        } else if (get && strncmp(path, "/api/app/", 9) == 0 && g_dashboard && (!c->tls || g_dashboard_lan)) {
            dashboard_api(c, path, FALSE, headers, NULL, ip);
        } else if (get && strncmp(path, "/ws/", 4) == 0) {
            UINT64 vm_id = 0;
            char upgrade[32], key[64], accept_key[64], reply[256];
            if (!url_decode(path + 4)) {
                http_text(c, "400 Bad Request", "Bad request");
                goto out;
            }
            AcquireSRWLockShared(&g_lock);
            {
                WebVm *entry = web_vm_find_name(path + 4);
                if (entry && (!c->tls || entry->lan)) vm_id = entry->vm_id;
            }
            ReleaseSRWLockShared(&g_lock);
            if (!vm_id || !session_cookie_ok(headers, vm_id, c->tls)) {
                http_text(c, "401 Unauthorized", "Sign in first.");
                goto out;
            }
            if (!http_header(headers, "Upgrade", upgrade, sizeof(upgrade)) || _stricmp(upgrade, "websocket") != 0 ||
                !http_header(headers, "Sec-WebSocket-Key", key, sizeof(key)) ||
                !ws_accept_key(key, accept_key, sizeof(accept_key))) {
                http_text(c, "400 Bad Request", "Expected a WebSocket.");
                goto out;
            }
            {
                int n = sprintf_s(reply, sizeof(reply),
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
                if (!conn_send(c, reply, (DWORD)n)) goto out;
            }
            timeout = 0;
            setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            {
                int sndbuf = 1024 * 1024;
                DWORD send_timeout = 10000;
                setsockopt(c->s, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
                setsockopt(c->s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));
            }
            free(req);
            req = NULL;
            session_run(c, vm_id);
        } else {
            http_text(c, "404 Not Found", "Not found");
        }
    }
out:
    free(req);
    conn_free(c);
    InterlockedDecrement(&g_conns);
    return 0;
}

static DWORD WINAPI accept_thread(LPVOID param)
{
    const BOOL tls = param != NULL;
    SOCKET listener = tls ? g_listen_lan : g_listen_local;
    while (!g_shutdown) {
        SOCKET s = accept(listener, NULL, NULL);
        WebConn *c;
        if (listener != (tls ? g_listen_lan : g_listen_local)) {
            if (s != INVALID_SOCKET) closesocket(s);
            break;
        }
        HANDLE t = NULL;
        if (s == INVALID_SOCKET) {
            if (g_shutdown || WSAGetLastError() == WSAENOTSOCK) break;
            Sleep(100);
            continue;
        }
        if (g_conns >= WEB_MAX_CONNS) { closesocket(s); continue; }
        c = (WebConn *)calloc(1, sizeof(*c));
        if (c) {
            c->s = s;
            c->tls = tls;
            InitializeSRWLock(&c->send_lock);
            InitializeCriticalSection(&c->tls_cs);
            InterlockedIncrement(&g_conns);
            t = CreateThread(NULL, 0, http_conn_thread, c, 0, NULL);
            if (t) CloseHandle(t);
            else { InterlockedDecrement(&g_conns); DeleteCriticalSection(&c->tls_cs); free(c); }
        }
        if (!t) closesocket(s);
    }
    return 0;
}

static SOCKET listen_on(ULONG addr, int first_port, int *chosen)
{
    int i;
    for (i = 0; i < WEB_PORT_TRIES; i++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a;
        BOOL exclusive = TRUE;
        /* The network port stays above the loopback port, two apart. */
        int port = first_port + i * 2;
        if (s == INVALID_SOCKET) return s;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
        ZeroMemory(&a, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = addr;
        a.sin_port = htons((u_short)port);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 16) == 0) {
            *chosen = port;
            return s;
        }
        closesocket(s);
    }
    return INVALID_SOCKET;
}

static BOOL ensure_local_listener(void)
{
    WSADATA wsa;
    HANDLE t;
    if (g_listen_local != INVALID_SOCKET) return TRUE;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_shutdown = 0;
    g_listen_local = listen_on(htonl(INADDR_LOOPBACK), WEB_PORT_FIRST, &g_port_local);
    if (g_listen_local == INVALID_SOCKET) return FALSE;
    t = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}

static BOOL any_lan_vm(void)
{
    int i;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (g_vms_web[i].vm_id && g_vms_web[i].lan) return TRUE;
    return g_dashboard && g_dashboard_lan;
}

static BOOL ensure_lan_listener(void)
{
    wchar_t cmd[300];
    HANDLE t;
    if (g_listen_lan != INVALID_SOCKET) return TRUE;
    if (!tls_init()) return FALSE;
    g_listen_lan = listen_on(htonl(INADDR_ANY), WEB_LAN_PORT_FIRST, &g_port_lan);
    if (g_listen_lan == INVALID_SOCKET) return FALSE;
    swprintf_s(cmd, 300, L"netsh advfirewall firewall delete rule name=\"%s\"", FIREWALL_RULE);
    run_hidden(cmd);
    swprintf_s(cmd, 300, L"netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow "
                         L"protocol=TCP localport=%d profile=private,domain", FIREWALL_RULE, g_port_lan);
    run_hidden(cmd);
    t = CreateThread(NULL, 0, accept_thread, (LPVOID)1, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}

static void close_lan_listener(void)
{
    wchar_t cmd[200];
    if (g_listen_lan == INVALID_SOCKET) return;
    closesocket(g_listen_lan);
    g_listen_lan = INVALID_SOCKET;
    swprintf_s(cmd, 200, L"netsh advfirewall firewall delete rule name=\"%s\"", FIREWALL_RULE);
    run_hidden(cmd);
}

/* ---- API ---- */

const wchar_t *vm_web_unsupported_reason(VmInstance *vm)
{
    if (!vm) return L"Unknown VM.";
    if (vm->lab_mode) return L"Malware lab VMs are isolated, so they can't be used from a browser.";
    if (vm->is_template || vm->is_appliance) return L"Only regular VMs can be used from a browser.";
    return NULL;
}

HRESULT vm_web_set(VmInstance *vm, BOOL enabled, BOOL lan, wchar_t *err, size_t err_chars)
{
    const wchar_t *why = vm_web_unsupported_reason(vm);
    int i, slot = -1;
    WebVm *entry;

    if (err && err_chars) err[0] = 0;
    if (enabled && why) {
        if (err) wcsncpy_s(err, err_chars, why, _TRUNCATE);
        return E_INVALIDARG;
    }
    if (!vm) return E_INVALIDARG;

    if (enabled) {
        char key[65];
        if (!ensure_local_listener()) {
            if (err) wcsncpy_s(err, err_chars, L"Couldn't open a local port for browser access.", _TRUNCATE);
            return E_FAIL;
        }
        if (lan && !ensure_lan_listener()) {
            if (err) wcsncpy_s(err, err_chars, L"Couldn't set up HTTPS for the network. See the log.", _TRUNCATE);
            return E_FAIL;
        }
        if (!random_hex(key, 64)) return E_FAIL;
        AcquireSRWLockExclusive(&g_lock);
        entry = web_vm_find_id(vm->unique_id);
        if (!entry) {
            for (i = 0; i < ASB_MAX_VMS; i++) if (!g_vms_web[i].vm_id) { slot = i; break; }
            entry = slot >= 0 ? &g_vms_web[slot] : NULL;
        } else {
            /* A new key: nothing issued under the old one stays valid. */
            for (i = 0; i < WEB_MAX_SESSIONS; i++)
                if (g_cookies[i].vm_id == vm->unique_id) ZeroMemory(&g_cookies[i], sizeof(g_cookies[i]));
            if (entry->session) {
                InterlockedExchange(&entry->session->stop, 1);
                shutdown(entry->session->conn->s, SD_BOTH);
                entry->session = NULL;
            }
        }
        if (entry) {
            entry->vm_id = vm->unique_id;
            WideCharToMultiByte(CP_UTF8, 0, vm->name, -1, entry->name, sizeof(entry->name), NULL, NULL);
            strcpy_s(entry->key, sizeof(entry->key), key);
            entry->lan = lan;
        }
        ReleaseSRWLockExclusive(&g_lock);
        SecureZeroMemory(key, sizeof(key));
        if (!entry) return E_OUTOFMEMORY;
    } else {
        AcquireSRWLockExclusive(&g_lock);
        entry = web_vm_find_id(vm->unique_id);
        if (entry) {
            if (entry->session) {
                InterlockedExchange(&entry->session->stop, 1);
                shutdown(entry->session->conn->s, SD_BOTH);
            }
            SecureZeroMemory(entry->key, sizeof(entry->key));
            entry->vm_id = 0;
            entry->session = NULL;
        }
        for (i = 0; i < WEB_MAX_SESSIONS; i++)
            if (g_cookies[i].vm_id == vm->unique_id) ZeroMemory(&g_cookies[i], sizeof(g_cookies[i]));
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockShared(&g_lock);
    i = any_lan_vm();
    ReleaseSRWLockShared(&g_lock);
    if (!i) close_lan_listener();
    return S_OK;
}

int vm_web_status_json(VmInstance *vm, char *out, int cap)
{
    const wchar_t *why = vm_web_unsupported_reason(vm);
    char why_u[300] = "", name[800] = "", name_url[2400] = "", key[65] = "";
    WebVm *entry;
    BOOL enabled = FALSE, lan = FALSE, connected = FALSE;
    int pos;

    if (!vm) return sprintf_s(out, cap, "{\"enabled\":false}");
    if (why) WideCharToMultiByte(CP_UTF8, 0, why, -1, why_u, sizeof(why_u), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, vm->name, -1, name, sizeof(name), NULL, NULL);
    url_encode(name, name_url, sizeof(name_url));
    AcquireSRWLockExclusive(&g_lock);
    entry = web_vm_find_id(vm->unique_id);
    if (entry) {
        strcpy_s(entry->name, sizeof(entry->name), name);   /* it may have been renamed */
        enabled = TRUE;
        lan = entry->lan;
        connected = entry->session != NULL;
        strcpy_s(key, sizeof(key), entry->key);
    }
    ReleaseSRWLockExclusive(&g_lock);

    pos = sprintf_s(out, cap, "{\"enabled\":%s,\"lan\":%s,\"connected\":%s,\"unsupported\":\"%s\",\"reach\":\"%s\"",
                    enabled ? "true" : "false", lan ? "true" : "false", connected ? "true" : "false", why_u,
                    !enabled ? "Nobody: browser access is off."
                    : lan ? "Anyone on your private or domain networks who has this VM's access key. They get full control of the VM: screen, keyboard, mouse and clipboard. The traffic is encrypted, and browsers warn about the certificate because it is self-signed: check the fingerprint."
                          : "Only someone on this PC who has this VM's access key. They get full control of the VM: screen, keyboard, mouse and clipboard.");
    if (enabled) {
        pos += sprintf_s(out + pos, cap - pos, ",\"key\":\"%s\",\"url\":\"http://127.0.0.1:%d/vm/%s#k=%s\"",
                         key, g_port_local, name_url, key);
        if (lan) {
            pos += sprintf_s(out + pos, cap - pos, ",\"lanPort\":%d,\"lanPath\":\"/vm/%s\",\"fingerprint\":\"%s\",\"lanAddresses\":[",
                             g_port_lan, name_url, g_fingerprint);
            pos = vm_stream_append_host_addresses(out, cap, pos);
            pos += sprintf_s(out + pos, cap - pos, "]");
        }
    }
    SecureZeroMemory(key, sizeof(key));
    pos += sprintf_s(out + pos, cap - pos, "}");
    return pos;
}

BOOL vm_web_connected(UINT64 vm_id)
{
    WebVm *entry;
    BOOL connected;
    AcquireSRWLockShared(&g_lock);
    entry = web_vm_find_id(vm_id);
    connected = entry && entry->session != NULL;
    ReleaseSRWLockShared(&g_lock);
    return connected;
}

HRESULT vm_web_dashboard_set(BOOL enabled, BOOL lan, wchar_t *err, size_t err_chars)
{
    BOOL keep_lan;
    if (err && err_chars) err[0] = 0;
    if (enabled) {
        if (!ensure_local_listener()) {
            if (err) wcsncpy_s(err, err_chars, L"Couldn't open a local port for the dashboard.", _TRUNCATE);
            return E_FAIL;
        }
        if (lan && !ensure_lan_listener()) {
            if (err) wcsncpy_s(err, err_chars, L"Couldn't set up HTTPS for the network. See the log.", _TRUNCATE);
            return E_FAIL;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_dashboard = enabled;
    g_dashboard_lan = enabled && lan;
    keep_lan = any_lan_vm();
    ReleaseSRWLockExclusive(&g_lock);
    if (!keep_lan) close_lan_listener();
    return S_OK;
}

int vm_web_dashboard_json(char *out, int cap, int pos)
{
    pos += sprintf_s(out + pos, cap - pos, "{\"localUrl\":\"http://127.0.0.1:%d/\"", g_port_local);
    if (g_dashboard_lan) {
        pos += sprintf_s(out + pos, cap - pos, ",\"lanPort\":%d,\"fingerprint\":\"%s\",\"lanAddresses\":[",
                         g_port_lan, g_fingerprint);
        pos = vm_stream_append_host_addresses(out, cap, pos);
        pos += sprintf_s(out + pos, cap - pos, "]");
    }
    pos += sprintf_s(out + pos, cap - pos, "}");
    return pos;
}

void vm_web_shutdown(void)
{
    int i;
    InterlockedExchange(&g_shutdown, 1);
    if (g_listen_local != INVALID_SOCKET) { closesocket(g_listen_local); g_listen_local = INVALID_SOCKET; }
    close_lan_listener();
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < ASB_MAX_VMS; i++) {
        if (g_vms_web[i].session) {
            InterlockedExchange(&g_vms_web[i].session->stop, 1);
            shutdown(g_vms_web[i].session->conn->s, SD_BOTH);
        }
        SecureZeroMemory(&g_vms_web[i], sizeof(g_vms_web[i]));
    }
    ZeroMemory(g_cookies, sizeof(g_cookies));
    ReleaseSRWLockExclusive(&g_lock);
}
