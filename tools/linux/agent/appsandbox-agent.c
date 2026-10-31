/*
 * appsandbox-agent.c — Linux guest agent for App Sandbox.
 *
 * Listens on AF_VSOCK port 1 for the host control channel. Speaks the
 * line-based text protocol from docs/linux-idd-implementation-plan.md
 * section 3 (Port 1 — agent). Compatible with the existing host code in
 * src/backend_win/vm_agent.c.
 *
 * Single-client at a time. On accept: send "hello", start a heartbeat
 * thread emitting "heartbeat" every 5s, run a command dispatch loop on
 * the main thread. On client disconnect, join heartbeat thread and accept
 * the next connection.
 *
 * Build:    gcc -O2 -Wall -Wextra -pthread -D_GNU_SOURCE \
 *               -o appsandbox-agent appsandbox-agent.c
 * Install:  /usr/local/bin/appsandbox-agent
 * Run as:   systemd system service (root).
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/vm_sockets.h>

/* ---- Constants ---- */

#define AGENT_PORT              1
#define HEARTBEAT_INTERVAL_SEC  5
#define LINE_BUF_MAX            4096
#define REPLY_MAX               256

/* ---- Global state for the currently-active client connection ---- */

static volatile sig_atomic_t g_stop      = 0;
static volatile int          g_client_fd = -1;
static pthread_mutex_t       g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Logging (timestamped, goes to stderr → systemd journal) ---- */

static void agent_log(const char *fmt, ...)
{
    char    ts[32];
    time_t  t = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- Line I/O ---- */

/* Send a line + '\n'. Thread-safe (heartbeat thread and command-reply
 * code share the write end of the same socket). */
static int send_line(int fd, const char *line)
{
    size_t  len = strlen(line);
    int     ok;
    ssize_t n1, n2;

    if (fd < 0) return -1;

    pthread_mutex_lock(&g_send_lock);
    n1 = write(fd, line, len);
    n2 = (n1 == (ssize_t)len) ? write(fd, "\n", 1) : -1;
    ok = (n1 == (ssize_t)len && n2 == 1);
    pthread_mutex_unlock(&g_send_lock);

    return ok ? 0 : -1;
}

/* Read one line (up to '\n') into buf. Strips '\r'. Null-terminates.
 * Returns: number of chars in buf, 0 on EOF, -1 on error. */
static int recv_line(int fd, char *buf, int max)
{
    int pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) { buf[pos] = '\0'; return 0; }       /* EOF */
        if (n < 0) {
            if (errno == EINTR) continue;
            buf[pos] = '\0';
            return -1;
        }
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

/* ---- Subprocess helpers ---- */

/* Run a shell command synchronously, return exit code (-1 on spawn fail). */
static int run_sync(const char *cmd)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Spawn a shell command, double-fork to detach it from us. Returns 0
 * if the intermediate fork succeeded (doesn't wait for the real child). */
static int spawn_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (setsid() < 0) _exit(127);
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);  /* reap intermediate */
    return 0;
}

/* ---- Clipboard helper spawn ----
 *
 * Mirrors the Windows-agent pattern (tools/agent/agent.c
 * clipboard_monitor_thread + spawn_clipboard_in_session): the agent
 * binds the privileged vsock ports as root, then forks+execs a
 * user-context helper with the fds inherited at 3/4 (LISTEN_FDS
 * convention). A background monitor thread polls every 3s for user
 * session presence and helper liveness and respawns as needed —
 * idd_connect alone is a one-shot trigger and doesn't survive helper
 * crashes / window-server restarts. */

static pid_t           g_clipboard_helper_pid = 0;
static pthread_mutex_t g_clipboard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_clipboard_monitor_thread;
static volatile int    g_clipboard_monitor_running = 0;

static int vsock_bind_listen_privileged(unsigned port)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = port;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("clipboard: bind vsock :%u failed: %s",
                  port, strerror(errno));
        close(s); return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("clipboard: listen vsock :%u failed: %s",
                  port, strerror(errno));
        close(s); return -1;
    }
    return s;
}

/* Find the active user's Mutter-XWayland auth cookie. Sets *out to a
 * NUL-terminated path on success, leaves it untouched otherwise.
 * Caller must already be in the user's uid context if checking
 * readability matters. */
static void find_mutter_xauth(uid_t uid, char *out, size_t cap)
{
    char xdg_rt[64];
    snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%u", (unsigned)uid);
    DIR *d = opendir(xdg_rt);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, ".mutter-Xwaylandauth.", 21) == 0) {
            snprintf(out, cap, "%s/%s", xdg_rt, de->d_name);
            break;
        }
    }
    closedir(d);
}

/* True if the UID looks like a regular human user we can spawn the
 * clipboard helper as — has a /home/... directory and a real login shell.
 *
 * Without this filter we'd accept system users like gdm (pw_dir=/var/lib/gdm3,
 * pw_shell=/usr/sbin/nologin). The greeter's own mutter creates a
 * /run/user/<gdm-uid>/.mutter-Xwaylandauth.* cookie for the login screen,
 * which would match the xauth probe. Spawning the clipboard helper as gdm
 * crashloops (exit 1) until the user actually logs in. The pw_dir + shell
 * checks below reject gdm regardless of its UID. */
static int uid_is_real_user(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (!pw || !pw->pw_dir || !pw->pw_shell) return 0;
    if (strncmp(pw->pw_dir, "/home/", 6) != 0) return 0;
    /* Reject nologin/false. Accept anything else as a real shell. */
    const char *shell = pw->pw_shell;
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if (strcmp(base, "nologin") == 0 || strcmp(base, "false") == 0)
        return 0;
    return 1;
}

/* Scan /run/user for the UID whose runtime dir has a Mutter XWayland auth
 * cookie AND who looks like a real user (not gdm). That's the user whose
 * graphical session we want to spawn the clipboard helper into. Returns
 * 0 (not found) or a valid uid_t.
 *
 * We can't hardcode a username — the create-VM modal lets the user pick
 * any account name. Looking for the live mutter xauth file is more robust
 * than parsing /etc/passwd because it implicitly filters for "user who
 * actually has a graphical session right now". */
static uid_t find_graphical_session_uid(void)
{
    DIR *d = opendir("/run/user");
    if (!d) return 0;
    struct dirent *de;
    uid_t found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char *end = NULL;
        unsigned long v = strtoul(de->d_name, &end, 10);
        if (!end || *end != '\0' || v < 1000 || v >= 65534) continue;
        if (!uid_is_real_user((uid_t)v)) continue;
        char xauth[512] = {0};
        find_mutter_xauth((uid_t)v, xauth, sizeof(xauth));
        if (xauth[0]) { found = (uid_t)v; break; }
    }
    closedir(d);
    return found;
}

/* User session is "ready" once Mutter has dropped its XWayland auth
 * cookie. Before that, xcb_connect would fail with the no-auth-protocol
 * error we saw on early spawns. */
static int is_user_session_ready(uid_t uid)
{
    char xauth[512] = {0};
    find_mutter_xauth(uid, xauth, sizeof(xauth));
    if (xauth[0] == '\0') return 0;
    struct stat st;
    return stat(xauth, &st) == 0 && S_ISREG(st.st_mode);
}

static void kill_clipboard_helper_locked(void)
{
    if (g_clipboard_helper_pid <= 0) return;
    kill(g_clipboard_helper_pid, SIGTERM);
    int status;
    for (int i = 0; i < 10; i++) {
        if (waitpid(g_clipboard_helper_pid, &status, WNOHANG) != 0) break;
        usleep(100 * 1000);
    }
    /* If still alive after 1s, SIGKILL. */
    if (waitpid(g_clipboard_helper_pid, &status, WNOHANG) == 0) {
        kill(g_clipboard_helper_pid, SIGKILL);
        waitpid(g_clipboard_helper_pid, &status, 0);
    }
    g_clipboard_helper_pid = 0;
}

/* Spawn helper if no live one. Caller must hold g_clipboard_lock. */
static void spawn_clipboard_helper_locked(void)
{
    if (g_clipboard_helper_pid > 0) {
        int status;
        pid_t r = waitpid(g_clipboard_helper_pid, &status, WNOHANG);
        if (r == 0) return;                  /* still running */
        g_clipboard_helper_pid = 0;
    }

    uid_t uid = find_graphical_session_uid();
    if (uid == 0) {
        /* No graphical session yet — retry on next monitor tick. */
        return;
    }
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        agent_log("clipboard: getpwuid(%u) failed: %s",
                  (unsigned)uid, strerror(errno));
        return;
    }

    if (!is_user_session_ready(pw->pw_uid)) {
        /* Will retry on the next monitor tick. */
        return;
    }

    int writer_fd = vsock_bind_listen_privileged(5);
    int reader_fd = vsock_bind_listen_privileged(6);
    if (writer_fd < 0 || reader_fd < 0) {
        if (writer_fd >= 0) close(writer_fd);
        if (reader_fd >= 0) close(reader_fd);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        agent_log("clipboard: fork failed: %s", strerror(errno));
        close(writer_fd); close(reader_fd);
        return;
    }

    if (pid == 0) {
        /* Child: prepare fds at 3 and 4 (systemd LISTEN_FDS convention). */
        if (dup2(writer_fd, 3) < 0 || dup2(reader_fd, 4) < 0) {
            fprintf(stderr, "clipboard child: dup2 failed: %s\n", strerror(errno));
            _exit(127);
        }
        if (writer_fd != 3) close(writer_fd);
        if (reader_fd != 4) close(reader_fd);

        if (setgid(pw->pw_gid) < 0 ||
            initgroups(pw->pw_name, pw->pw_gid) < 0 ||
            setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "clipboard child: drop privs failed: %s\n",
                    strerror(errno));
            _exit(127);
        }

        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("DISPLAY", ":0", 1);

        char xdg_rt[64];
        snprintf(xdg_rt, sizeof(xdg_rt), "/run/user/%u", (unsigned)pw->pw_uid);
        setenv("XDG_RUNTIME_DIR", xdg_rt, 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);

        char xauth[512] = {0};
        find_mutter_xauth(pw->pw_uid, xauth, sizeof(xauth));
        if (xauth[0] == '\0')
            snprintf(xauth, sizeof(xauth), "%s/.Xauthority", pw->pw_dir);
        setenv("XAUTHORITY", xauth, 1);

        char fds_str[16], pid_str[16];
        snprintf(fds_str, sizeof(fds_str), "%d", 2);
        snprintf(pid_str, sizeof(pid_str), "%d", (int)getpid());
        setenv("LISTEN_FDS", fds_str, 1);
        setenv("LISTEN_PID", pid_str, 1);

        execl("/usr/local/bin/appsandbox-clipboard",
              "appsandbox-clipboard", (char *)NULL);
        fprintf(stderr, "clipboard child: execl failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(writer_fd);
    close(reader_fd);
    g_clipboard_helper_pid = pid;
    agent_log("clipboard helper spawned (pid=%d, uid=%u, xauth_ok=1)",
              (int)pid, (unsigned)pw->pw_uid);
}

