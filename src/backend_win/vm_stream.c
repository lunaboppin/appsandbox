/*
 * vm_stream.c -- Moonlight access to Sunshine in the guest (see vm_stream.h).
 *
 * The relay is plain sockets rather than an HCN port mapping so that it can bind
 * to loopback alone, goes away with the app, and leaves no policy behind on the
 * network if the app dies. One thread serves the UDP ports, one accepts on the TCP
 * ports, and each TCP connection has a thread.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <stdio.h>
#include "vm_stream.h"
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include "vm_agent.h"
#include "hcn_network.h"
#include "asb_core.h"
#include "ui.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define STREAM_DEFAULT_PORT 47989
#define STREAM_PORT_STEP    100   /* Sunshine's ports span 47984-48010 for 47989 */

/* Offsets from Sunshine's `port`. The web UI (+1) is deliberately not here. */
static const int g_tcp_offsets[] = { -5, 0, 21 };             /* HTTPS, HTTP, RTSP */
static const int g_udp_offsets[] = { 9, 10, 11, 13 };         /* video, control, audio, mic */
#define TCP_PORTS 3
#define UDP_PORTS 4
#define UDP_CLIENTS 8
#define UDP_IDLE_MS 120000
#define UDP_BUFFER  (4 * 1024 * 1024)

typedef struct {
    struct sockaddr_in addr;
    SOCKET   upstream;
    int      port_index;
    ULONGLONG last_tick;
} UdpClient;

typedef struct StreamRelay {
    UINT64   vm_id;
    char     guest_ip[32];
    int      base_port;
    volatile BOOL stop;
    volatile LONG tcp_conns;
    volatile ULONGLONG last_activity;
    SOCKET   tcp_listen[TCP_PORTS];
    SOCKET   udp_listen[UDP_PORTS];
    HANDLE   tcp_thread;
    HANDLE   udp_thread;
    BOOL     firewall_rule;
    wchar_t  rule_name[128];
} StreamRelay;

typedef struct {
    StreamRelay *relay;
    SOCKET client;
    int    guest_port;
} TcpConn;

#define MAX_RELAYS 32
static StreamRelay *g_relays[MAX_RELAYS];
static wchar_t g_relay_errors[MAX_RELAYS][128];
static UINT64  g_relay_error_ids[MAX_RELAYS];
static SRWLOCK g_relay_lock = SRWLOCK_INIT;

static void relay_set_error(UINT64 vm_id, const wchar_t *text)
{
    int i, free_slot = -1;
    AcquireSRWLockExclusive(&g_relay_lock);
    for (i = 0; i < MAX_RELAYS; i++) {
        if (g_relay_error_ids[i] == vm_id) { free_slot = i; break; }
        if (!g_relay_error_ids[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_relay_error_ids[free_slot] = text ? vm_id : 0;
        wcsncpy_s(g_relay_errors[free_slot], 128, text ? text : L"", _TRUNCATE);
    }
    ReleaseSRWLockExclusive(&g_relay_lock);
}

static void run_hidden(const wchar_t *command)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t buf[1024];
    wcsncpy_s(buf, 1024, command, _TRUNCATE);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* Only private and domain networks: a laptop on public Wi-Fi stays closed. */
static void firewall_open(StreamRelay *r, const wchar_t *vm_name)
{
    wchar_t cmd[512];
    int i;
    wchar_t tcp[64] = L"", udp[64] = L"";
    swprintf_s(r->rule_name, 128, L"App Sandbox streaming (%s)", vm_name);
    for (i = 0; i < TCP_PORTS; i++) {
        wchar_t one[16];
        swprintf_s(one, 16, L"%s%d", i ? L"," : L"", r->base_port + g_tcp_offsets[i]);
        wcscat_s(tcp, 64, one);
    }
    for (i = 0; i < UDP_PORTS; i++) {
        wchar_t one[16];
        swprintf_s(one, 16, L"%s%d", i ? L"," : L"", r->base_port + g_udp_offsets[i]);
        wcscat_s(udp, 64, one);
    }
    swprintf_s(cmd, 512, L"netsh advfirewall firewall delete rule name=\"%s\"", r->rule_name);
    run_hidden(cmd);
    swprintf_s(cmd, 512, L"netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow "
                         L"protocol=TCP localport=%s profile=private,domain", r->rule_name, tcp);
    run_hidden(cmd);
    swprintf_s(cmd, 512, L"netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow "
                         L"protocol=UDP localport=%s profile=private,domain", r->rule_name, udp);
    run_hidden(cmd);
    r->firewall_rule = TRUE;
}

static void firewall_close(StreamRelay *r)
{
    wchar_t cmd[256];
    if (!r->firewall_rule) return;
    swprintf_s(cmd, 256, L"netsh advfirewall firewall delete rule name=\"%s\"", r->rule_name);
    run_hidden(cmd);
    r->firewall_rule = FALSE;
}

static BOOL send_all(SOCKET s, const char *p, int n)
{
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0) return FALSE;
        p += w;
        n -= w;
    }
    return TRUE;
}

