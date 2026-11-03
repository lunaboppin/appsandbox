#include "iso_custom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Growable byte buffer ---- */

typedef struct {
    char  *p;
    size_t len, cap;
} SBuf;

static BOOL sb_add(SBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        char *np;
        while (b->len + n + 1 > cap) cap *= 2;
        np = (char *)realloc(b->p, cap);
        if (!np) return FALSE;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return TRUE;
}

static BOOL sb_adds(SBuf *b, const char *s) { return sb_add(b, s, strlen(s)); }

static void sb_free(SBuf *b)
{
    if (b->p) SecureZeroMemory(b->p, b->cap);   /* ours holds the encoded password */
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* Read an XML file as UTF-8 without a BOM. UTF-16 LE files (Windows SIM saves
   those) are converted. */
static char *read_xml_utf8(const wchar_t *path, size_t *out_len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    BYTE *raw;
    DWORD got = 0;
    char *out = NULL;

    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 4 * 1024 * 1024) { CloseHandle(h); return NULL; }
    raw = (BYTE *)malloc((size_t)size.QuadPart + 2);
    if (!raw) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, raw, (DWORD)size.QuadPart, &got, NULL)) got = 0;
    CloseHandle(h);
    raw[got] = raw[got + 1] = 0;

    if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        int wlen = (int)((got - 2) / 2);
        int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(raw + 2), wlen, NULL, 0, NULL, NULL);
        out = (char *)malloc((size_t)n + 1);
        if (out) {
            WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(raw + 2), wlen, out, n, NULL, NULL);
            out[n] = '\0';
            *out_len = (size_t)n;
        }
    } else if (got >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        out = NULL;   /* UTF-16 BE: Windows Setup doesn't take it either */
    } else {
        size_t skip = (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        out = (char *)malloc(got - skip + 1);
        if (out) {
            memcpy(out, raw + skip, got - skip);
            out[got - skip] = '\0';
            *out_len = got - skip;
        }
    }
    free(raw);
    return out;
}

/* ---- Minimal XML scanning: enough for unattend files, which are plain
   elements, attributes, text and comments. ---- */