/* Kill+respawn variant used from the idd_connect handler — guarantees a
 * fresh helper bound to a fresh socket so we don't accidentally serve
 * stale state when the host display reconnects. */
static void respawn_clipboard_helper(void)
{
    pthread_mutex_lock(&g_clipboard_lock);
    kill_clipboard_helper_locked();
    spawn_clipboard_helper_locked();
    pthread_mutex_unlock(&g_clipboard_lock);
}

/* Monitor thread: 3s polling, like tools/agent/agent.c
 * clipboard_monitor_thread. Respawns the helper if it died and spawns
 * one the first time the user session becomes ready. */
static void *clipboard_monitor_proc(void *arg)
{
    (void)arg;
    while (g_clipboard_monitor_running) {
        pthread_mutex_lock(&g_clipboard_lock);
        if (g_clipboard_helper_pid > 0) {
            int status;
            pid_t r = waitpid(g_clipboard_helper_pid, &status, WNOHANG);
            if (r > 0) {
                agent_log("clipboard helper exited (pid=%d, status=%d), "
                          "will respawn", (int)g_clipboard_helper_pid, status);
                g_clipboard_helper_pid = 0;
            }
        }
        if (g_clipboard_helper_pid == 0) {
            spawn_clipboard_helper_locked();
        }
        pthread_mutex_unlock(&g_clipboard_lock);

        for (int i = 0; i < 6 && g_clipboard_monitor_running; i++)
            usleep(500 * 1000);
    }
    return NULL;
}

static void start_clipboard_monitor(void)
{
    g_clipboard_monitor_running = 1;
    if (pthread_create(&g_clipboard_monitor_thread, NULL,
                       clipboard_monitor_proc, NULL) != 0) {
        agent_log("clipboard: failed to start monitor thread: %s",
                  strerror(errno));
        g_clipboard_monitor_running = 0;
    }
}

/* ---- Reply helper: prepends the sequence tag if present ---- */

static void send_reply(int fd, const char *tag, const char *msg)
{
    if (tag && tag[0]) {
        char buf[REPLY_MAX];
        snprintf(buf, sizeof(buf), "%s%s", tag, msg);
        send_line(fd, buf);
    } else {
        send_line(fd, msg);
    }
}

/* ---- Heartbeat thread ---- */

static void *heartbeat_thread(void *arg)
{
    (void)arg;
    int idd_last = 0;   /* last reported display-driver readiness (so we only send on change) */
    while (!g_stop) {
        /* Sleep first so the very first heartbeat is at +5s, after hello. */
        for (int s = 0; s < HEARTBEAT_INTERVAL_SEC && !g_stop; s++)
            sleep(1);
        if (g_stop) break;
        int fd = g_client_fd;
        if (fd < 0) break;          /* client disconnected */
        if (send_line(fd, "heartbeat") < 0) {
            agent_log("heartbeat: send failed, exiting thread");
            break;
        }
        /* Report display readiness to the host (the same idd_status the Windows
         * agent sends). The display is serveable once the user session is up --
         * Mutter is compositing, so asb_drm has an active framebuffer for a host
         * consumer to capture. The host latches this as the display-open gate, so
         * it never has to probe the frame channel to find out. send_line is
         * mutex-locked, so this is safe alongside the command thread's replies. */
        {
            uid_t uid = find_graphical_session_uid();
            int ready = (uid != 0 && is_user_session_ready(uid));
            if (ready != idd_last) {
                send_line(fd, ready ? "idd_status:ok" : "idd_status:not_found");
                idd_last = ready;
            }
        }
    }
    return NULL;
}

/* ---- Command handlers ---- */

/* set_ip:<ip>/<prefix>:<gw>
 *   e.g. set_ip:192.168.42.2/24:192.168.42.1
 *
 * Mirrors the Windows agent's `netsh ip set static` clobber semantics:
 *   1. Disable cloud-init's network module so it stops re-writing
 *      /etc/netplan/50-cloud-init.yaml at next boot.
 *   2. Remove every other *.yaml in /etc/netplan so the merge has only
 *      our file to consider — no coexistence games.
 *   3. Write /etc/netplan/99-appsandbox.yaml with the host-assigned
 *      address + gateway + DNS (gateway primary, 8.8.8.8 fallback —
 *      same DNS layout the Windows agent uses).
 *   4. `netplan apply` then `systemctl restart systemd-networkd` so the
 *      kernel actually drops any stale addresses from a prior config.
 *
 * Reply: "ok" on success, "error:..." on failure. */
static void handle_set_ip(int fd, const char *tag, const char *args)
{
    char ip[64], prefix[8], gw[64], dns[160], match[64];
    char *field;
    const char *slash  = strchr(args, '/');
    const char *colon2 = slash ? strchr(slash, ':') : NULL;
    size_t ip_len, pfx_len;
    char cmd[2400];
    int n, rc;

    if (!slash || !colon2) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    ip_len  = (size_t)(slash - args);
    pfx_len = (size_t)(colon2 - slash - 1);
    if (ip_len >= sizeof(ip) || pfx_len >= sizeof(prefix)) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    memcpy(ip,     args,        ip_len);  ip[ip_len]   = '\0';
    memcpy(prefix, slash + 1,   pfx_len); prefix[pfx_len] = '\0';
    snprintf(gw, sizeof(gw), "%s", colon2 + 1);
    /* set_ip:IP/PREFIX:GATEWAY[:MAC[:DNS,DNS]]. netplan matches the NIC by its
       MAC, since a VM with shared drives has a second e* adapter that must not
       get this address, and by name if the host could not read it. DNS defaults
       to the gateway. */
    snprintf(dns, sizeof(dns), "%s, 8.8.8.8", gw);
    snprintf(match, sizeof(match), "name: \"e*\"");
    field = strchr(gw, ':');
    if (field) {
        char *dns_field;
        char mac[18] = "";
        *field++ = '\0';
        snprintf(dns, sizeof(dns), "%s, 8.8.8.8", gw);
        dns_field = strchr(field, ':');
        if (dns_field && (size_t)(dns_field - field) == 17) memcpy(mac, field, 17);
        else if (!dns_field && strlen(field) == 17) memcpy(mac, field, 17);
        if (mac[0]) {
            int valid = 1;
            for (char *p = mac; *p; p++) {
                if (*p == '-') *p = ':';
                else *p = (char)tolower((unsigned char)*p);
                if (!isxdigit((unsigned char)*p) && *p != ':') valid = 0;
            }
            if (valid) snprintf(match, sizeof(match), "macaddress: \"%s\"", mac);
        }
        if (dns_field && dns_field[1]) {
            const char *p;
            for (p = dns_field + 1; *p; p++)
                if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ',')) break;
            if (*p || strlen(dns_field + 1) >= sizeof(dns)) {
                send_reply(fd, tag, "error:bad_format");
                return;
            }
            snprintf(dns, sizeof(dns), "%s", dns_field + 1);
        }
    }

    n = snprintf(cmd, sizeof(cmd),
        /* Pick the renderer by probing whether the NetworkManager service
         * is active (Ubuntu Desktop); otherwise default to systemd-networkd
         * (Server). Writing a netplan with the wrong renderer makes the
         * active one stop managing the NIC entirely (which is why "no
         * network in Ubuntu Settings" happened). */
        "RENDERER=networkd; "
        "if systemctl is-active --quiet NetworkManager; then "
        "  RENDERER=NetworkManager; "
        "fi; "
        /* Disable cloud-init's network module so it stops re-writing
         * /etc/netplan/50-cloud-init.yaml on every boot. Idempotent. */
        "mkdir -p /etc/cloud/cloud.cfg.d && "
        "printf 'network: {config: disabled}\\n' "
        "  > /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg && "
        /* Remove every other netplan dropfile so merge can't reintroduce
         * a conflicting interface key (e.g. cloud-init's `enp0s5: dhcp4`
         * + our `appsbnic` both binding the same NIC). */
        "find /etc/netplan -maxdepth 1 -type f -name '*.yaml' "
        "  ! -name '99-appsandbox.yaml' -delete; "
        /* Our authoritative config. Heredoc is unquoted so $RENDERER
         * expands; nothing else in the YAML uses '$'. */
        "umask 077 && cat > /etc/netplan/99-appsandbox.yaml <<EOF\n"
        "network:\n"
        "  version: 2\n"
        "  renderer: $RENDERER\n"
        "  ethernets:\n"
        "    appsbnic:\n"
        "      match: { %s }\n"
        "      dhcp4: false\n"
        "      dhcp6: false\n"
        "      addresses: [\"%s/%s\"]\n"
        "      routes:\n"
        "        - to: default\n"
        "          via: %s\n"
        "      nameservers:\n"
        "        addresses: [%s]\n"
        "EOF\n"
        /* Apply, then restart the renderer to drop stale leases / state
         * left behind by cloud-init or a previous run. netplan apply
         * alone is sometimes a no-op against a NIC that already has an
         * address the renderer hasn't released.
         *
         * IMPORTANT: reset umask BEFORE `netplan apply`. netplan's
         * systemd-networkd backend writes the generated
         * /run/systemd/network/10-netplan-*.network file via Python
         * open() with no explicit mode, so the file ends up at
         * (0666 & ~umask). With our 077 umask the generated file is 600
         * (root:systemd-network), and systemd-networkd — running as
         * `systemd-network` user, only in the systemd-network group —
         * gets "Permission denied" trying to read it, falls back to the
         * dracut catch-all `.network`, and eth0 ends up with no IP.
         * 022 → generated .network is 644 → systemd-network can read it. */
        "chmod 600 /etc/netplan/99-appsandbox.yaml && "
        "umask 022 && "
        "netplan apply 2>&1; "
        "if [ \"$RENDERER\" = NetworkManager ]; then "
        "  systemctl restart NetworkManager 2>&1; "
        "else "
        "  systemctl restart systemd-networkd 2>&1; "
        "fi; "
        /* Verify the address actually materialised. netplan apply / NM
         * reload are async — the host might try to SSH before the new
         * IP claims the wire. Poll for up to 5 seconds. */
        "for i in 1 2 3 4 5 6 7 8 9 10; do "
        "  ip -4 addr show | grep -q '%s/' && exit 0; "
        "  sleep 0.5; "
        "done; "
        "echo 'set_ip: address never appeared'; ip -4 addr show; exit 1",
        match, ip, prefix, gw, dns, ip);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        send_reply(fd, tag, "error:cmd_too_long");
        return;
    }
    rc = run_sync(cmd);
    agent_log("set_ip: %s/%s via %s → rc=%d", ip, prefix, gw, rc);
    send_reply(fd, tag, rc == 0 ? "ok" : "error:netplan_failed");
}