static SOCKET tcp_connect_guest(const char *ip, int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    u_long nb = 1;
    fd_set wr, er;
    struct timeval tv = { 3, 0 };
    if (s == INVALID_SOCKET) return s;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&a, sizeof(a));
    FD_ZERO(&wr); FD_ZERO(&er);
    FD_SET(s, &wr); FD_SET(s, &er);
    if (select(0, NULL, &wr, &er, &tv) <= 0 || FD_ISSET(s, &er)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

static DWORD WINAPI tcp_conn_thread(LPVOID param)
{
    TcpConn *c = (TcpConn *)param;
    StreamRelay *r = c->relay;
    SOCKET guest = tcp_connect_guest(r->guest_ip, c->guest_port);
    char *buf = (char *)malloc(65536);
    int on = 1;

    if (guest != INVALID_SOCKET && buf) {
        WSAPOLLFD fds[2];
        setsockopt(guest, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
        setsockopt(c->client, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
        fds[0].fd = c->client; fds[0].events = POLLRDNORM;
        fds[1].fd = guest;     fds[1].events = POLLRDNORM;
        while (!r->stop) {
            int n = WSAPoll(fds, 2, 500), i;
            BOOL ok = TRUE;
            if (n < 0) break;
            if (n == 0) continue;
            for (i = 0; i < 2 && ok; i++) {
                if (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) {
                    int got = recv(fds[i].fd, buf, 65536, 0);
                    BOOL closing = FALSE;
                    /* Sunshine closes after each pairing reply. If the client gets the
                       reply well before the close, it reuses the connection, loses the
                       next request and sends it again, and Sunshine ends the pairing over
                       the repeat. So a reply and its close arrive together. */
                    if (got > 0 && i == 1) {
                        WSAPOLLFD peek;
                        char one;
                        peek.fd = guest;
                        peek.events = POLLRDNORM;
                        peek.revents = 0;
                        if (WSAPoll(&peek, 1, 15) > 0 && recv(guest, &one, 1, MSG_PEEK) == 0)
                            closing = TRUE;
                    }
                    if (got <= 0 || !send_all(fds[1 - i].fd, buf, got) || closing) ok = FALSE;
                }
            }
            if (!ok) break;
        }
    }
    if (guest != INVALID_SOCKET) closesocket(guest);
    /* Closing with unread data would reset the connection and could take the last
       reply with it, so say we're done and read what's left. */
    shutdown(c->client, SD_SEND);
    if (buf) {
        WSAPOLLFD drain;
        int waits = 0;
        drain.fd = c->client;
        drain.events = POLLRDNORM;
        while (waits++ < 10 && !r->stop) {
            drain.revents = 0;
            if (WSAPoll(&drain, 1, 100) > 0 && recv(c->client, buf, 65536, 0) <= 0) break;
        }
    }
    closesocket(c->client);
    free(buf);
    InterlockedDecrement(&r->tcp_conns);
    free(c);
    return 0;
}

static DWORD WINAPI tcp_accept_thread(LPVOID param)
{
    StreamRelay *r = (StreamRelay *)param;
    WSAPOLLFD fds[TCP_PORTS];
    int i;
    for (i = 0; i < TCP_PORTS; i++) {
        fds[i].fd = r->tcp_listen[i];
        fds[i].events = POLLRDNORM;
    }
    while (!r->stop) {
        int n = WSAPoll(fds, TCP_PORTS, 500);
        if (n <= 0) continue;
        for (i = 0; i < TCP_PORTS; i++) {
            if (fds[i].revents & POLLRDNORM) {
                SOCKET client = accept(fds[i].fd, NULL, NULL);
                TcpConn *c;
                HANDLE t = NULL;
                if (client == INVALID_SOCKET) continue;
                c = (TcpConn *)malloc(sizeof(*c));
                if (c) {
                    c->relay = r;
                    c->client = client;
                    c->guest_port = r->base_port + g_tcp_offsets[i];
                    InterlockedIncrement(&r->tcp_conns);
                    r->last_activity = GetTickCount64();
                    t = CreateThread(NULL, 0, tcp_conn_thread, c, 0, NULL);
                    if (t) CloseHandle(t);
                    else { InterlockedDecrement(&r->tcp_conns); free(c); }
                }
                if (!t) closesocket(client);
            }
        }
    }
    return 0;
}

static UdpClient *udp_client_for(UdpClient *clients, const struct sockaddr_in *from, int port_index,
                                 const StreamRelay *r)
{
    int i, slot = -1;
    ULONGLONG oldest = ~0ull;
    struct sockaddr_in g;
    SOCKET s;
    int size = UDP_BUFFER;
    for (i = 0; i < UDP_CLIENTS * UDP_PORTS; i++) {
        UdpClient *c = &clients[i];
        if (c->upstream != INVALID_SOCKET && c->port_index == port_index &&
            c->addr.sin_addr.s_addr == from->sin_addr.s_addr && c->addr.sin_port == from->sin_port)
            return c;
        if (c->upstream == INVALID_SOCKET) { if (slot < 0 || oldest) { slot = i; oldest = 0; } }
        else if (c->last_tick < oldest) { oldest = c->last_tick; slot = i; }
    }
    if (clients[slot].upstream != INVALID_SOCKET) closesocket(clients[slot].upstream);
    clients[slot].upstream = INVALID_SOCKET;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return NULL;
    ZeroMemory(&g, sizeof(g));
    g.sin_family = AF_INET;
    g.sin_port = htons((u_short)(r->base_port + g_udp_offsets[port_index]));
    inet_pton(AF_INET, r->guest_ip, &g.sin_addr);
    if (connect(s, (struct sockaddr *)&g, sizeof(g)) != 0) { closesocket(s); return NULL; }
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof(size));
    {
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
    }
    clients[slot].addr = *from;
    clients[slot].upstream = s;
    clients[slot].port_index = port_index;
    return &clients[slot];
}

/* A stream's packets arrive in bursts, so each ready socket is drained a little at
   a time rather than one packet per poll. */
#define UDP_BATCH 32

static DWORD WINAPI udp_thread(LPVOID param)
{
    StreamRelay *r = (StreamRelay *)param;
    UdpClient clients[UDP_CLIENTS * UDP_PORTS];
    WSAPOLLFD fds[UDP_PORTS + UDP_CLIENTS * UDP_PORTS];
    UdpClient *owners[UDP_PORTS + UDP_CLIENTS * UDP_PORTS];
    char *buf = (char *)malloc(65536);
    int i;
    ULONGLONG last_sweep = GetTickCount64();

    if (!buf) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ZeroMemory(clients, sizeof(clients));
    for (i = 0; i < UDP_CLIENTS * UDP_PORTS; i++) clients[i].upstream = INVALID_SOCKET;

    while (!r->stop) {
        int count = 0, n, batch;
        ULONGLONG now;
        for (i = 0; i < UDP_PORTS; i++) {
            fds[count].fd = r->udp_listen[i];
            fds[count].events = POLLRDNORM;
            owners[count++] = NULL;
        }
        for (i = 0; i < UDP_CLIENTS * UDP_PORTS; i++) {
            if (clients[i].upstream != INVALID_SOCKET) {
                fds[count].fd = clients[i].upstream;
                fds[count].events = POLLRDNORM;
                owners[count++] = &clients[i];
            }
        }
        n = WSAPoll(fds, count, 250);
        now = GetTickCount64();
        if (n > 0) {
            for (i = 0; i < count; i++) {
                if (!(fds[i].revents & POLLRDNORM)) continue;
                for (batch = 0; batch < UDP_BATCH; batch++) {
                    if (i < UDP_PORTS) {
                        struct sockaddr_in from;
                        int from_len = sizeof(from);
                        UdpClient *c;
                        int got = recvfrom(fds[i].fd, buf, 65536, 0, (struct sockaddr *)&from, &from_len);
                        if (got < 0) break;
                        c = udp_client_for(clients, &from, i, r);
                        if (c) {
                            c->last_tick = now;
                            r->last_activity = now;
                            send(c->upstream, buf, got, 0);
                        }
                    } else {
                        UdpClient *c = owners[i];
                        int got = recv(fds[i].fd, buf, 65536, 0);
                        if (got < 0) break;
                        c->last_tick = now;
                        sendto(r->udp_listen[c->port_index], buf, got, 0,
                               (struct sockaddr *)&c->addr, sizeof(c->addr));
                    }
                    /* Only keep draining while more is known to be ready. */
                    {
                        WSAPOLLFD one;
                        one.fd = fds[i].fd;
                        one.events = POLLRDNORM;
                        if (WSAPoll(&one, 1, 0) <= 0) break;
                    }
                }
            }
        }
        if (now - last_sweep > 5000) {
            last_sweep = now;
            for (i = 0; i < UDP_CLIENTS * UDP_PORTS; i++) {
                if (clients[i].upstream != INVALID_SOCKET && now - clients[i].last_tick > UDP_IDLE_MS) {
                    closesocket(clients[i].upstream);
                    clients[i].upstream = INVALID_SOCKET;
                }
            }
        }
    }
    for (i = 0; i < UDP_CLIENTS * UDP_PORTS; i++)
        if (clients[i].upstream != INVALID_SOCKET) closesocket(clients[i].upstream);
    free(buf);
    return 0;
}

static SOCKET bind_port(int type, int proto, ULONG addr, int port)
{
    SOCKET s = socket(AF_INET, type, proto);
    struct sockaddr_in a;
    BOOL exclusive = TRUE;
    u_long nb = 1;
    if (s == INVALID_SOCKET) return s;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr;
    a.sin_port = htons((u_short)port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        (type == SOCK_STREAM && listen(s, 8) != 0)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nb);
    if (type == SOCK_DGRAM) {
        int size = UDP_BUFFER;
        BOOL off = FALSE;
        DWORD ret = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof(size));
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
    }
    return s;
}

static StreamRelay **relay_find(UINT64 vm_id)
{
    int i;
    for (i = 0; i < MAX_RELAYS; i++)
        if (g_relays[i] && g_relays[i]->vm_id == vm_id) return &g_relays[i];
    return NULL;
}

void vm_stream_relay_stop(VmInstance *vm)
{
    StreamRelay **slot, *r;
    int i;
    if (!vm) return;
    AcquireSRWLockExclusive(&g_relay_lock);
    slot = relay_find(vm->unique_id);
    r = slot ? *slot : NULL;
    if (slot) *slot = NULL;
    ReleaseSRWLockExclusive(&g_relay_lock);
    if (!r) return;

    r->stop = TRUE;
    if (r->tcp_thread) { WaitForSingleObject(r->tcp_thread, INFINITE); CloseHandle(r->tcp_thread); }
    if (r->udp_thread) { WaitForSingleObject(r->udp_thread, INFINITE); CloseHandle(r->udp_thread); }
    for (i = 0; i < 40 && r->tcp_conns > 0; i++) Sleep(50);
    for (i = 0; i < TCP_PORTS; i++) if (r->tcp_listen[i] != INVALID_SOCKET) closesocket(r->tcp_listen[i]);
    for (i = 0; i < UDP_PORTS; i++) if (r->udp_listen[i] != INVALID_SOCKET) closesocket(r->udp_listen[i]);
    firewall_close(r);
    /* Connection threads read the relay until they leave; one still stuck in a
       send is left its memory rather than risk a use after free. */
    if (r->tcp_conns == 0) free(r);
    ui_log(L"Streaming relay stopped for \"%s\".", vm->name);
}

void vm_stream_relay_start(VmInstance *vm)
{
    StreamRelay *r;
    ULONG addr;
    int i, slot = -1;
    WSADATA wsa;

    if (!vm || !vm->stream_enabled || vm->network_mode != NET_NAT || !vm->nat_ip[0]) return;
    vm_stream_relay_stop(vm);
    WSAStartup(MAKEWORD(2, 2), &wsa);

    r = (StreamRelay *)calloc(1, sizeof(*r));
    if (!r) return;
    r->vm_id = vm->unique_id;
    strcpy_s(r->guest_ip, sizeof(r->guest_ip), vm->nat_ip);
    r->base_port = (int)vm->stream_port;
    addr = vm->stream_exposure == ASB_STREAM_NETWORK ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    for (i = 0; i < TCP_PORTS; i++) r->tcp_listen[i] = INVALID_SOCKET;
    for (i = 0; i < UDP_PORTS; i++) r->udp_listen[i] = INVALID_SOCKET;

    for (i = 0; i < TCP_PORTS; i++) {
        r->tcp_listen[i] = bind_port(SOCK_STREAM, IPPROTO_TCP, addr, r->base_port + g_tcp_offsets[i]);
        if (r->tcp_listen[i] == INVALID_SOCKET) goto in_use;
    }
    for (i = 0; i < UDP_PORTS; i++) {
        r->udp_listen[i] = bind_port(SOCK_DGRAM, IPPROTO_UDP, addr, r->base_port + g_udp_offsets[i]);
        if (r->udp_listen[i] == INVALID_SOCKET) goto in_use;
    }

    if (vm->stream_exposure == ASB_STREAM_NETWORK) firewall_open(r, vm->name);
    r->tcp_thread = CreateThread(NULL, 0, tcp_accept_thread, r, 0, NULL);
    r->udp_thread = CreateThread(NULL, 0, udp_thread, r, 0, NULL);

    AcquireSRWLockExclusive(&g_relay_lock);
    for (i = 0; i < MAX_RELAYS; i++) if (!g_relays[i]) { slot = i; break; }
    if (slot >= 0) g_relays[slot] = r;
    ReleaseSRWLockExclusive(&g_relay_lock);
    relay_set_error(vm->unique_id, NULL);
    ui_log(L"Streaming relay for \"%s\": %s port %d to %S.", vm->name,
           vm->stream_exposure == ASB_STREAM_NETWORK ? L"all interfaces," : L"this PC only,",
           r->base_port, r->guest_ip);
    return;

in_use:
    for (i = 0; i < TCP_PORTS; i++) if (r->tcp_listen[i] != INVALID_SOCKET) closesocket(r->tcp_listen[i]);
    for (i = 0; i < UDP_PORTS; i++) if (r->udp_listen[i] != INVALID_SOCKET) closesocket(r->udp_listen[i]);
    relay_set_error(vm->unique_id, L"Another program on this PC is using the streaming ports. If Sunshine runs on this PC, stop it or pick another VM.");
    ui_log(L"Streaming relay for \"%s\": port %d range is in use.", vm->name, r->base_port);
    free(r);
}

const wchar_t *vm_stream_unsupported_reason(VmInstance *vm)
{
    if (!vm) return L"Unknown VM.";
    if (vm->lab_mode) return L"Malware lab VMs are isolated, so they can't stream.";
    if (vm->is_template || vm->is_appliance) return L"Only regular VMs can stream.";
    if (_wcsicmp(vm->os_type, L"Windows") != 0)
        return L"Moonlight needs a Windows guest for now: Sunshine can't capture the Ubuntu guest display or use its GPU yet. Browser access works on Ubuntu guests.";
    if (vm->network_mode != NET_NAT && vm->network_mode != NET_EXTERNAL)
        return L"Moonlight needs a NAT or bridged network on this VM. Change it under Network.";
    return NULL;
}

/* Sunshine must know its own port, so each VM keeps a distinct range. The first
   one gets Moonlight's default, which clients find without being told a port. */
/* Windows reserves blocks of ports for itself and Hyper-V, and they move between boots,
   so the only real test of a port is binding it. */
static BOOL bind_free(int type, int protocol, int port)
{
    SOCKET s = socket(AF_INET, type, protocol);
    struct sockaddr_in a;
    BOOL ok;
    if (s == INVALID_SOCKET) return FALSE;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)port);
    ok = bind(s, (struct sockaddr *)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

BOOL vm_stream_port_free(DWORD port)
{
    WSADATA wsa;
    int i;
    BOOL ok = TRUE;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (i = 0; i < TCP_PORTS && ok; i++) ok = bind_free(SOCK_STREAM, IPPROTO_TCP, (int)port + g_tcp_offsets[i]);
    for (i = 0; i < UDP_PORTS && ok; i++) ok = bind_free(SOCK_DGRAM, IPPROTO_UDP, (int)port + g_udp_offsets[i]);
    WSACleanup();
    return ok;
}

static DWORD pick_port(VmInstance *vm)
{
    DWORD port = STREAM_DEFAULT_PORT;
    for (;;) {
        BOOL used = FALSE;
        int i, count = asb_vm_count();
        for (i = 0; i < count; i++) {
            VmInstance *other = asb_vm_instance(asb_vm_get(i));
            if (other && other != vm && other->stream_port == port) { used = TRUE; break; }
        }
        if (!used && vm_stream_port_free(port)) return port;
        port += STREAM_PORT_STEP;
    }
}

ULONGLONG vm_stream_last_activity(UINT64 vm_id)
{
    ULONGLONG when = 0;
    int i;
    AcquireSRWLockShared(&g_relay_lock);
    for (i = 0; i < MAX_RELAYS; i++) {
        if (g_relays[i] && g_relays[i]->vm_id == vm_id) {
            when = g_relays[i]->last_activity;
            break;
        }
    }
    ReleaseSRWLockShared(&g_relay_lock);
    return when;
}

void vm_stream_setup_command(VmInstance *vm, char *cmd, int cap)
{
    char allowed[32];
    if (vm->network_mode == NET_NAT)
        sprintf_s(allowed, sizeof(allowed), "%s.1", hcn_nat_subnet_base());   /* the relay */
    else
        strcpy_s(allowed, sizeof(allowed), "LocalSubnet");
    if (vm->stream_name[0])
        sprintf_s(cmd, cap, "stream_setup:%lu:%s:%s:%s", (unsigned long)vm->stream_port,
                  vm->stream_software ? "software" : "auto", allowed, vm->stream_name);
    else
        sprintf_s(cmd, cap, "stream_setup:%lu:%s:%s:%S", (unsigned long)vm->stream_port,
                  vm->stream_software ? "software" : "auto", allowed, vm->name);
}

HRESULT vm_stream_configure(VmInstance *vm, BOOL enabled, int exposure, BOOL software_encoder,
                            wchar_t *err, size_t err_chars)
{
    const wchar_t *why = vm_stream_unsupported_reason(vm);
    if (err && err_chars) err[0] = L'\0';
    if (enabled && why) {
        if (err) wcsncpy_s(err, err_chars, why, _TRUNCATE);
        return E_INVALIDARG;
    }
    if (!vm) return E_INVALIDARG;
    if (enabled && exposure != ASB_STREAM_THIS_PC && exposure != ASB_STREAM_NETWORK) {
        if (err) wcsncpy_s(err, err_chars, L"Choose who can connect: this PC or your network.", _TRUNCATE);
        return E_INVALIDARG;
    }
    /* A bridged VM's address is on the network whatever is chosen here. */
    if (enabled && vm->network_mode == NET_EXTERNAL) exposure = ASB_STREAM_NETWORK;

    vm->stream_enabled = enabled;
    if (enabled) {
        vm->stream_exposure = exposure;
        vm->stream_software = software_encoder;
        if (!vm->stream_port) vm->stream_port = pick_port(vm);
    }
    asb_save();

    if (!vm->running) return S_OK;
    if (enabled) {
        vm_stream_relay_start(vm);
        if (vm->agent_online && !vm->agent_initializing) {
            char cmd[300], reply[300] = "";
            vm_stream_setup_command(vm, cmd, sizeof(cmd));
            if (!vm_agent_send(vm, cmd, reply, sizeof(reply), 15000)) {
                if (err) swprintf_s(err, err_chars, L"The VM's agent said: %S. Restart the VM to update it.", reply);
                return E_FAIL;
            }
        }
    } else {
        vm_stream_relay_stop(vm);
        if (vm->agent_online && !vm->agent_initializing)
            vm_agent_send(vm, "stream_off", NULL, 0, 60000);
    }
    return S_OK;
}

static int append_json_escaped(char *out, int cap, int pos, const char *s)
{
    if (pos < cap - 1) out[pos++] = '"';
    for (; *s && pos < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[pos++] = '\\'; out[pos++] = (char)c; }
        else if (c < 0x20) pos += sprintf_s(out + pos, cap - pos, "\\u%04x", c);
        else out[pos++] = (char)c;
    }
    if (pos < cap - 1) out[pos++] = '"';
    out[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

static int append_json_wide(char *out, int cap, int pos, const wchar_t *w)
{
    char u[1024] = "";
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    return append_json_escaped(out, cap, pos, u);
}

int vm_stream_append_host_addresses(char *out, int cap, int pos)
{
    ULONG size = 32768;
    IP_ADAPTER_ADDRESSES *list = (IP_ADAPTER_ADDRESSES *)malloc(size), *a;
    BOOL first = TRUE;
    if (!list) return pos;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             NULL, list, &size) == NO_ERROR) {
        for (a = list; a; a = a->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *u;
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            /* Hyper-V's own switches lead only to VMs on this PC. */
            if (wcsstr(a->FriendlyName, L"vEthernet")) continue;
            for (u = a->FirstUnicastAddress; u; u = u->Next) {
                char ip[32];
                const struct sockaddr_in *sin = (const struct sockaddr_in *)u->Address.lpSockaddr;
                if ((ntohl(sin->sin_addr.s_addr) >> 16) == 0xA9FE) continue;   /* 169.254: no network */
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                if (!first && pos < cap - 1) out[pos++] = ',';
                first = FALSE;
                pos = append_json_escaped(out, cap, pos, ip);
            }
        }
    }
    free(list);
    return pos;
}