static const char *memfind(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    for (; p + n <= end; p++)
        if (*p == needle[0] && memcmp(p, needle, n) == 0) return p;
    return NULL;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Next '<' that opens a start or end tag, skipping text, comments, CDATA,
   processing instructions and declarations. */
static const char *xml_skip_misc(const char *p, const char *end)
{
    while (p && p < end) {
        const char *c;
        if (*p != '<') { p++; continue; }
        if (end - p >= 4 && memcmp(p, "<!--", 4) == 0)            c = memfind(p + 4, end, "-->");
        else if (end - p >= 9 && memcmp(p, "<![CDATA[", 9) == 0)  c = memfind(p + 9, end, "]]>");
        else if (end - p >= 2 && p[1] == '?')                     c = memfind(p + 2, end, "?>");
        else if (end - p >= 2 && p[1] == '!')                     c = memfind(p + 2, end, ">");
        else return p;
        if (!c) return NULL;
        p = memfind(c, end, ">") + 1;
    }
    return NULL;
}

/* The '>' that closes the tag starting at p (quotes respected). */
static const char *xml_tag_close(const char *p, const char *end)
{
    char q = 0;
    for (; p < end; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

/* Just past the end of the element whose start tag is at p. */
static const char *xml_element_end(const char *p, const char *end)
{
    int depth = 0;
    while (p && p < end) {
        const char *gt;
        p = xml_skip_misc(p, end);
        if (!p) return NULL;
        gt = xml_tag_close(p, end);
        if (!gt) return NULL;
        if (p[1] == '/') depth--;
        else if (gt[-1] != '/') depth++;
        p = gt + 1;
        if (depth <= 0) return p;
    }
    return NULL;
}

/* An element: its start tag, content and end, as positions in the document. */
typedef struct {
    const char *start;      /* '<' of the start tag */
    const char *body;       /* just past the start tag */
    const char *body_end;   /* '<' of the end tag (== body for <x/>) */
    const char *end;        /* just past the end tag */
} XmlEl;

static BOOL xml_element(const char *p, const char *end, XmlEl *el)
{
    const char *gt = xml_tag_close(p, end);
    if (!gt) return FALSE;
    el->start = p;
    el->body = gt + 1;
    if (gt[-1] == '/') {
        el->end = el->body_end = gt + 1;
        return TRUE;
    }
    el->end = xml_element_end(p, end);
    if (!el->end) return FALSE;
    for (el->body_end = el->end - 1; el->body_end > el->body && *el->body_end != '<'; el->body_end--)
        ;
    return TRUE;
}

/* First child element of [p, end), or FALSE at the end of the parent. */
static BOOL xml_child(const char *p, const char *end, XmlEl *el)
{
    p = xml_skip_misc(p, end);
    if (!p || p[1] == '/') return FALSE;
    return xml_element(p, end, el);
}

/* Does the element's tag name equal name? */
static BOOL xml_is(const XmlEl *el, const char *name)
{
    size_t n = strlen(name);
    const char *p = el->start + 1;
    return strncmp(p, name, n) == 0 &&
           (is_space(p[n]) || p[n] == '>' || p[n] == '/');
}

/* Copy attribute `name` of the element's start tag into out. */
static BOOL xml_attr(const XmlEl *el, const char *name, char *out, size_t cap)
{
    const char *gt = el->body - 1, *p = el->start + 1;
    size_t nl = strlen(name);

    while (p < gt && !is_space(*p) && *p != '>' && *p != '/') p++;
    while (p < gt) {
        const char *an, *v;
        size_t al;
        char q;
        while (p < gt && is_space(*p)) p++;
        an = p;
        while (p < gt && *p != '=' && !is_space(*p) && *p != '/' && *p != '>') p++;
        al = (size_t)(p - an);
        if (al == 0) { p++; continue; }
        while (p < gt && is_space(*p)) p++;
        if (p >= gt || *p != '=') continue;
        p++;
        while (p < gt && is_space(*p)) p++;
        if (p >= gt || (*p != '"' && *p != '\'')) return FALSE;
        q = *p++;
        v = p;
        while (p < gt && *p != q) p++;
        if (al == nl && memcmp(an, name, nl) == 0) {
            size_t vl = (size_t)(p - v);
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return TRUE;
        }
        p++;
    }
    return FALSE;
}

/* <settings pass="..."> directly under the document's root element. */
static BOOL find_settings(const char *doc, const char *end, const char *pass, XmlEl *out)
{
    XmlEl root, s;
    const char *p;
    if (!xml_child(doc, end, &root)) return FALSE;
    for (p = root.body; xml_child(p, root.body_end, &s); p = s.end) {
        char v[64];
        if (xml_is(&s, "settings") && xml_attr(&s, "pass", v, sizeof v) && strcmp(v, pass) == 0) {
            *out = s;
            return TRUE;
        }
    }
    return FALSE;
}

/* <component name="..."> (or any child element named tag) inside parent. */
static BOOL find_child(const XmlEl *parent, const char *tag, const char *comp_name, XmlEl *out)
{
    XmlEl c;
    const char *p;
    for (p = parent->body; xml_child(p, parent->body_end, &c); p = c.end) {
        char v[128];
        if (!xml_is(&c, tag)) continue;
        if (comp_name && !(xml_attr(&c, "name", v, sizeof v) && strcmp(v, comp_name) == 0)) continue;
        *out = c;
        return TRUE;
    }
    return FALSE;
}

static int count_children(const XmlEl *parent)
{
    XmlEl c;
    const char *p;
    int n = 0;
    for (p = parent->body; xml_child(p, parent->body_end, &c); p = c.end) n++;
    return n;
}

/* Append a command element with its <Order> replaced by order. */
static BOOL add_renumbered(SBuf *b, const XmlEl *cmd, int order)
{
    char num[32];
    const char *o = memfind(cmd->body, cmd->body_end, "<Order>");
    const char *c = o ? memfind(o, cmd->body_end, "</Order>") : NULL;
    sprintf_s(num, sizeof num, "<Order>%d</Order>", order);
    if (!sb_adds(b, "\n                ")) return FALSE;
    if (o && c)
        return sb_add(b, cmd->start, (size_t)(o - cmd->start)) && sb_adds(b, num) &&
               sb_add(b, c + 8, (size_t)(cmd->end - (c + 8)));
    return sb_add(b, cmd->start, (size_t)(cmd->body - cmd->start)) && sb_adds(b, num) &&
           sb_add(b, cmd->body, (size_t)(cmd->end - cmd->body));
}

static BOOL add_element(SBuf *b, const XmlEl *el, const char *indent)
{
    return sb_adds(b, indent) && sb_add(b, el->start, (size_t)(el->end - el->start));
}

/* One insertion (or replacement) into our answer file. */
typedef struct {
    const char *at;
    size_t      del;
    SBuf        text;
} Edit;

static int edit_cmp(const void *a, const void *b)
{
    const Edit *x = (const Edit *)a, *y = (const Edit *)b;
    return x->at < y->at ? -1 : x->at > y->at;
}

/* Our own settings for these Shell-Setup elements stay; the ISO's are dropped. */
static BOOL shell_setup_ours(const XmlEl *el, BOOL oobe)
{
    if (xml_is(el, "ComputerName")) return TRUE;
    return oobe && (xml_is(el, "UserAccounts") || xml_is(el, "AutoLogon") ||
                    xml_is(el, "OOBE") || xml_is(el, "FirstLogonCommands"));
}

#define ENABLE_NET_COMMAND \
    "\n                <SynchronousCommand wcm:action=\"add\">\n" \
    "                    <Order>%d</Order>\n" \
    "                    <Description>App Sandbox: turn back on network adapters the ISO's setup turned off</Description>\n" \
    "                    <CommandLine>powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Get-NetAdapter -IncludeHidden | Where-Object { $_.AdminStatus -eq 'Down' -or $_.Status -eq 'Disabled' } | Enable-NetAdapter -IncludeHidden -Confirm:$false\"</CommandLine>\n" \
    "                    <RequiresUserInput>false</RequiresUserInput>\n" \
    "                </SynchronousCommand>"

BOOL iso_custom_merge_unattend(const wchar_t *ours_path, const wchar_t *theirs_path,
                               wchar_t *summary, size_t summary_cap)
{
    size_t ours_len = 0, theirs_len = 0;
    char *ours = NULL, *theirs = NULL;
    const char *oe, *te;
    XmlEl o_spec, o_spec_shell, o_spec_deploy, o_spec_run;
    XmlEl o_oobe, o_oobe_intl, o_oobe_shell, o_flc;
    XmlEl root, s;
    Edit edits[8];
    int nedits = 0, i;
    int spec_order, flc_order, spec_cmds = 0, flc_cmds = 0, kept_comps = 0;
    char dropped[512] = "";
    BOOL ok = FALSE, has_intl = FALSE;
    const char *p;
    FILE *f = NULL;

    ZeroMemory(edits, sizeof edits);
    if (summary && summary_cap) summary[0] = L'\0';
    ours = read_xml_utf8(ours_path, &ours_len);
    theirs = read_xml_utf8(theirs_path, &theirs_len);
    if (!ours || !theirs) goto done;
    oe = ours + ours_len;
    te = theirs + theirs_len;

    /* Where things go in ours (generated by asb_provision_unattend). */
    if (!find_settings(ours, oe, "specialize", &o_spec) ||
        !find_child(&o_spec, "component", "Microsoft-Windows-Shell-Setup", &o_spec_shell) ||
        !find_child(&o_spec, "component", "Microsoft-Windows-Deployment", &o_spec_deploy) ||
        !find_child(&o_spec_deploy, "RunSynchronous", NULL, &o_spec_run) ||
        !find_settings(ours, oe, "oobeSystem", &o_oobe) ||
        !find_child(&o_oobe, "component", "Microsoft-Windows-Shell-Setup", &o_oobe_shell) ||
        !find_child(&o_oobe_shell, "FirstLogonCommands", NULL, &o_flc))
        goto done;
    has_intl = find_child(&o_oobe, "component", "Microsoft-Windows-International-Core", &o_oobe_intl);
    spec_order = count_children(&o_spec_run) + 1;
    flc_order = count_children(&o_flc) + 1;

    /* 0 spec Shell-Setup, 1 spec RunSynchronous, 2 spec Deployment, 3 spec components,
       4 oobe International-Core, 5 oobe Shell-Setup, 6 FirstLogonCommands, 7 oobe components */
    edits[0].at = o_spec_shell.body_end;
    edits[1].at = o_spec_run.body_end;
    edits[2].at = o_spec_run.end;
    edits[3].at = o_spec.body_end;
    edits[4].at = has_intl ? o_oobe_intl.start : o_oobe.body;
    edits[5].at = o_flc.start;
    edits[6].at = o_flc.body_end;
    edits[7].at = o_oobe.body_end;
    nedits = 8;

    /* Our own first-logon step for ISOs: some take the network away for OOBE and
       count on their own account screen to bring it back, which never runs here. */
    {
        char cmd[1024];
        sprintf_s(cmd, sizeof cmd, ENABLE_NET_COMMAND, flc_order++);
        if (!sb_adds(&edits[6].text, cmd)) goto done;
    }

    if (!xml_child(theirs, te, &root) || !xml_is(&root, "unattend")) goto done;
    for (p = root.body; xml_child(p, root.body_end, &s); p = s.end) {
        char pass[64] = "";
        BOOL oobe;
        XmlEl c;
        const char *q;

        if (!xml_is(&s, "settings")) continue;
        xml_attr(&s, "pass", pass, sizeof pass);
        oobe = strcmp(pass, "oobeSystem") == 0;
        if (!oobe && strcmp(pass, "specialize") != 0) {
            /* windowsPE belongs to Setup, which doesn't run; the audit and generalize
               passes would change how the install proceeds. */
            if (strlen(dropped) + strlen(pass) + 3 < sizeof dropped) {
                if (dropped[0]) strcat_s(dropped, sizeof dropped, ", ");
                strcat_s(dropped, sizeof dropped, pass);
            }
            continue;
        }

        for (q = s.body; xml_child(q, s.body_end, &c); q = c.end) {
            char name[128] = "";
            XmlEl k;
            const char *r;

            if (!xml_is(&c, "component")) continue;
            xml_attr(&c, "name", name, sizeof name);

            if (strcmp(name, "Microsoft-Windows-Shell-Setup") == 0) {
                for (r = c.body; xml_child(r, c.body_end, &k); r = k.end) {
                    if (oobe && xml_is(&k, "FirstLogonCommands")) {
                        XmlEl cmd;
                        const char *t;
                        for (t = k.body; xml_child(t, k.body_end, &cmd); t = cmd.end) {
                            if (!add_renumbered(&edits[6].text, &cmd, flc_order++)) goto done;
                            flc_cmds++;
                        }
                    } else if (!shell_setup_ours(&k, oobe)) {
                        if (!add_element(&edits[oobe ? 5 : 0].text, &k, oobe ? "" : "\n            "))
                            goto done;
                        if (oobe && !sb_adds(&edits[5].text, "\n            ")) goto done;
                    }
                }
            } else if (!oobe && strcmp(name, "Microsoft-Windows-Deployment") == 0) {
                for (r = c.body; xml_child(r, c.body_end, &k); r = k.end) {
                    if (xml_is(&k, "RunSynchronous")) {
                        XmlEl cmd;
                        const char *t;
                        for (t = k.body; xml_child(t, k.body_end, &cmd); t = cmd.end) {
                            if (!add_renumbered(&edits[1].text, &cmd, spec_order++)) goto done;
                            spec_cmds++;
                        }
                    } else if (!add_element(&edits[2].text, &k, "\n            ")) {
                        goto done;
                    }
                }
            } else if (oobe && strcmp(name, "Microsoft-Windows-Deployment") == 0) {
                /* Reseal and friends would send the install somewhere else. */
                if (strlen(dropped) + 40 < sizeof dropped) {
                    if (dropped[0]) strcat_s(dropped, sizeof dropped, ", ");
                    strcat_s(dropped, sizeof dropped, "oobeSystem Deployment");
                }
            } else if (oobe && strcmp(name, "Microsoft-Windows-International-Core") == 0) {
                /* The ISO's locale and keyboard replace the ones we guessed from the image. */
                if (!add_element(&edits[4].text, &c, has_intl ? "" : "\n        ")) goto done;
                if (has_intl) edits[4].del = (size_t)(o_oobe_intl.end - o_oobe_intl.start);
                kept_comps++;
            } else {
                if (!add_element(&edits[oobe ? 7 : 3].text, &c, "\n        ")) goto done;
                kept_comps++;
            }
        }
    }

    /* Write the merged file, keeping our UTF-8 BOM. */
    qsort(edits, (size_t)nedits, sizeof edits[0], edit_cmp);
    if (_wfopen_s(&f, ours_path, L"wb") != 0 || !f) goto done;
    fputs("\xEF\xBB\xBF", f);
    p = ours;
    for (i = 0; i < nedits; i++) {
        fwrite(p, 1, (size_t)(edits[i].at - p), f);
        if (edits[i].text.len) fwrite(edits[i].text.p, 1, edits[i].text.len, f);
        p = edits[i].at + edits[i].del;
    }
    fwrite(p, 1, (size_t)(oe - p), f);
    ok = fclose(f) == 0;

    if (summary && summary_cap)
        swprintf_s(summary, summary_cap,
                   L"%d specialize command(s), %d first-logon command(s) and %d component(s) kept; "
                   L"the ISO's accounts, autologon and OOBE settings replaced by App Sandbox's%s%S",
                   spec_cmds, flc_cmds, kept_comps, dropped[0] ? L"; ignored: " : L"", dropped);

done:
    for (i = 0; i < 8; i++) sb_free(&edits[i].text);
    if (ours) { SecureZeroMemory(ours, ours_len); free(ours); }
    free(theirs);
    return ok;
}

/* ---- $OEM$ files ---- */

static int add_oem_tree(FILE *mf, const wchar_t *host_dir, const wchar_t *guest_dir, BOOL windows_root)
{
    wchar_t pattern[MAX_PATH], host[MAX_PATH], guest[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", host_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(host, MAX_PATH, L"%s\\%s", host_dir, fd.cFileName);
        swprintf_s(guest, MAX_PATH, L"%s\\%s", guest_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* The answer file in Panther is ours. */
            if (windows_root && _wcsicmp(guest, L"\\Windows\\Panther") == 0) continue;
            count += add_oem_tree(mf, host, guest, windows_root);
        } else {
            if (windows_root && _wcsicmp(guest, L"\\Windows\\Setup\\Scripts\\SetupComplete.cmd") == 0)
                wcscpy_s(guest, MAX_PATH, L"\\Windows\\Setup\\Scripts\\SetupComplete.iso.cmd");
            fwprintf(mf, L"%s\t%s\n", host, guest);
            count++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

int iso_custom_append_manifest(const wchar_t *manifest_path, const wchar_t *custom_dir,
                               const wchar_t *iso_path)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH];
    FILE *mf = NULL, *mk = NULL;
    int count = 0;

    /* Marker: tells SetupComplete.cmd this install came with its own setup. */
    swprintf_s(marker, MAX_PATH, L"%s\\custom-iso.txt", custom_dir);
    if (_wfopen_s(&mk, marker, L"w, ccs=UTF-8") != 0 || !mk) return -1;
    fwprintf(mk, L"Installed from %s, with its own setup customisation.\n", iso_path);
    fclose(mk);

    if (_wfopen_s(&mf, manifest_path, L"a, ccs=UTF-8") != 0 || !mf) return -1;
    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\custom-iso.txt\n", marker);
    count++;

    swprintf_s(dir, MAX_PATH, L"%s\\" ISO_CUSTOM_OEM L"\\$$", custom_dir);
    count += add_oem_tree(mf, dir, L"\\Windows", TRUE);
    swprintf_s(dir, MAX_PATH, L"%s\\" ISO_CUSTOM_OEM L"\\$1", custom_dir);
    count += add_oem_tree(mf, dir, L"", FALSE);
    fclose(mf);
    return count;
}