/* ---- SSH proxy: AF_VSOCK :7 ↔ localhost:22 ----
 *
 * Mirror of tools/agent/agent.c's ssh_proxy_thread + ssh_relay_thread.
 * The host's vm_ssh_proxy.c connects to AF_HYPERV service GUID :0007 —
 * hcs_service_guid(os_type, 7, ...) translates that to AF_VSOCK port 7
 * for Linux VMs — so we listen there and bridge each accepted vsock
 * connection to a fresh TCP socket on 127.0.0.1:22 (sshd).
 *
 * Port 7 is privileged (<1024); the agent runs as root so the bind
 * goes through without extra capability work. Per-connection relays
 * run as detached pthreads so we don't have to track them. */

#define SSH_RELAY_BUF   8192u
#define SSH_VSOCK_PORT  7u
#define SSH_TCP_PORT    22u

static pthread_t        g_ssh_proxy_pthread;
static volatile int     g_ssh_proxy_running = 0;

typedef struct {
    int vsock_fd;
    int tcp_fd;
} ssh_relay_ctx_t;

static int ssh_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *ssh_relay_proc(void *arg)
{
    ssh_relay_ctx_t *r = (ssh_relay_ctx_t *)arg;
    uint8_t buf[SSH_RELAY_BUF];
    struct pollfd pfd[2];

    pfd[0].fd = r->vsock_fd; pfd[0].events = POLLIN;
    pfd[1].fd = r->tcp_fd;   pfd[1].events = POLLIN;

    for (;;) {
        int n = poll(pfd, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfd[0].revents & POLLIN) {
            ssize_t m = recv(r->vsock_fd, buf, sizeof(buf), 0);
            if (m <= 0) break;
            if (ssh_send_all(r->tcp_fd, buf, (size_t)m) < 0) break;
        }
        if (pfd[1].revents & POLLIN) {
            ssize_t m = recv(r->tcp_fd, buf, sizeof(buf), 0);
            if (m <= 0) break;
            if (ssh_send_all(r->vsock_fd, buf, (size_t)m) < 0) break;
        }
        if ((pfd[0].revents | pfd[1].revents) & (POLLHUP | POLLERR | POLLNVAL))
            break;
    }
    close(r->vsock_fd);
    close(r->tcp_fd);
    free(r);
    return NULL;
}

static void *ssh_proxy_proc(void *arg)
{
    (void)arg;

    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) {
        agent_log("ssh proxy: socket(AF_VSOCK) failed: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_vm sa = {0};
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = SSH_VSOCK_PORT;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("ssh proxy: bind vsock :%u failed: %s",
                  SSH_VSOCK_PORT, strerror(errno));
        close(s);
        return NULL;
    }
    if (listen(s, 4) < 0) {
        agent_log("ssh proxy: listen failed: %s", strerror(errno));
        close(s);
        return NULL;
    }
    agent_log("ssh proxy: listening on AF_VSOCK :%u", SSH_VSOCK_PORT);

    while (g_ssh_proxy_running) {
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        int r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("ssh proxy: accept failed: %s", strerror(errno));
            continue;
        }

        /* Per-connection TCP socket → localhost:22. Done eagerly so we
         * surface refused/timeout immediately rather than after one
         * round of relay polling. */
        int t = socket(AF_INET, SOCK_STREAM, 0);
        if (t < 0) { close(c); continue; }
        struct sockaddr_in ta = {0};
        ta.sin_family      = AF_INET;
        ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ta.sin_port        = htons(SSH_TCP_PORT);
        if (connect(t, (struct sockaddr *)&ta, sizeof(ta)) < 0) {
            agent_log("ssh proxy: connect 127.0.0.1:%u failed: %s",
                      SSH_TCP_PORT, strerror(errno));
            close(t); close(c);
            continue;
        }

        ssh_relay_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) { close(t); close(c); continue; }
        ctx->vsock_fd = c;
        ctx->tcp_fd   = t;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t rt;
        if (pthread_create(&rt, &attr, ssh_relay_proc, ctx) != 0) {
            agent_log("ssh proxy: relay pthread_create failed: %s",
                      strerror(errno));
            close(t); close(c); free(ctx);
        }
        pthread_attr_destroy(&attr);
    }

    close(s);
    agent_log("ssh proxy: stopped");
    return NULL;
}

static void start_ssh_proxy(void)
{
    if (g_ssh_proxy_running) return;
    g_ssh_proxy_running = 1;
    if (pthread_create(&g_ssh_proxy_pthread, NULL, ssh_proxy_proc, NULL) != 0) {
        agent_log("ssh proxy: pthread_create failed: %s", strerror(errno));
        g_ssh_proxy_running = 0;
    }
}

/* Find the primary interactive account (the one firstboot's useradd created):
 * a real /home user with a login shell. The username is user-chosen, so we scan
 * the passwd db rather than hardcode it. Returns 1 on success. */
static int find_primary_user(char *home, size_t hsz, uid_t *uid, gid_t *gid)
{
    struct passwd *pw;
    int found = 0;
    setpwent();
    while ((pw = getpwent())) {
        const char *sh, *b;
        if (pw->pw_uid < 1000 || pw->pw_uid >= 65534) continue;
        if (!pw->pw_dir || strncmp(pw->pw_dir, "/home/", 6) != 0) continue;
        sh = pw->pw_shell ? pw->pw_shell : "";
        b = strrchr(sh, '/'); b = b ? b + 1 : sh;
        if (strcmp(b, "nologin") == 0 || strcmp(b, "false") == 0) continue;
        snprintf(home, hsz, "%s", pw->pw_dir);
        *uid = pw->pw_uid; *gid = pw->pw_gid; found = 1;
        break;
    }
    endpwent();
    return found;
}

/* ssh_deploy_key: write the AppSandbox public key into the primary user's
 * ~/.ssh/authorized_keys (0700 dir, 0600 file, owned by the user). */
static int deploy_ssh_key(const char *pubkey)
{
    char home[256], sshdir[320], authk[400];
    uid_t uid; gid_t gid;
    FILE *f;
    if (!pubkey || !*pubkey) return 0;
    if (!find_primary_user(home, sizeof(home), &uid, &gid)) {
        agent_log("ssh key: no primary /home user found");
        return 0;
    }
    snprintf(sshdir, sizeof(sshdir), "%s/.ssh", home);
    snprintf(authk, sizeof(authk), "%s/authorized_keys", sshdir);
    mkdir(sshdir, 0700);
    f = fopen(authk, "w");
    if (!f) { agent_log("ssh key: cannot write %s: %s", authk, strerror(errno)); return 0; }
    fprintf(f, "%s\n", pubkey);
    fclose(f);
    chmod(sshdir, 0700);
    chmod(authk, 0600);
    if (chown(sshdir, uid, gid) != 0 || chown(authk, uid, gid) != 0)
        agent_log("ssh key: chown warning: %s", strerror(errno));
    agent_log("ssh key: deployed to %s", authk);
    return 1;
}

/* ssh_enable: enable+start openssh-server, then start the vsock:7 →
 * localhost:22 proxy. Reply: ssh_ready/ssh_failed. Idempotent — repeat
 * calls just confirm state. */
static void handle_ssh_enable(int fd, const char *tag)
{
    if (run_sync("systemctl is-active --quiet ssh") == 0) {
        agent_log("ssh: already running");
        start_ssh_proxy();
        send_reply(fd, tag, "ssh_ready");
        return;
    }
    int rc = run_sync("systemctl enable --now ssh 2>&1");
    if (rc == 0) {
        agent_log("ssh: enabled and started");
        start_ssh_proxy();
        send_reply(fd, tag, "ssh_ready");
    } else {
        agent_log("ssh: enable failed (rc=%d)", rc);
        send_reply(fd, tag, "ssh_failed");
    }
}

/* gpu_query_response:N — host sent N share descriptors after the header.
 *
 * Wire format per share line: "share_name|guest_path|filter\n" where
 *   share_name = "AppSandbox.HostLxssLib" or "AppSandbox.Drv.N"
 *   guest_path = Windows-format target path (we ignore it except to
 *                lift the leaf folder name for AppSandbox.Drv.N)
 *   filter     = ";"-separated filename whitelist (unused on Linux —
 *                we mount the whole share read-only instead of copying)
 *
 * For each share we open AF_VSOCK to (CID_HOST=2, port=50001), the HCS
 * Plan9 server, and hand the socket fd to the kernel's 9p driver via
 * mount(2) with "trans=fd,rfdno=N,wfdno=N,aname=<share_name>". The
 * kernel 9p client does Tversion + Tattach over the fd; userspace can
 * close its copy after mount() returns (kernel fget()'d the file).
 *
 * After all mounts succeed, write /etc/ld.so.conf.d/wsl.conf with one
 * line per mount path that contains .so files, run ldconfig, and send
 * gpu_copy_done back to the host (best-effort status reply — host
 * doesn't block on it).
 *
 * Mount paths follow the WSL convention:
 *   AppSandbox.HostLxssLib  → /usr/lib/wsl/lib
 *   AppSandbox.Drv.N        → /usr/lib/wsl/drivers/<windows-leaf>
 */

#define VSOCK_PLAN9_PORT  50001u
#define WSL_LIB_DIR       "/usr/lib/wsl/lib"
#define WSL_DRV_BASE      "/usr/lib/wsl/drivers"

/* Recursive mkdir like `mkdir -p`. Best-effort: ignores existing dirs
 * but reports if the final mkdir fails. */