int vm_stream_status_json(VmInstance *vm, char *out, int cap)
{
    const wchar_t *why = vm_stream_unsupported_reason(vm);
    const wchar_t *relay_error = L"";
    char guest[1200] = "";
    int pos, i;
    BOOL bridged = vm && vm->network_mode == NET_EXTERNAL;
    BOOL network = vm && (bridged || vm->stream_exposure == ASB_STREAM_NETWORK);
    wchar_t reach[400] = L"";
    BOOL relay_up;

    if (!vm) return sprintf_s(out, cap, "{\"enabled\":false}");

    AcquireSRWLockShared(&g_relay_lock);
    relay_up = relay_find(vm->unique_id) != NULL;
    for (i = 0; i < MAX_RELAYS; i++)
        if (g_relay_error_ids[i] == vm->unique_id) relay_error = g_relay_errors[i];
    ReleaseSRWLockShared(&g_relay_lock);

    if (vm->stream_enabled && vm->agent_online && !vm->agent_initializing)
        vm_agent_send(vm, "stream_status", guest, sizeof(guest), 30000);

    if (!vm->stream_enabled)
        wcscpy_s(reach, 400, L"Nobody: streaming is off.");
    else if (bridged)
        wcscpy_s(reach, 400, L"Any device on the network this VM is bridged to can connect to it directly. It must pair with a PIN you enter here first. A paired device controls the VM, including at the sign-in screen.");
    else if (network)
        wcscpy_s(reach, 400, L"Any device that can reach this PC on a private or domain network, VPNs like Tailscale included if Windows counts them as private. It must pair with a PIN you enter here first. A paired device controls the VM, including at the sign-in screen.");
    else
        wcscpy_s(reach, 400, L"Only programs on this PC, at 127.0.0.1. Other devices can't connect. A client must still pair with a PIN you enter here.");

    pos = sprintf_s(out, cap,
        "{\"enabled\":%s,\"exposure\":\"%s\",\"bridged\":%s,\"port\":%lu,\"encoder\":\"%s\",\"relayUp\":%s,\"relayError\":",
        vm->stream_enabled ? "true" : "false",
        network ? "network" : "this_pc", bridged ? "true" : "false",
        (unsigned long)vm->stream_port, vm->stream_software ? "software" : "auto",
        relay_up ? "true" : "false");
    pos = append_json_wide(out, cap, pos, relay_error);
    pos += sprintf_s(out + pos, cap - pos, ",\"unsupported\":");
    pos = append_json_wide(out, cap, pos, why ? why : L"");
    pos += sprintf_s(out + pos, cap - pos, ",\"reach\":");
    pos = append_json_wide(out, cap, pos, reach);
    pos += sprintf_s(out + pos, cap - pos, ",\"addresses\":[");
    if (network && !bridged) pos = vm_stream_append_host_addresses(out, cap, pos);
    pos += sprintf_s(out + pos, cap - pos, "],\"guest\":%s}",
                     strncmp(guest, "ok:{", 4) == 0 ? guest + 3 : "null");
    return pos;
}

