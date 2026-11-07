"""Contract: Game Streaming stays optional and keeps a domain password contained.

It's off until an admin sets it up, and only for Windows guests. Duplicates are never
saved, and they and their AD accounts go when the session ends. The password stays out
of logs and out of the registry, and the guest deletes it once the user is signed in.
The agent only opens launcher ids its own scan can produce. The dashboard sits behind
the gateway's existing checks.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    gs = read("src/backend_win/game_stream.c")
    core = read("src/backend_win/asb_core.c")
    web = read("src/backend_win/vm_web.c")
    link = read("src/backend_win/vm_agent.c")
    agent = read("tools/agent/agent.c")
    appliance = read("src/backend_win/shared_appliance.c")
    disk = read("src/backend_win/disk_util.c")
    ui = read("src/app_win/ui.c")
    headless = read("src/app_win/headless.c")
    js = read("web/app.js")
    html = read("web/index.html")
    dash = read("web/dash.js")
    proj = read("AppSandboxCore.vcxproj")

    assert "src\\backend_win\\game_stream.c" in proj and "src\\backend_win\\game_stream.h" in proj

    # Optional: its own button, off until set up, needs the domain, and can be turned off.
    assert 'onclick="openGameStreamingModal()"' in html and 'id="gs-overlay"' in html
    assert "static BOOL      g_enabled;" in gs
    assert "BOOL game_stream_has_settings(void) { return g_enabled; }" in gs
    assert "if (!appliance.domain_enabled)" in gs
    assert "game_stream_turn_off" in ui and "/v1/game-streaming/off" in headless
    assert "gameStreaming\\\":true" in headless

    # Windows only.
    assert 'L"Game Streaming needs a Windows VM."' in gs
    assert "v.osType === 'Windows'" in js

    # A duplicate is never saved; its disk is a child of the image; it's deleted when done.
    assert "g_vms[i].building_vhdx || g_vms[i].session_clone" in core
    assert "config.session_clone = TRUE" in gs and "config.base_vhdx = base" in gs
    assert "asb_session_clone_delete(id)" in gs
    assert "shared_appliance_domain_computer_remove(s->vm_name)" in gs
    assert 'strncmp(name, "GS-", 3) != 0' in agent   # nothing else in the domain goes that way
    assert 'wcsncmp(name, L"GS-", 3) != 0' in appliance
    assert "sweep_thread" in gs and "game_stream_shutdown();" in core

    # The image: the source's account stays, and no pairing or password is inherited.
    assert "NetUnjoinDomain($null,$null,$null,0)" in agent
    for leftover in ("sunshine_state.json", "credentials", "webui.txt"):
        assert leftover in agent
    assert "/generalize" in agent
    # Sysprep never stops at a dialog nobody can see, and apps it would refuse are removed first.
    assert "'/generalize','/oobe','/shutdown','/quiet','/mode:vm'" in agent
    image_ps = agent[agent.index("SESSION_IMAGE_PS[]"):agent.index("session_image_thread")]
    assert image_ps.index("Remove-AppxPackage -Package $a.PackageFullName -AllUsers") < image_ps.index("St 'sysprep'")
    assert "$prov -notcontains $_.Name" in image_ps and "if($prov.Count){" in image_ps
    # A failed refresh leaves the working image in place.
    assert "wcscpy_s(g_image, MAX_PATH, base)" in gs
    # A build VM that stops is noticed at once, not after the waits time out.
    assert "else if (started) return FALSE;" in gs
    assert gs.count('L"The copy stopped before the image was finished."') == 2
    # ...and its delete waits for a force stop that is still tearing it down.
    stop = core[core.index("ASB_API HRESULT asb_vm_stop"):core.index("/* ---- VM Delete ---- */")]
    assert stop.index("InterlockedIncrement(&inst->stopping)") < stop.index("inst->running = FALSE")
    assert "InterlockedDecrement(&inst->stopping)" in stop
    assert "while (InterlockedCompareExchange(&inst->stopping, 0, 0)" in core

    # Sunshine is never left stopped: a loaded encoder library isn't copied over, and a
    # setup that fails starts the installed service again.
    setup_ps = agent[agent.index("stream_setup_ps[]"):agent.index("stream_status_ps[]")]
    assert "(Get-FileHash $dst).Hash -eq (Get-FileHash $lib.FullName).Hash" in setup_ps
    assert "try { Copy-Item $lib.FullName $dst -Force } catch {" in setup_ps
    catch = setup_ps[setup_ps.rindex("} catch {"):]
    assert "Start-Service SunshineService -ErrorAction SilentlyContinue" in catch

    # The password: checked on the appliance, encrypted while kept, and gone after use.
    assert "ValidateCredentials" in agent
    assert "CryptProtectMemory" in gs and "GS_PASSWORD_KEEP_MS" in gs
    assert "SecureZeroMemory(start->password" in gs and "SecureZeroMemory(logon" in gs
    # In the guest: an LSA secret, never the registry value, and deleted after sign-in.
    assert "LsaStorePrivateData" in agent
    assert 'RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, L"DefaultPassword")' in agent
    assert 'L"DefaultPassword", REG_SZ' not in agent
    assert 'strcmp(cmd, "session_signed_in") == 0' in agent and "session_autologon_clear();" in agent
    assert 'L"AutoLogonCount"' in agent
    assert "SecureZeroMemory(vm->session_logon" in link
    # The command starts domain_, which both ends keep out of their logs.
    assert '"domain_autologon:%s:%s:%s"' in appliance
    assert '"domain_validate:%s:%s:%s"' in appliance
    assert 'strncmp(command, "domain_", 7) == 0' in link
    assert 'agent_log("Command: %.20s<redacted>", cmd)' in agent
    # The sign-in runs early in boot, so its link to the domain controller is saved for that
    # boot, and the host's network setup waits for it to finish.
    assert 'PrefixLength 24 | Out-Null' in agent[agent.index("SESSION_LINK_PS[]"):agent.index("session_wait_signin(char")]
    assert "IMAGE_STATE_COMPLETE" in agent
    # Nobody is there to answer setup's screens, and if its temporary session runs anyway
    # it has to end before the join restarts the guest.
    assert "<SkipMachineOOBE>true</SkipMachineOOBE>" in disk and "<SkipUserOOBE>true</SkipUserOOBE>" in disk
    assert "clear = setup_session_present() ? 0 : clear + 1" in agent
    assert "if (signed_in && session_has_shell(session))" in agent
    # The network setup doesn't remove a link that's already right.
    assert "Where-Object {$_.IPAddress -eq $env:ASB_NET_IP})){" in agent
    assert link.index('"session_wait_signin"') < link.index("Configure the independent host-only NIC")
    assert link.index('"session_link:%s"') < link.index("shared_appliance_domain_join_command(vm->share_host_ip")
    # After the join, which resets the automatic sign-in settings.
    assert link.index('strcmp(buf, "ok:joined") == 0') < link.index("vm->session_logon, buf")
    # A duplicate's unattend signs nobody in first.
    assert "if (session) {" in disk and "iso_create_session_resources" in core

    # Launching takes ids, not paths or links, and only ones in the image's list.
    launch = re.search(r"static void session_launch.*?\n}\n", agent, re.S).group(0)
    assert "session_digits(id + 6)" in launch and 'domain_field_ok(id + 5, ":-_", 400)' in launch
    assert '"error:invalid arguments"' in launch
    assert "if (!item_allowed(item))" in gs

    # The dashboard: after the Host and Origin checks, signed in, limited, HTTPS on the network.
    route = web.index('strcmp(path, "/") == 0')
    assert web.index("host_allowed(host, c->tls)") < route
    assert web.index("origin_allowed(headers, host, c->tls)") < route
    assert web.count("g_dashboard && (!c->tls || g_dashboard_lan)") >= 3
    assert "login_allowed(ip)" in web[web.index("static void dashboard_login"):web.index("static void dashboard_serve_catalog_file")]
    assert 'asb_app=%s; Path=/; HttpOnly; SameSite=Strict%s' in web
    assert "game_stream_signed_in(token, c->tls" in web
    assert "return g_dashboard && g_dashboard_lan;" in web
    assert "SameSite=None" not in web
    # A cookie only opens this user's VM, and art and the list only come from the catalog folder.
    assert "game_stream_session_vm(user, name, sizeof(name))" in web
    assert 'strstr(leaf, "..")' in gs
    # The page keeps nothing secret.
    assert not re.search(r"(localStorage|sessionStorage)\.\w+\([^)]*(password|pass|token)", dash, re.I)

    # One Moonlight identity per user: kept for admins only, restored before streaming is
    # set up, saved after a pairing, per-user, and gone with the user or the feature.
    assert 'L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"' in gs
    assert "identity_commands(s->user)" in gs and "config.stream_port = user_stream_port(s->user)" in gs
    assert 'identity_path(user, name, temp, MAX_PATH)' in gs and "identity_save(vm, user)" in gs
    assert link.index("vm->session_identity) {") < link.index("vm_stream_setup_command(vm, stream_cmd")
    assert "if (SUCCEEDED(hr)) game_stream_forget_user(name);" in core
    assert "asb_session_clone_remove_leftover(NULL, root);" in gs     # Turn Off takes identities too
    # A user's port is tested before use, and sits below Windows' dynamic range.
    assert "#define GS_PORT_FIRST        40000" in gs
    assert "port && !vm_stream_port_free(port)" in gs and "vm_stream_port_free(candidate)" in gs
    # A key never reaches a log: the commands are redacted, quiet, and only name three files.
    assert 'strncmp(command, "domain_identity_", 16) == 0' in link
    assert 'strncmp(cmd, "domain_identity_", 16) == 0' in agent
    assert re.search(r'identity_file\(const char \*leaf.*?"state".*?"cert".*?"key"', agent, re.S)
    assert "SecureZeroMemory(vm->session_identity" in link

    # Duplicates don't become the folder suggested for the user's next VM, and a setting
    # that older versions saved that way is ignored.
    assert "if (!inst || inst->session_clone || !inst->storage_root[0]) return;" in core
    assert r'if (!wcsstr(line + 18, L"\\GameStreaming\\sessions"))' in core

    # Sessions get the network and drives chosen for Game Streaming, not the nominated
    # VM's: a VPN routes their traffic, and the relay reaches them on the host's
    # address on the VPN network. A VPN deleted since falls back to NAT.
    stream = read("src/backend_win/vm_stream.c")
    assert "config.network_mode = vpn[0] ? ASB_NET_VPN : ASB_NET_NAT;" in gs
    assert "config.shared_resource_exclusions = drives_off;" in gs
    assert "if (vpn[0] && !vpn_configs_find(vpn, NULL))" in gs
    assert 'wcsspn(drives_off, L"0123456789abcdefABCDEF-{},")' in gs
    assert 'L"GameStreamVpn=%s' in gs and 'L"GameStreamDrivesOff=%s' in gs
    assert "vm->network_mode == NET_VPN ? vm->vpn_ip" in stream
    assert 'sprintf_s(allowed, sizeof(allowed), "%s.1", hcn_vpn_subnet_base());' in stream
    assert 'json_get_string(json, L"drivesOff"' in ui and 'json_get_string(body, L"drivesOff"' in headless
    assert 'id="gs-vpn"' in html and 'id="gs-drives"' in html and "drivesOff:" in js
    # From an image, the VPN choice reaches the instance before its network is set up,
    # and each duplicate gets this version's agent rather than the image's.
    sync = core[core.index("/* ---- Synchronous path (from-template"):]
    sync = sync[:sync.index("/* Create HCS VM */")]
    assert sync.index("wcscpy_s(inst->vpn_id, _countof(inst->vpn_id), config->vpn_id") < sync.index("vpn_gateway_prepare_client(inst")
    assert sync.index("upgrade_windows_agent_offline(cfg.vhdx_path)") < sync.index("Resources ISO")
    # The disc that set a duplicate up leaves This PC once its user is signed in.
    signed_in = agent[agent.index('strcmp(cmd, "session_signed_in") == 0'):]
    assert "session_hide_setup_disc();" in signed_in[:300]
    assert '_wcsicmp(label, L"APPSETUP") != 0' in agent and "DeleteVolumeMountPointW(root)" in agent

    # Moonlight is refused (401) when Sunshine finds its certificate paired more than once:
    # setup keeps the latest pairing of each device before Sunshine starts.
    setup = agent[agent.index("static const char stream_setup_ps[]"):]
    setup = setup[:setup.index("static DWORD WINAPI stream_setup_thread")]
    assert setup.startswith("static const char stream_setup_ps[] =\n    PAIRING_REPAIR_PS") or \
        setup.startswith("static const char stream_setup_ps[] =\r\n    PAIRING_REPAIR_PS")
    assert setup.index("Repair-Pairings (Join-Path $root") < setup.index(r" Start-Service SunshineService\r\n")
    # Windows hands Sunshine the SDR desktop as sRGB-encoded FP16: without this Sunshine
    # applies gamma again and streams look washed out. Needs Sunshine 2026.1103 or newer.
    assert r"'fp16_sdr_gamma_encoded = enabled'\r\n" in setup
    ver = re.search(r'#define STREAM_SUNSHINE_VER\s+"(\d+)\.(\d+)\.(\d+)"', agent)
    assert ver and (int(ver.group(1)), int(ver.group(2))) >= (2026, 1103), "the option is ignored by older Sunshine"
    repair = agent[agent.index("#define PAIRING_REPAIR_PS"):agent.index("static const char pairing_watch_ps[]")]
    assert "$seen.ContainsKey($c)" in repair and "$s.root.named_devices=$keep" in repair
    assert "Stop-Service SunshineService -Force" in repair and "Start-Service SunshineService" in repair
    # Pairing again during a session adds a duplicate at once: a watcher repairs it then.
    watch = agent[agent.index("static DWORD WINAPI pairing_watch_thread"):]
    watch = watch[:watch.index("\n}") + 2]
    assert "GetFileAttributesExW(state" in watch and "write_agent_script(script, pairing_watch_ps)" in watch
    assert "' $true\\r\\n\";" in agent[agent.index("static const char pairing_watch_ps[]"):][:400]
    assert "CreateThread(NULL, 0, pairing_watch_thread" in agent
    # Streams aren't washed out: the AppSandbox display is kept out of wide colour (FP16).
    displays = read("tools/agent/appsandbox-displays.c")
    assert "DISPLAYCONFIG_DEVICE_INFO_SET_WCG_STATE" in displays and "DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR" in displays
    assert "colour = sdr_only(&paths[ours]);" in displays
    prep = displays[displays.index("static int stream_prep(void)"):]
    assert "sdr_ours();" in prep[:prep.index("int main(")]
    assert 'strncmp(output, "applied: colour", 15) == 0' in agent

    # Moonlight checks its certificate in the last step of pairing, so a device pairing
    # again must not be listed twice even for a moment: entries are set aside first.
    pin_ps = agent[agent.index("static const char stream_pin_ps[]"):agent.index("static const char stream_unpair_ps[]")]
    assert pin_ps.index("WebUi '/api/clients/unpair'") < pin_ps.index("WebUi '/api/pin' @{pairing_id")
    assert pin_ps.index("WebUi '/api/pin' @{pairing_id") < pin_ps.index(" Restore-Pairings\\r\\n")
    assert "-not $certs.ContainsKey([string]$_.cert)" in pin_ps and "try { Restore-Pairings } catch {}" in pin_ps
    assert "stream_run(vars, stream_pin_ps, 60000" in agent
    assert "vm_agent_send(vm, cmd, reply, sizeof(reply), 70000)" in read("src/backend_win/vm_stream.c")

    # A duplicate shut down from inside is kept 30 minutes and can be started again from
    # the dashboard; it's deleted after that, or on End session, as before.
    gs = read("src/backend_win/game_stream.c")
    assert "#define GS_KEEP_STOPPED_MIN  30" in gs
    thread_at = gs.index("static DWORD WINAPI session_thread")
    thread = gs[thread_at:gs.index("static int find_session(const wchar_t *user)\n{", thread_at) if "static int find_session(const wchar_t *user)\n{" in gs else gs.index("static int find_session(const wchar_t *user)\r\n{", thread_at)]
    assert "if (!vm->running) goto stopped;" in thread
    stopped = thread[thread.index("\nstopped:"):thread.index("\nwait_to_clean:")]
    assert 'session_state(slot, "stopped"' in stopped and "goto cleanup;" in stopped
    assert "asb_vm_start_ex(asb_vm_find(s->vm_name)" in stopped and "goto watch;" in stopped
    assert "seen_user = FALSE;" in stopped and "(anyone || !seen_user) ? 0 : signed_out + 1" in thread
    assert 'strcmp(g_sessions[slot].state, "stopped") == 0' in gs
    web = read("src/backend_win/vm_web.c")
    assert 'strcmp(path, "/api/app/session/resume") == 0' in web
    assert web.index("/api/app/session/resume") > web.index("/api/app/session\"") if "/api/app/session\"" in web else True
    assert 'id="btn-resume"' in read("web/dash.html") and "api('/api/app/session/resume'" in dash

    # Each user's VM is kept between sessions: shut down, taken out of the list with its
    # folder left, and booted again next time without setup, identity restore or a join.
    assert "static int       g_keep_days = 14;" in gs and 'L"GameStreamKeepDays="' in gs
    assert "keep = id && !InterlockedCompareExchange(&s->discard, 0, 0) &&" in thread
    assert "(pinned || (keep_days > 0 && (reuse || ready_once)))" in thread
    end = thread[thread.index("\ncleanup:"):]
    assert end.index("asb_vm_shutdown(handle);") < end.index("asb_session_clone_release(id)")
    assert "kept_forget(s->user);" in end and "shared_appliance_domain_computer_remove(s->vm_name)" in end
    assert "config.session_reuse = reuse;" in thread and "identity = reuse ? NULL : identity_commands(s->user);" in thread
    assert 'if (reuse && !pinned) kept_remove(s->user, s->vm_name, L"it couldn\'t start");' in thread
    unusable = gs[gs.index("static const wchar_t *kept_unusable"):gs.index("static BOOL session_active(const wchar_t *user);")]
    assert "the games image was refreshed" in unusable and "unused for too long" in unusable
    assert "kept_sweep(NULL, NULL);   /* VMs from the old image start over */" in gs
    sweep = gs[gs.index("static DWORD WINAPI sweep_thread"):gs.index("void game_stream_init(void)")]
    assert "kept_sweep(kept_note, kept);" in sweep and "if (is_kept) continue;" in sweep and "if (!kept) continue;" in sweep
    forget = gs[gs.index("void game_stream_forget_user"):]
    assert "InterlockedExchange(&g_sessions[slot].discard, 1);" in forget[:900] and "kept_remove(user, name" in forget[:900]
    core = read("src/backend_win/asb_core.c")
    assert "if (!keep_files) remove_dir_recursive(dir);" in core
    assert "if (from_template && config->session_clone && config->session_reuse) {" in core
    assert "cfg.admin_pass[0] != L'\\0' && !config->session_reuse" in core
    agent_link = read("src/backend_win/vm_agent.c")
    assert "vm->session_logon[0] && !vm->session_reuse" in agent_link
    reuse_join = agent_link[agent_link.index('strcmp(buf, "ok:already") == 0'):][:1400]
    assert '"restart"' in reuse_join and "vm->session_signing_in = TRUE;" in reuse_join
    assert "SecureZeroMemory(vm->session_logon" in reuse_join
    assert 'id="gs-keep"' in html and "keepDays:" in js and 'id="session-keep"' in read("web/dash.html")
    assert 'json_get_int(body, L"keepDays", &keep_days)' in headless and 'json_get_int(json, L"keepDays", &keep_days)' in ui

    # Time zone: a generalized duplicate would start on Windows' default (Pacific). The
    # answer file carries this PC's zone, and each session sends it for VMs kept from
    # before; the domain's own zone wins.
    disk = read("src/backend_win/disk_util.c")
    assert "static void host_time_zone_xml" in disk and "<TimeZone>%s</TimeZone>" in disk
    assert "host_time_zone_xml(time_zone, _countof(time_zone));" in disk
    assert "time_zone, admin_user, b64_password);" in disk
    assert '"session_time_zone:%S", tz.TimeZoneKeyName' in gs
    assert gs.index('"session_signed_in"') < gs.index('"session_time_zone:%S"')
    tzcmd = agent[agent.index('strncmp(cmd, "session_time_zone:", 18) == 0'):][:900]
    assert "policy_time_zone(policy, _countof(policy))" in tzcmd and '"ok:domain"' in tzcmd
    assert 'strchr(" ().+-", *p)' in tzcmd and 'time_zone_set(want, "the host")' in tzcmd
    assert 'time_zone_set(want, "domain policy");' in agent

    # Callers for one VM take turns on its command slot.
    assert "g_conn_send_locks" in link
    print("Game Streaming contract verified.")


if __name__ == "__main__":
    main()