static int mkpath_p(const char *path)
{
    char tmp[1024];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    /* Strip trailing slash (except root). */
    if (len > 1 && tmp[len-1] == '/') tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                agent_log("mkdir %s: %s", tmp, strerror(errno));
                /* keep going — parent might already exist */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Translate a share name (and the Windows guest_path we're handed) into
 * the Linux mount target. Returns 0 if the share is one we recognise,
 * -1 otherwise. */
static int derive_mount_target(const char *share_name,
                               const char *wpath,
                               char *out, size_t outsz)
{
    if (strcmp(share_name, "AppSandbox.HostLxssLib") == 0) {
        snprintf(out, outsz, "%s", WSL_LIB_DIR);
        return 0;
    }
    if (strncmp(share_name, "AppSandbox.Drv.", 15) == 0) {
        /* Extract the leaf folder from the Windows guest_path.
         * Tolerate either backslash or forward slash separators. */
        const char *bs = strrchr(wpath, '\\');
        const char *fs = strrchr(wpath, '/');
        const char *leaf = bs > fs ? bs : fs;
        leaf = leaf ? leaf + 1 : wpath;
        if (!*leaf) return -1;
        snprintf(out, outsz, "%s/%s", WSL_DRV_BASE, leaf);
        return 0;
    }
    return -1;
}

/* Mount one Plan9 share. Returns 0 on success, -1 otherwise. Logs the
 * outcome either way. */
static int mount_plan9_share(const char *share_name, const char *target)
{
    int v;
    struct sockaddr_vm sa = {0};
    char opts[LINE_BUF_MAX + 128];   /* share_name can be a whole command line */

    /* mkdir -p target first — mount(2) requires it to exist. */
    if (mkpath_p(target) < 0) {
        agent_log("gpu_mount: mkdir(%s): %s", target, strerror(errno));
        return -1;
    }

    /* If already mounted (re-run on second agent connect), umount first
     * so the new mount replaces stale state. MNT_DETACH lets the kernel
     * drop refs lazily without blocking on outstanding accesses. */
    if (umount2(target, MNT_DETACH) == 0)
        agent_log("gpu_mount: lazy-unmounted stale %s", target);

    v = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (v < 0) {
        agent_log("gpu_mount: socket(AF_VSOCK): %s", strerror(errno));
        return -1;
    }
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_HOST;   /* 2 = host parent partition */
    sa.svm_port   = VSOCK_PLAN9_PORT;
    if (connect(v, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log("gpu_mount: vsock connect (2,%u): %s",
                  VSOCK_PLAN9_PORT, strerror(errno));
        close(v);
        return -1;
    }

    snprintf(opts, sizeof(opts),
        "trans=fd,rfdno=%d,wfdno=%d,version=9p2000.L,"
        "aname=%s,access=any,msize=65536,cache=loose",
        v, v, share_name);

    if (mount("none", target, "9p", MS_RDONLY | MS_NODEV | MS_NOSUID,
              opts) < 0) {
        agent_log("gpu_mount: mount %s -> %s: %s",
                  share_name, target, strerror(errno));
        close(v);
        return -1;
    }

    /* Kernel held its own reference via fget; we can drop ours. */
    close(v);
    agent_log("gpu_mount: %s -> %s OK", share_name, target);
    return 0;
}

/* Update /etc/ld.so.conf.d/wsl.conf to list every mount path that holds
 * .so files, then run ldconfig. Idempotent — overwrites each time. */
static void refresh_ldconfig_for_mounts(char (*paths)[1024], int n_paths)
{
    FILE *f = fopen("/etc/ld.so.conf.d/wsl.conf", "w");
    if (!f) {
        agent_log("ldconfig: open wsl.conf: %s", strerror(errno));
        return;
    }
    fprintf(f, "# Auto-generated by appsandbox-agent. Do not edit.\n");
    for (int i = 0; i < n_paths; i++)
        fprintf(f, "%s\n", paths[i]);
    fclose(f);

    int rc = run_sync("ldconfig 2>&1");
    agent_log("ldconfig refreshed (%d paths, rc=%d)", n_paths, rc);
}

static void handle_gpu_query_response(int fd, int n_shares)
{
    if (n_shares <= 0 || n_shares > 64) {
        agent_log("gpu_query: bad share count %d", n_shares);
        return;
    }

    char ldpaths[64][1024];
    int  n_ldpaths = 0;
    int  n_mounted = 0;

    for (int i = 0; i < n_shares; i++) {
        char line[LINE_BUF_MAX];
        int n = recv_line(fd, line, sizeof(line));
        if (n <= 0) {
            agent_log("gpu_query: short read on share %d", i);
            return;
        }
        agent_log("gpu_share[%d]: %s", i, line);

        /* Parse "share_name|guest_path|filter". filter may be empty. */
        char *p1 = strchr(line, '|');
        if (!p1) { agent_log("gpu_query: malformed: %s", line); continue; }
        *p1++ = '\0';
        char *p2 = strchr(p1, '|');
        if (p2) *p2++ = '\0';

        const char *share = line;        /* before first '|' */
        const char *wpath = p1;          /* between '|'s */
        (void)p2;                        /* filter — unused on Linux */

        char target[1024];
        if (derive_mount_target(share, wpath, target, sizeof(target)) < 0) {
            agent_log("gpu_query: unknown share name '%s', skipping", share);
            continue;
        }

        if (mount_plan9_share(share, target) < 0)
            continue;

        if (n_ldpaths < (int)(sizeof(ldpaths) / sizeof(ldpaths[0]))) {
            strncpy(ldpaths[n_ldpaths], target, sizeof(ldpaths[0]) - 1);
            ldpaths[n_ldpaths][sizeof(ldpaths[0]) - 1] = '\0';
            n_ldpaths++;
        }
        n_mounted++;
    }

    if (n_ldpaths > 0)
        refresh_ldconfig_for_mounts(ldpaths, n_ldpaths);

    /* Status reply to host. vm_agent.c just logs it; we send anyway so
     * the host log line "GPU copy complete for ..." fires and the user
     * sees confirmation that the share pipeline finished. */
    char ack[64];
    snprintf(ack, sizeof(ack), "gpu_copy_done:%d", n_mounted);
    send_line(g_client_fd, ack);
    agent_log("gpu_query: mounted %d/%d shares", n_mounted, n_shares);
}

static int grow_root_filesystem(void)
{
    const char *cmd =
        "fstype=$(findmnt -n -o FSTYPE /) && [ \"$fstype\" = ext4 ] && "
        "command -v growpart >/dev/null 2>&1 && command -v resize2fs >/dev/null 2>&1 && "
        "root=$(readlink -f \"$(findmnt -n -o SOURCE /)\") && "
        "part=$(lsblk -no PARTN \"$root\" | tr -d ' ') && "
        "parent=$(lsblk -no PKNAME \"$root\" | tr -d ' ') && "
        "[ -n \"$part\" ] && [ -n \"$parent\" ] && "
        "growpart \"/dev/$parent\" \"$part\" && resize2fs \"$root\"";
    int rc = run_sync(cmd);
    agent_log("grow_root: rc=%d", rc);
    return rc;
}

static int appliance_token_valid(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    if (!p || !*p) return 0;
    for (; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '$') return 0;
    return 1;
}

static int configure_shared_nic(const char *arguments)
{
    char copy[128], subnet[64], *separator, *dot, *p, command[1024];
    snprintf(copy, sizeof(copy), "%s", arguments);
    separator = strrchr(copy, ':');
    if (!separator) return EINVAL;
    *separator++ = '\0';
    if (strlen(copy) != 17 || !*separator) return EINVAL;
    /* The host speaks Windows MAC notation; ip(8) prints colons. */
    for (p = copy; *p; ++p) {
        if (*p == '-') *p = ':';
        if (!isxdigit((unsigned char)*p) && *p != ':') return EINVAL;
    }
    /* Both values are interpolated into a shell command. */
    for (p = separator; *p; ++p)
        if (!isdigit((unsigned char)*p) && *p != '.') return EINVAL;
    snprintf(subnet, sizeof(subnet), "%s", separator);
    dot = strrchr(subnet, '.');
    if (!dot) return EINVAL;
    snprintf(dot, sizeof(subnet) - (size_t)(dot - subnet), ".0/24");
    /* The share network is LAN-only: drop any default route DHCP gave this
       adapter and refuse egress off the share subnet, so the appliance cannot
       reach the internet through it. Best effort: the address is what the
       shares need, and HCN ACLs on the host enforce the same boundary. */
    /* On a desktop guest NetworkManager would run DHCP on this adapter and take
       the address away again, so it is told to leave this MAC alone first. */
    snprintf(command, sizeof(command),
        "iface=$(ip -o link | awk -v mac='%s' 'tolower($0) ~ tolower(mac){gsub(\":\",\"\",$2);print $2;exit}'); "
        "if systemctl is-active --quiet NetworkManager; then "
        "printf '[keyfile]\\nunmanaged-devices=mac:%s\\n' > /etc/NetworkManager/conf.d/90-appsandbox-share.conf; "
        "nmcli general reload conf 2>/dev/null; nmcli device set $iface managed no 2>/dev/null; fi; "
        "[ -n \"$iface\" ] && ip link set $iface up && ip addr replace %s/24 dev $iface && "
        "{ ip route del default dev $iface 2>/dev/null; "
        "iptables -C OUTPUT -o $iface ! -d %s -j DROP 2>/dev/null || "
        "iptables -A OUTPUT -o $iface ! -d %s -j DROP 2>/dev/null; true; }",
        copy, copy, separator, subnet, subnet);
    return run_sync(command);
}

/* ---- Shared drives ----
   Same command the Windows agent gets: shared_smb_map:L:HOST:SHARE:USER:PASSWORD.
   Linux has no drive letters, so each drive mounts at /mnt/appsandbox/<L> and gets
   a Files sidebar bookmark. The password goes in a root-only file, never on a
   command line. Retries because the appliance's SMB may still be starting. */

#define SHARED_MOUNT_ROOT "/mnt/appsandbox"

static void add_files_bookmark(const char *letter, const char *share)
{
    char home[256], dir[320], path[360], line[256], want[128];
    uid_t uid; gid_t gid;
    FILE *f;
    if (!find_primary_user(home, sizeof(home), &uid, &gid)) return;
    snprintf(dir, sizeof(dir), "%s/.config/gtk-3.0", home);
    snprintf(path, sizeof(path), "%s/bookmarks", dir);
    snprintf(want, sizeof(want), "file://" SHARED_MOUNT_ROOT "/%s ", letter);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, want, strlen(want)) == 0) { fclose(f); return; }
        }
        fclose(f);
    }
    mkpath_p(dir);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s%s: %s\n", want, letter, share);
    fclose(f);
    {
        char home_cfg[300];
        snprintf(home_cfg, sizeof(home_cfg), "%s/.config", home);
        if (chown(home_cfg, uid, gid) != 0 || chown(dir, uid, gid) != 0 ||
            chown(path, uid, gid) != 0)
            agent_log("shared drive: bookmark chown: %s", strerror(errno));
    }
}