HRESULT vm_stream_pair(VmInstance *vm, const char *pin, const char *client_name,
                       char *message, int message_cap)
{
    char cmd[128], reply[600] = "";
    const char *p;
    if (message && message_cap) message[0] = 0;
    if (!vm || !vm->stream_enabled || !vm->agent_online) return E_INVALIDARG;
    if (!pin || strlen(pin) != 4) return E_INVALIDARG;
    for (p = pin; *p; p++) if (*p < '0' || *p > '9') return E_INVALIDARG;
    sprintf_s(cmd, sizeof(cmd), "stream_pin:%s:%.40s", pin, client_name && *client_name ? client_name : "Moonlight");
    if (vm_agent_send(vm, cmd, reply, sizeof(reply), 35000)) return S_OK;
    if (message) strncpy_s(message, message_cap, strncmp(reply, "error:", 6) == 0 ? reply + 6 : reply, _TRUNCATE);
    return E_FAIL;
}

HRESULT vm_stream_unpair_all(VmInstance *vm, char *message, int message_cap)
{
    char reply[600] = "";
    if (message && message_cap) message[0] = 0;
    if (!vm || !vm->stream_enabled || !vm->agent_online) return E_INVALIDARG;
    if (vm_agent_send(vm, "stream_unpair", reply, sizeof(reply), 35000)) return S_OK;
    if (message) strncpy_s(message, message_cap, strncmp(reply, "error:", 6) == 0 ? reply + 6 : reply, _TRUNCATE);
    return E_FAIL;
}