static int shared_valid_host(const char *host)
{
    const char *p;
    if (!*host) return 0;
    for (p = host; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    return 1;
}

static int shared_valid_letter(const char *letter)
{
    return letter && letter[0] >= 'A' && letter[0] <= 'Z' && !letter[1];
}

static int shared_map(char *args)
{
    char *save = NULL, command[768], home[256], cred[] = "/run/appsandbox-cifs-XXXXXX";
    char *letter = strtok_r(args, ":", &save);
    char *host = strtok_r(NULL, ":", &save);
    char *share = strtok_r(NULL, ":", &save);
    char *user = strtok_r(NULL, ":", &save);
    char *password = strtok_r(NULL, "", &save);
    char *domain = NULL;
    uid_t uid = 0; gid_t gid = 0;
    FILE *file;
    int fd, rc;

    /* The appliance's account is USER or DOMAIN\\USER. */
    if (user) {
        char *sep = strchr(user, '\\');
        if (sep) { *sep = '\0'; domain = user; user = sep + 1; }
    }
    if (domain && !shared_valid_host(domain) && !appliance_token_valid(domain))
        return EINVAL;
    if (!shared_valid_letter(letter) || !host || !shared_valid_host(host) ||
        !share || !appliance_token_valid(share) ||
        !user || !appliance_token_valid(user) || !password || !*password)
        return EINVAL;
    find_primary_user(home, sizeof(home), &uid, &gid);

    fd = mkstemp(cred);
    if (fd < 0) return errno;
    fchmod(fd, 0600);
    file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(cred); return errno; }
    fprintf(file, "username=%s\npassword=%s\n", user, password);
    if (domain) fprintf(file, "domain=%s\n", domain);
    fclose(file);
    memset(password, 0, strlen(password));

    snprintf(command, sizeof(command),
        "m=" SHARED_MOUNT_ROOT "/%s; mkdir -p \"$m\"; "
        "mountpoint -q \"$m\" && umount -l \"$m\"; "
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do "
        "mount -t cifs //%s/%s \"$m\" -o credentials=%s,uid=%u,gid=%u,"
        "file_mode=0775,dir_mode=0775,nosuid,nodev && exit 0; sleep 5; done; exit 32",
        letter, host, share, cred, (unsigned)uid, (unsigned)gid);
    rc = run_sync(command);
    unlink(cred);
    if (rc == 0) add_files_bookmark(letter, share);
    return rc;
}

static int shared_unmap(const char *letter)
{
    char command[256];
    if (!shared_valid_letter(letter)) return EINVAL;
    snprintf(command, sizeof(command),
        "m=" SHARED_MOUNT_ROOT "/%s; ! mountpoint -q \"$m\" || umount -l \"$m\"", letter);
    return run_sync(command);
}

/* ---- Apps chosen at creation ----
   install_apps:a,b,c, the command the Windows agent takes with winget IDs. Here
   each name is an apt package, or else a snap. Kept in a file across reboots, and
   a name that keeps failing is dropped after a few starts like the Windows agent. */

#define APPS_FILE      "/var/lib/appsandbox/apps-pending"
#define APPS_ATTEMPTS  "/var/lib/appsandbox/apps-attempts"
#define APPS_MAX_ATTEMPTS 3

static volatile int g_apps_running = 0;

static void host_log(const char *fmt, ...)
{
    char line[1024];
    size_t used;
    va_list ap;
    int fd = g_client_fd;
    used = (size_t)snprintf(line, sizeof(line), "log:");
    va_start(ap, fmt);
    vsnprintf(line + used, sizeof(line) - used, fmt, ap);
    va_end(ap);
    agent_log("%s", line + used);
    if (fd >= 0) send_line(fd, line);
}

static int apps_valid_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    if (!*p || strlen(name) > 100 || *p == '-') return 0;
    for (; *p; p++)
        if (!isalnum(*p) && *p != '.' && *p != '+' && *p != '_' && *p != '-') return 0;
    return 1;
}

static void *apps_thread(void *arg)
{
    char names[1024] = "", failed[1024] = "", ok[1024] = "";
    char *save = NULL, *name;
    FILE *f;
    int attempts = 0, i;
    (void)arg;

    f = fopen(APPS_FILE, "r");
    if (f) {
        if (!fgets(names, sizeof(names), f)) names[0] = '\0';
        fclose(f);
    }
    names[strcspn(names, "\r\n")] = '\0';
    if (!names[0]) goto done;
    f = fopen(APPS_ATTEMPTS, "r");
    if (f) { if (fscanf(f, "%d", &attempts) != 1) attempts = 0; fclose(f); }
    f = fopen(APPS_ATTEMPTS, "w");
    if (f) { fprintf(f, "%d\n", attempts + 1); fclose(f); }

    /* Wait for the network the host configures after connecting. */
    for (i = 0; i < 60; i++) {
        if (run_sync("getent hosts archive.ubuntu.com >/dev/null 2>&1") == 0) break;
        sleep(5);
    }
    host_log("Installing the apps chosen for this VM: %s", names);
    run_sync("DEBIAN_FRONTEND=noninteractive apt-get update >/dev/null 2>&1");
    for (name = strtok_r(names, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
        char cmd[512], *list = failed;
        if (apps_valid_name(name)) {
            snprintf(cmd, sizeof(cmd),
                "DEBIAN_FRONTEND=noninteractive apt-get install -y %s >/dev/null 2>&1 || "
                "snap install %s >/dev/null 2>&1 || snap install %s --classic >/dev/null 2>&1",
                name, name, name);
            if (run_sync(cmd) == 0) list = ok;
        }
        size_t used = strlen(list);
        snprintf(list + used, sizeof(ok) - used, "%s%s", used ? "," : "", name);
    }
    if (ok[0]) host_log("Installed apps: %s", ok);
    if (failed[0] && attempts + 1 < APPS_MAX_ATTEMPTS) {
        f = fopen(APPS_FILE, "w");
        if (f) { fprintf(f, "%s\n", failed); fclose(f); }
        host_log("Could not install: %s (retried at the next start)", failed);
    } else {
        if (failed[0]) host_log("Giving up on installing: %s", failed);
        unlink(APPS_FILE);
        unlink(APPS_ATTEMPTS);
    }
done:
    g_apps_running = 0;
    return NULL;
}

static void start_app_install(void)
{
    pthread_t t;
    struct stat st;
    if (stat(APPS_FILE, &st) != 0 || g_apps_running) return;
    g_apps_running = 1;
    if (pthread_create(&t, NULL, apps_thread, NULL) == 0) pthread_detach(t);
    else g_apps_running = 0;
}

static int queue_apps(const char *names)
{
    FILE *f;
    mkpath_p("/var/lib/appsandbox");
    f = fopen(APPS_FILE, "w");
    if (!f) return 0;
    fprintf(f, "%s\n", names);
    fclose(f);
    unlink(APPS_ATTEMPTS);
    return 1;
}

/* ---- Guest refresh ----
   The Linux counterpart of the Windows helper refresh: the host compares the
   stamp of the guest source it ships with the one this VM was built from, and
   sends a tar of the source if they differ. The update script builds and installs
   it, which restarts this agent; a changed display driver waits for the reboot. */

#define GUEST_STAMP_FILE  "/etc/appsandbox-guest-stamp"
#define GUEST_UPDATE_DIR  "/var/lib/appsandbox/update"
#define GUEST_UPDATE_MAX  (32u * 1024u * 1024u)

static int stamp_valid(const char *stamp)
{
    const unsigned char *p = (const unsigned char *)stamp;
    if (!*p || strlen(stamp) > 128) return 0;
    for (; *p; p++) if (!isxdigit(*p)) return 0;
    return 1;
}

static void handle_guest_stamp(int fd, const char *tag)
{
    char stamp[160] = "none", reply[192];
    FILE *f = fopen(GUEST_STAMP_FILE, "r");
    if (f) {
        if (!fgets(stamp, sizeof(stamp), f)) snprintf(stamp, sizeof(stamp), "none");
        stamp[strcspn(stamp, "\r\n")] = '\0';
        fclose(f);
    }
    snprintf(reply, sizeof(reply), "stamp:%s", stamp);
    send_reply(fd, tag, reply);
}

/* guest_update:STAMP:BYTES, then BYTES of .tar.gz on this socket. */
static void handle_guest_update(int fd, const char *tag, const char *args)
{
    char stamp[160], path[] = GUEST_UPDATE_DIR ".tar.gz", cmd[512], buf[65536];
    const char *colon = strchr(args, ':');
    unsigned long left = 0;
    size_t len;
    FILE *f;

    if (!colon) { send_reply(fd, tag, "error:bad_format"); return; }
    len = (size_t)(colon - args);
    left = strtoul(colon + 1, NULL, 10);
    if (len >= sizeof(stamp) || !left || left > GUEST_UPDATE_MAX) {
        send_reply(fd, tag, "error:bad_format");
        return;
    }
    memcpy(stamp, args, len);
    stamp[len] = '\0';
    mkpath_p("/var/lib/appsandbox");
    f = fopen(path, "wb");
    /* The bytes are on the wire whatever happens, so always read them all. */
    while (left) {
        ssize_t n = read(fd, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (f) fclose(f); return; }
        if (f && fwrite(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); f = NULL; }
        left -= (unsigned long)n;
    }
    if (!f || fclose(f) != 0 || !stamp_valid(stamp)) {
        send_reply(fd, tag, "error:write");
        return;
    }
    snprintf(cmd, sizeof(cmd),
        "rm -rf " GUEST_UPDATE_DIR " && mkdir -p " GUEST_UPDATE_DIR " && "
        "tar -xzf %s -C " GUEST_UPDATE_DIR, path);
    if (run_sync(cmd) != 0) { send_reply(fd, tag, "error:extract"); return; }
    send_reply(fd, tag, "ok");
    snprintf(cmd, sizeof(cmd),
        "systemd-run --no-block --collect --unit=appsandbox-guest-update "
        "/bin/bash " GUEST_UPDATE_DIR "/tools/linux/agent/appsandbox-guest-update.sh %s",
        stamp);
    if (run_sync(cmd) != 0) host_log("Could not start the guest refresh.");
}

static int appliance_prepare_storage(void)
{
    return run_sync(
        "mkdir -p /srv/appsandbox; "
        "disk=$(lsblk -dpno NAME,TYPE | awk '$2==\"disk\"{print $1}' | tail -1); "
        "part=${disk}1; "
        "if ! blkid $part >/dev/null 2>&1; then parted -s $disk mklabel gpt mkpart primary ext4 1MiB 100% && partprobe $disk && mkfs.ext4 -F -L ASBSHARED $part; fi; "
        "grep -q 'LABEL=ASBSHARED' /etc/fstab || echo 'LABEL=ASBSHARED /srv/appsandbox ext4 defaults,nofail 0 2' >> /etc/fstab; "
        "mountpoint -q /srv/appsandbox || mount /srv/appsandbox");
}

static int appliance_set_account(const char *arguments)
{
    char copy[512], *separator, command[1152], temp[] = "/run/appsandbox-smb-XXXXXX";
    FILE *file;
    int fd, rc;
    snprintf(copy, sizeof(copy), "%s", arguments);
    separator = strchr(copy, ':');
    if (!separator) return EINVAL;
    *separator++ = '\0';
    if (!appliance_token_valid(copy) || !*separator) return EINVAL;
    snprintf(command, sizeof(command),
        "id -u %s >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin %s",
        copy, copy);
    if (run_sync(command) != 0) return EIO;
    fd = mkstemp(temp);
    if (fd < 0) return errno;
    fchmod(fd, 0600);
    file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temp); return errno; }
    fprintf(file, "%s\n%s\n", separator, separator);
    fclose(file);
    snprintf(command, sizeof(command), "smbpasswd -s -a %s < %s", copy, temp);
    rc = run_sync(command);
    unlink(temp);
    memset(copy, 0, sizeof(copy));
    return rc;
}

/* Samba's include takes one file and does not expand wildcards, so the
   per-share files are concatenated into appsandbox-shares.conf and that file
   is included. Also drops the old "include = .../appsandbox-shares/*.conf"
   line, which never loaded anything. */
static int appliance_rebuild_share_include(void)
{
    return run_sync(
        "{ cat /etc/samba/appsandbox-shares/*.conf 2>/dev/null || :; } > /etc/samba/appsandbox-shares.conf && "
        "if grep -qxF 'include = /etc/samba/appsandbox-shares/*.conf' /etc/samba/smb.conf; then "
        "grep -vxF 'include = /etc/samba/appsandbox-shares/*.conf' /etc/samba/smb.conf > /etc/samba/smb.conf.asb && "
        "mv /etc/samba/smb.conf.asb /etc/samba/smb.conf; fi && "
        "{ grep -qx 'include = /etc/samba/appsandbox-shares.conf' /etc/samba/smb.conf || "
        "echo 'include = /etc/samba/appsandbox-shares.conf' >> /etc/samba/smb.conf; }");
}

static int appliance_reconcile_share(const char *arguments)
{
    char copy[256], *mode, directory[256], path[320], command[768];
    FILE *file;
    snprintf(copy, sizeof(copy), "%s", arguments);
    mode = strchr(copy, ':');
    if (!mode) return EINVAL;
    *mode++ = '\0';
    if (!appliance_token_valid(copy) ||
        (strcmp(mode, "ro") != 0 && strcmp(mode, "rw") != 0)) return EINVAL;
    snprintf(directory, sizeof(directory), "%s", copy);
    { char *dollar = strchr(directory, '$'); if (dollar) *dollar = '\0'; }
    snprintf(command, sizeof(command),
        "mkdir -p /srv/appsandbox/%s /etc/samba/appsandbox-shares && chown AppSandboxShare:AppSandboxShare /srv/appsandbox/%s",
        directory, directory);
    if (run_sync(command) != 0) return EIO;
    snprintf(path, sizeof(path), "/etc/samba/appsandbox-shares/%s.conf", directory);
    file = fopen(path, "w");
    if (!file) return errno;
    fprintf(file,
        "[%s]\npath = /srv/appsandbox/%s\nbrowseable = no\nvalid users = AppSandboxShare\nread only = %s\nforce user = AppSandboxShare\n",
        copy, directory, strcmp(mode, "ro") == 0 ? "yes" : "no");
    fclose(file);
    if (appliance_rebuild_share_include() != 0)
        return EIO;
    return run_sync("testparm -s >/dev/null 2>&1 && systemctl reload smbd");
}

static int appliance_purge_share(const char *share)
{
    char directory[128], command[512];
    if (!appliance_token_valid(share)) return EINVAL;
    snprintf(directory, sizeof(directory), "%s", share);
    { char *dollar = strchr(directory, '$'); if (dollar) *dollar = '\0'; }
    snprintf(command, sizeof(command),
        "rm -f /etc/samba/appsandbox-shares/%s.conf && rm -rf -- /srv/appsandbox/%s",
        directory, directory);
    if (run_sync(command) != 0 || appliance_rebuild_share_include() != 0) return EIO;
    return run_sync("systemctl reload smbd");
}

static int appliance_remove_share(const char *share)
{
    char directory[128], command[384];
    if (!appliance_token_valid(share)) return EINVAL;
    snprintf(directory, sizeof(directory), "%s", share);
    { char *dollar = strchr(directory, '$'); if (dollar) *dollar = '\0'; }
    snprintf(command, sizeof(command),
        "rm -f /etc/samba/appsandbox-shares/%s.conf",
        directory);
    if (run_sync(command) != 0 || appliance_rebuild_share_include() != 0) return EIO;
    return run_sync("systemctl reload smbd");
}

/* ---- VPN gateway ----
   Used only on the VPN gateway appliance. Each WireGuard config runs as
   interface asbvpn<slot> with its own routing table (100 + slot); a VM's
   source IP is bound to one table with an ip rule. Everything from the VPN
   LAN that no rule claims falls to table 99, which is unreachable, and the
   ASBVPN chain only forwards LAN traffic into asbvpn interfaces: if a tunnel
   is down, its VMs have no route out at all. */

static int vpn_valid_chars(const char *s, const char *allowed, size_t max_len)
{
    size_t n = 0;
    if (!s || !*s) return 0;
    for (; *s; s++, n++) {
        if (n >= max_len) return 0;
        if (!((*s >= '0' && *s <= '9') || strchr(allowed, *s))) return 0;
    }
    return 1;
}

static int vpn_slot(const char *s)
{
    int slot;
    if (!vpn_valid_chars(s, "", 2)) return -1;
    slot = atoi(s);
    return slot >= 0 && slot < 16 ? slot : -1;
}

static int vpn_init(const char *arguments)
{
    char command[2600], lan[24], self[24] = "";
    const char *colon = strchr(arguments, ':');
    size_t n = colon ? (size_t)(colon - arguments) : strlen(arguments);
    if (n == 0 || n >= sizeof(lan)) return EINVAL;
    memcpy(lan, arguments, n); lan[n] = '\0';
    if (colon) snprintf(self, sizeof(self), "%s", colon + 1);
    if (!vpn_valid_chars(lan, "./", 18) || (self[0] && !vpn_valid_chars(self, ".", 15)))
        return EINVAL;
    snprintf(command, sizeof(command),
        /* Loose reverse-path filtering: replies arrive on an asbvpn
           interface, while the main table routes the internet via the
           uplink, so strict mode would drop them. */
        "sysctl -qw net.ipv4.ip_forward=1 net.ipv4.conf.all.rp_filter=2 net.ipv4.conf.default.rp_filter=2; "
        "iptables -N ASBVPN 2>/dev/null; iptables -F ASBVPN && "
        "{ iptables -C FORWARD -j ASBVPN 2>/dev/null || iptables -I FORWARD 1 -j ASBVPN; } && "
        "iptables -A ASBVPN -s %s -o asbvpn+ -j ACCEPT && "
        "iptables -A ASBVPN -d %s -i asbvpn+ -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT && "
        "iptables -A ASBVPN -s %s -j DROP && iptables -A ASBVPN -d %s -j DROP && "
        "{ iptables -C INPUT -s %s -j DROP 2>/dev/null || iptables -I INPUT 1 -s %s -j DROP; } && "
        "{ iptables -t nat -C POSTROUTING -o asbvpn+ -j MASQUERADE 2>/dev/null || "
        "iptables -t nat -A POSTROUTING -o asbvpn+ -j MASQUERADE; } && "
        "{ iptables -t mangle -C FORWARD -o asbvpn+ -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || "
        "iptables -t mangle -A FORWARD -o asbvpn+ -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu; } && "
        "ip route replace unreachable default table 99 && "
        "{ ip rule show | grep -q 'from %s lookup 99' || ip rule add from %s lookup 99 priority 32000; }",
        lan, lan, lan, lan, lan, lan, lan, lan);
    if (run_sync(command) != 0) return EIO;
    /* The gateway's own LAN address is inside the LAN range, so without
       this its replies (ICMP errors, ARP-driven traffic) hit table 99 too. */
    if (self[0]) {
        snprintf(command, sizeof(command),
            "ip rule show | grep -q 'from %s lookup main' || ip rule add from %s lookup main priority 31000",
            self, self);
        if (run_sync(command) != 0) return EIO;
    }
    return 0;
}

/* vpn_set:<slot>:<base64 config>. Replies ok once the tunnel has a handshake,
   or no_handshake if the interface is up but the peer hasn't answered yet. */
static const char *vpn_set(const char *arguments)
{
    char slot_text[4], command[4096];
    const char *colon = strchr(arguments, ':');
    int slot, rc;
    size_t n;
    if (!colon) return "error:bad_format";
    n = (size_t)(colon - arguments);
    if (n == 0 || n >= sizeof(slot_text)) return "error:bad_format";
    memcpy(slot_text, arguments, n); slot_text[n] = '\0';
    slot = vpn_slot(slot_text);
    if (slot < 0 || !vpn_valid_chars(colon + 1,
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/=", 3000))
        return "error:bad_format";
    snprintf(command, sizeof(command),
        "mkdir -p /etc/wireguard && umask 077 && "
        "echo '%s' | base64 -d > /etc/wireguard/asbvpn%d.conf && "
        "{ wg-quick down asbvpn%d >/dev/null 2>&1 || true; } && "
        "wg-quick up asbvpn%d && ip route replace default dev asbvpn%d table %d || exit 1; "
        "for i in $(seq 1 30); do "
        "  t=$(wg show asbvpn%d latest-handshakes | awk '{print $2}'); "
        "  [ -n \"$t\" ] && [ \"$t\" -gt 0 ] && exit 0; sleep 0.5; "
        "done; exit 3",
        colon + 1, slot, slot, slot, slot, 100 + slot, slot);
    rc = run_sync(command);
    memset(command, 0, sizeof(command));
    return rc == 0 ? "ok" : rc == 3 ? "ok:no_handshake" : "error:wg_up";
}

static int vpn_bind(const char *arguments)
{
    char slot_text[4], ip[20], command[512];
    const char *colon = strchr(arguments, ':');
    int slot;
    size_t n;
    if (!colon) return EINVAL;
    n = (size_t)(colon - arguments);
    if (n == 0 || n >= sizeof(slot_text)) return EINVAL;
    memcpy(slot_text, arguments, n); slot_text[n] = '\0';
    slot = vpn_slot(slot_text);
    if (slot < 0 || !vpn_valid_chars(colon + 1, ".", 15)) return EINVAL;
    snprintf(ip, sizeof(ip), "%s", colon + 1);
    snprintf(command, sizeof(command),
        "while ip rule del from %s/32 2>/dev/null; do :; done; "
        "ip rule add from %s/32 lookup %d priority 10000",
        ip, ip, 100 + slot);
    return run_sync(command) == 0 ? 0 : EIO;
}

static int vpn_unbind(const char *ip)
{
    char command[256];
    if (!vpn_valid_chars(ip, ".", 15)) return EINVAL;
    snprintf(command, sizeof(command),
        "while ip rule del from %s/32 2>/dev/null; do :; done", ip);
    return run_sync(command) == 0 ? 0 : EIO;
}

static int vpn_down(const char *slot_text)
{
    char command[256];
    int slot = vpn_slot(slot_text);
    if (slot < 0) return EINVAL;
    snprintf(command, sizeof(command),
        "wg-quick down asbvpn%d >/dev/null 2>&1; rm -f /etc/wireguard/asbvpn%d.conf; "
        "ip route flush table %d 2>/dev/null; exit 0",
        slot, slot, 100 + slot);
    return run_sync(command) == 0 ? 0 : EIO;
}

/* ---- Per-client command dispatch ---- */

static void handle_client(int fd)
{
    pthread_t hb;
    int       hb_started = 0;
    char      line[LINE_BUF_MAX];

    g_client_fd = fd;
    agent_log("client connected (fd=%d)", fd);

    if (send_line(fd, "hello") < 0) {
        agent_log("hello: send failed");
        goto out;
    }

    if (pthread_create(&hb, NULL, heartbeat_thread, NULL) != 0) {
        agent_log("heartbeat: pthread_create failed: %s", strerror(errno));
    } else {
        hb_started = 1;
    }

    while (!g_stop) {
        int   n;
        char  tag[32] = "";
        const char *cmd;
        char *colon;

        n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;

        /* Strip optional "<digits>:" sequence tag */
        cmd   = line;
        colon = strchr(line, ':');
        if (colon && colon > line && (size_t)(colon - line) < sizeof(tag) - 1) {
            int is_seq = 1;
            for (char *p = line; p < colon; p++) {
                if (*p < '0' || *p > '9') { is_seq = 0; break; }
            }
            if (is_seq) {
                size_t t = (size_t)(colon - line + 1);
                memcpy(tag, line, t);
                tag[t] = '\0';
                cmd = colon + 1;
            }
        }

        if (strncmp(cmd, "appliance_account:", 18) == 0)
            agent_log("cmd: appliance_account:<redacted>");
        else if (strncmp(cmd, "shared_smb_map:", 15) == 0)
            agent_log("cmd: shared_smb_map:<redacted>");
        else if (strncmp(cmd, "vpn_set:", 8) == 0)
            agent_log("cmd: vpn_set:<redacted>");
        else
            agent_log("cmd: %s", line);

        if (strcmp(cmd, "ping") == 0) {
            send_reply(fd, tag, "ok");
        }
        else if (strcmp(cmd, "appliance_ready") == 0) {
            send_reply(fd, tag, appliance_prepare_storage() == 0 ? "ok" : "error:storage");
        }
        else if (strncmp(cmd, "appliance_account:", 18) == 0) {
            send_reply(fd, tag, appliance_set_account(cmd + 18) == 0 ? "ok" : "error:account");
        }
        else if (strncmp(cmd, "appliance_reconcile:", 20) == 0) {
            send_reply(fd, tag, appliance_reconcile_share(cmd + 20) == 0 ? "ok" : "error:share");
        }
        else if (strncmp(cmd, "appliance_purge:", 16) == 0) {
            send_reply(fd, tag, appliance_purge_share(cmd + 16) == 0 ? "ok" : "error:purge");
        }
        else if (strncmp(cmd, "vpn_init:", 9) == 0) {
            send_reply(fd, tag, vpn_init(cmd + 9) == 0 ? "ok" : "error:vpn_init");
        }
        else if (strncmp(cmd, "vpn_set:", 8) == 0) {
            send_reply(fd, tag, vpn_set(cmd + 8));
        }
        else if (strncmp(cmd, "vpn_bind:", 9) == 0) {
            send_reply(fd, tag, vpn_bind(cmd + 9) == 0 ? "ok" : "error:vpn_bind");
        }
        else if (strncmp(cmd, "vpn_unbind:", 11) == 0) {
            send_reply(fd, tag, vpn_unbind(cmd + 11) == 0 ? "ok" : "error:vpn_unbind");
        }
        else if (strncmp(cmd, "vpn_down:", 9) == 0) {
            send_reply(fd, tag, vpn_down(cmd + 9) == 0 ? "ok" : "error:vpn_down");
        }
        else if (strncmp(cmd, "appliance_remove:", 17) == 0) {
            send_reply(fd, tag, appliance_remove_share(cmd + 17) == 0 ? "ok" : "error:remove");
        }
        else if (strncmp(cmd, "appliance_grow:", 15) == 0) {
            send_reply(fd, tag, run_sync(
                "part=$(findmnt -n -o SOURCE /srv/appsandbox); disk=/dev/$(lsblk -no PKNAME $part); number=$(lsblk -no PARTN $part); growpart $disk $number && resize2fs $part") == 0
                ? "ok" : "error:grow");
        }
        else if (strcmp(cmd, "appliance_update") == 0) {
            send_reply(fd, tag, run_sync(
                "DEBIAN_FRONTEND=noninteractive apt-get update && DEBIAN_FRONTEND=noninteractive apt-get -y upgrade") == 0
                ? "ok" : "error:update");
        }
        else if (strncmp(cmd, "shared_net:", 11) == 0) {
            send_reply(fd, tag, configure_shared_nic(cmd + 11) == 0 ? "ok" : "net_failed");
        }
        else if (strncmp(cmd, "shared_smb_map:", 15) == 0) {
            char failure[64];
            int rc = shared_map((char *)cmd + 15);
            snprintf(failure, sizeof(failure), "map_failed:%d", rc);
            send_reply(fd, tag, rc == 0 ? "ok" : failure);
        }
        else if (strncmp(cmd, "shared_smb_unmap:", 17) == 0) {
            send_reply(fd, tag, shared_unmap(cmd + 17) == 0 ? "ok" : "unmap_failed");
        }
        else if (strncmp(cmd, "install_apps:", 13) == 0) {
            int queued = queue_apps(cmd + 13);
            send_reply(fd, tag, queued ? "ok" : "error:queue");
            if (queued) start_app_install();
        }
        else if (strcmp(cmd, "guest_stamp") == 0) {
            handle_guest_stamp(fd, tag);
        }
        else if (strncmp(cmd, "guest_update:", 13) == 0) {
            handle_guest_update(fd, tag, cmd + 13);
        }
        else if (strcmp(cmd, "shutdown") == 0) {
            send_reply(fd, tag, "ok");
            agent_log("shutdown requested");
            spawn_detached("sleep 1 && systemctl poweroff");
        }
        else if (strcmp(cmd, "restart") == 0) {
            send_reply(fd, tag, "ok");
            agent_log("restart requested");
            spawn_detached("sleep 1 && systemctl reboot");
        }
        else if (strncmp(cmd, "set_ip:", 7) == 0) {
            handle_set_ip(fd, tag, cmd + 7);
        }
        else if (strcmp(cmd, "ssh_enable") == 0) {
            handle_ssh_enable(fd, tag);
        }
        else if (strncmp(cmd, "ssh_deploy_key ", 15) == 0) {
            send_reply(fd, tag, deploy_ssh_key(cmd + 15) ? "ssh_key_deployed" : "ssh_key_failed");
        }
        else if (strcmp(cmd, "idd_connect") == 0) {
            /* Mirror Windows handle_idd_connect (tools/agent/agent.c:1485):
             * kill+respawn the helper so a fresh CLDY handshake happens
             * for the new host display session. The monitor thread keeps
             * it alive after that. */
            respawn_clipboard_helper();
            send_reply(fd, tag, "ok");
        }
        else if (strncmp(cmd, "grow_root:", 10) == 0) {
            send_reply(fd, tag, grow_root_filesystem() == 0
                ? "ok" : "error:guest_grow_failed");
        }
        else if (strncmp(cmd, "gpu_query_response:", 19) == 0) {
            int n_shares = atoi(cmd + 19);
            handle_gpu_query_response(fd, n_shares);
            /* No reply — host sends this fire-and-forget */
        }
        else if (strcmp(cmd, "gpu_none") == 0) {
            /* Host reports no GPU-PV assignment — nothing to do */
        }
        else if (strcmp(cmd, "gpu_copy") == 0) {
            /* Host asks us to re-trigger the share enumeration.
             * Not implemented in v1. */
            send_reply(fd, tag, "error:not_implemented");
        }
        else {
            send_reply(fd, tag, "error:unknown");
        }
    }

out:
    agent_log("client disconnected");
    g_client_fd = -1;
    if (hb_started) {
        /* heartbeat thread checks g_client_fd and exits on its own */
        pthread_join(hb, NULL);
    }
}

/* ---- Listener setup ---- */

static int listen_vsock(void)
{
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    struct sockaddr_vm addr;

    if (s < 0) {
        agent_log("socket(AF_VSOCK) failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid    = VMADDR_CID_ANY;
    addr.svm_port   = AGENT_PORT;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        agent_log("bind(vsock:%d) failed: %s", AGENT_PORT, strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        agent_log("listen failed: %s", strerror(errno));
        close(s);
        return -1;
    }
    agent_log("listening on AF_VSOCK port %d", AGENT_PORT);
    return s;
}

/* ---- Signal handling ---- */

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
    /* Best-effort: tell host we're stopping before systemd kills us.
     * Use raw write — async-signal-safe. */
    int fd = g_client_fd;
    if (fd >= 0) {
        static const char msg[] = "service_stopping\n";
        ssize_t w = write(fd, msg, sizeof(msg) - 1);
        (void)w;
    }
}

/* ---- Main ---- */

/* ---- Run in Sandbox: files the host copies in (vsock :16) ----
 *
 * Mirror of tools/agent/file_drop.c, same wire protocol:
 *   host:  "ASBDROP1 <size>\n<file name, UTF-8>\n"
 *   guest: "ready\n" | "error:<code>\n"
 *   host:  <size> bytes
 *   guest: "saved:<guest path, UTF-8>\n" | "error:<code>\n"
 *   host:  "open\n"
 *   guest: "opened\n" | "nohandler\n" | "nosession\n" | "error:<code>\n"
 */

#define DROP_PORT           16
#define DROP_NAME_BYTES     200     /* well under NAME_MAX, whatever the encoding */
#define DROP_SPARE_BYTES    (64ull * 1024 * 1024)
#define DROP_SESSION_WAIT_S 90      /* a VM that was just started may still be signing in */
#define DROP_OPEN_WAIT_S    15

static int drop_recv_line(int fd, char *buf, size_t cap)
{
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

static int drop_send_line(int fd, const char *msg)
{
    size_t len = strlen(msg);
    char line[1200];
    if (len + 2 > sizeof(line)) return -1;
    memcpy(line, msg, len);
    line[len] = '\n';
    return write(fd, line, len + 1) == (ssize_t)(len + 1) ? 0 : -1;
}

/* The name is only ever a base name: no separator survives this, and the agent
   runs as root. Shortens on a UTF-8 boundary, keeping the extension. */
static void drop_sanitize_name(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    const char *p;
    for (p = in; *p && len + 1 < cap; p++)
        out[len++] = ((unsigned char)*p < 32 || *p == '/' || *p == '\\') ? '_' : *p;
    out[len] = '\0';
    if (!len || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        snprintf(out, cap, "file");
    if (strlen(out) > DROP_NAME_BYTES) {
        const char *dot = strrchr(out, '.');
        size_t ext = dot ? strlen(dot) : 0, keep;
        if (ext > 32) ext = 0;
        keep = DROP_NAME_BYTES - ext;
        while (keep && ((unsigned char)out[keep] & 0xC0) == 0x80) keep--;
        if (ext) memmove(out + keep, out + strlen(out) - ext, ext + 1);
        else out[keep] = '\0';
    }
}

/* Whose desktop this goes on: whoever is signed in, else the first real user. */
static struct passwd *drop_target_user(void)
{
    uid_t uid = find_graphical_session_uid();
    struct passwd *pw;
    if (uid) return getpwuid(uid);
    setpwent();
    while ((pw = getpwent())) {
        if (pw->pw_uid >= 1000 && pw->pw_uid < 65534 && uid_is_real_user(pw->pw_uid)) {
            endpwent();
            return pw;
        }
    }
    endpwent();
    return NULL;
}

/* A new file that doesn't overwrite an earlier copy: "name (2).ext". O_NOFOLLOW
   because this is root writing into a folder the user owns. */
static int drop_create_unique(const char *folder, const char *name, char *path, size_t cap)
{
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    const char *ext = dot && dot != name ? dot : "";
    int n;
    for (n = 1; n < 1000; n++) {
        int fd;
        if (n == 1) snprintf(path, cap, "%s/%s", folder, name);
        else snprintf(path, cap, "%s/%.*s (%d)%s", folder, stem, name, n, ext);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}

/* Run xdg-open as the user. A small watcher reports through a pipe, so a handler
   that stays in the foreground isn't waited for and isn't left a zombie.
   Returns the exit status, -1 if it is still running, -2 if it never ran. */
static int drop_xdg_open(const struct passwd *pw, const char *path)
{
    int pipefd[2], status = -2;
    pid_t watcher;
    struct pollfd pfd;

    if (pipe(pipefd) != 0) return -2;
    watcher = fork();
    if (watcher < 0) { close(pipefd[0]); close(pipefd[1]); return -2; }
    if (watcher == 0) {
        char env_rt[64], env_bus[96], xauth[512] = {0};
        pid_t child;
        int devnull, i;
        close(pipefd[0]);
        setsid();
        if (setgid(pw->pw_gid) < 0 || initgroups(pw->pw_name, pw->pw_gid) < 0 ||
            setuid(pw->pw_uid) < 0)
            _exit(127);
        snprintf(env_rt, sizeof(env_rt), "/run/user/%u", (unsigned)pw->pw_uid);
        snprintf(env_bus, sizeof(env_bus), "unix:path=/run/user/%u/bus", (unsigned)pw->pw_uid);
        find_mutter_xauth(pw->pw_uid, xauth, sizeof(xauth));
        if (!xauth[0]) snprintf(xauth, sizeof(xauth), "%s/.Xauthority", pw->pw_dir);
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("DISPLAY", ":0", 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("XDG_RUNTIME_DIR", env_rt, 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", env_bus, 1);
        setenv("XDG_CURRENT_DESKTOP", "ubuntu:GNOME", 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);
        setenv("XAUTHORITY", xauth, 1);
        if (chdir(pw->pw_dir) != 0) { /* xdg-open doesn't need it */ }
        child = fork();
        if (child < 0) _exit(127);
        if (child == 0) {
            devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
                if (devnull > 2) close(devnull);
            }
            close(pipefd[1]);
            /* An argv element, never a shell string: any name is safe here. */
            execlp("xdg-open", "xdg-open", path, (char *)NULL);
            _exit(127);
        }
        for (i = 0; i < DROP_OPEN_WAIT_S * 10; i++) {
            int st;
            if (waitpid(child, &st, WNOHANG) == child) {
                int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
                if (write(pipefd[1], &code, sizeof(code)) < 0) _exit(1);
                _exit(0);
            }
            usleep(100 * 1000);
        }
        {
            int still = -1;
            if (write(pipefd[1], &still, sizeof(still)) < 0) _exit(1);
        }
        _exit(0);   /* the handler carries on, reparented to init */
    }
    close(pipefd[1]);
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, (DROP_OPEN_WAIT_S + 5) * 1000) > 0 &&
        read(pipefd[0], &status, sizeof(status)) != sizeof(status))
        status = -2;
    close(pipefd[0]);
    waitpid(watcher, NULL, 0);
    return status;
}

static const char *drop_open(const char *path, const char *folder)
{
    uid_t uid = 0;
    struct passwd *pw;
    int waited, rc;

    for (waited = 0; waited < DROP_SESSION_WAIT_S && !g_stop; waited++) {
        uid = find_graphical_session_uid();
        if (uid && is_user_session_ready(uid)) break;
        uid = 0;
        sleep(1);
    }
    if (!uid) return "nosession";
    pw = getpwuid(uid);
    if (!pw) return "error:user";
    rc = drop_xdg_open(pw, path);
    if (rc == 0 || rc == -1) return "opened";
    if (rc == -2) return "error:spawn";
    /* xdg-open failing means nothing here handles this type. Show the file in its
       folder so it's at least in front of the user. */
    agent_log("file drop: xdg-open exited %d", rc);
    drop_xdg_open(pw, folder);
    return "nohandler";
}

static void drop_handle(int fd)
{
    char line[1200], name[256], folder[512], path[1024], reply[1100], *buf;
    unsigned long long size = 0, left;
    struct passwd *pw;
    struct statvfs vfs;
    uid_t uid;
    gid_t gid;
    int file, ok = 1;

    if (drop_recv_line(fd, line, sizeof(line)) < 0) return;
    if (sscanf(line, "ASBDROP1 %llu", &size) != 1) { drop_send_line(fd, "error:protocol"); return; }
    if (drop_recv_line(fd, line, sizeof(line)) < 0) return;
    drop_sanitize_name(line, name, sizeof(name));

    pw = drop_target_user();
    if (!pw) { drop_send_line(fd, "error:nouser"); return; }
    uid = pw->pw_uid;
    gid = pw->pw_gid;
    snprintf(folder, sizeof(folder), "%s/Run in Sandbox", pw->pw_dir);
    if (mkdir(folder, 0755) != 0 && errno != EEXIST) { drop_send_line(fd, "error:folder"); return; }
    if (lchown(folder, uid, gid) != 0) agent_log("file drop: chown folder: %s", strerror(errno));
    if (statvfs(folder, &vfs) == 0 &&
        (unsigned long long)vfs.f_bavail * vfs.f_frsize < size + DROP_SPARE_BYTES) {
        drop_send_line(fd, "error:nospace");
        return;
    }
    file = drop_create_unique(folder, name, path, sizeof(path));
    if (file < 0) { drop_send_line(fd, "error:create"); return; }
    if (fchown(file, uid, gid) != 0) agent_log("file drop: chown file: %s", strerror(errno));
    buf = malloc(1024 * 1024);
    if (!buf) { close(file); unlink(path); drop_send_line(fd, "error:memory"); return; }

    if (drop_send_line(fd, "ready") != 0) ok = 0;
    left = size;
    while (ok && left) {
        ssize_t got = read(fd, buf, left < 1024 * 1024 ? left : 1024 * 1024), off = 0;
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ok = 0; break; }
        while (off < got) {
            ssize_t w = write(file, buf + off, (size_t)(got - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = 0; drop_send_line(fd, "error:write"); break; }
            off += w;
        }
        left -= (unsigned long long)got;
    }
    free(buf);
    if (ok && fsync(file) != 0) ok = 0;
    close(file);
    if (!ok) {
        /* Never leave half a file where someone could mistake it for the real one. */
        unlink(path);
        agent_log("file drop: copy ended early with %llu bytes to go", left);
        return;
    }
    snprintf(reply, sizeof(reply), "saved:%s", path);
    drop_send_line(fd, reply);
    agent_log("file drop: saved %llu bytes", size);

    if (drop_recv_line(fd, line, sizeof(line)) >= 0 && strcmp(line, "open") == 0)
        drop_send_line(fd, drop_open(path, folder));
}

static void *drop_thread(void *arg)
{
    int ls = -1;
    (void)arg;
    while (!g_stop) {
        int c;
        if (ls < 0) {
            ls = vsock_bind_listen_privileged(DROP_PORT);
            if (ls < 0) { sleep(3); continue; }
            agent_log("file drop: listening on AF_VSOCK :%d", DROP_PORT);
        }
        c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno != EINTR) { close(ls); ls = -1; sleep(1); }
            continue;
        }
        drop_handle(c);
        close(c);
    }
    if (ls >= 0) close(ls);
    return NULL;
}

static void start_file_drop(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, drop_thread, NULL) == 0) pthread_detach(t);
    else agent_log("file drop: pthread_create failed: %s", strerror(errno));
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int ls;

    (void)argc; (void)argv;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Don't die from SIGPIPE if peer goes away mid-write */
    signal(SIGPIPE, SIG_IGN);

    ls = listen_vsock();
    if (ls < 0) return 1;

    /* Start the clipboard monitor thread BEFORE accepting any control
     * clients. It'll spawn the helper as soon as the user session is
     * ready, independent of host-driven idd_connect. */
    start_clipboard_monitor();

    /* Apps left over from an earlier start (see apps_thread). */
    start_app_install();

    /* Run in Sandbox: files the host copies in. */
    start_file_drop();

    while (!g_stop) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            agent_log("accept failed: %s", strerror(errno));
            break;
        }
        handle_client(c);
        close(c);
    }

    close(ls);
    agent_log("agent stopped");
    return 0;
}
