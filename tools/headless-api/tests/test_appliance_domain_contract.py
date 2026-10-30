"""Contract: the Windows Server Core appliance can be the AppSandbox domain controller.

Optional (Shared Appliance > Domain, Server Core only; off works as before).
Setup or the Domain dialog promotes the appliance (AD DS + DNS), restarts it,
turns off every password rule and makes the appliance admin a Domain Admin.
The Domain dialog manages users and groups through agent commands whose
arguments travel base64 in environment variables. A VM created as Managed
gets the appliance link even without shares, joins the domain on every boot
(DNS + NRPT to the DC), restarts after the first join, and its domain users
administer it. SMB logins become domain-qualified on a DC.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    hdr = read("src/backend_win/shared_appliance.h")
    app = read("src/backend_win/shared_appliance.c")
    core = read("src/backend_win/asb_core.c")
    link = read("src/backend_win/vm_agent.c")
    agent = read("tools/agent/agent.c")
    ui = read("src/app_win/ui.c")
    js = read("web/app.js")
    html = read("web/index.html")

    # Optional, Server Core only, persisted; a rebuilt OS starts without it.
    assert "BOOL enable_domain;" in hdr and "BOOL domain_enabled;" in hdr
    assert 'fwprintf(file, L"DomainEnabled=%d\\n"' in app
    assert "args->config.enable_domain && args->config.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE" in app
    assert "g_appliance.status.domain_enabled = FALSE;" in app
    assert "enableDomain:document.getElementById('appliance-backend').value==='server_core'" in js
    assert 'id="appliance-domain"' in html

    # Promotion: AD DS forest, restart, then the policy and admin account.
    promote = app[app.index("static HRESULT domain_promote_internal(void)\n{"):]
    promote = promote[:promote.index("\n}\n")]
    assert promote.index('"domain_promote:%s:%s:%s"') < promote.index("shared_appliance_stop(FALSE);") \
        < promote.index('"domain_finish:%s:%s"')
    assert "Install-ADDSForest -DomainName $env:ASB_DOM_FQDN" in agent
    # The local policy is relaxed before the Administrator/DSRM passwords are set.
    pr = agent[agent.index("static const char DOMAIN_PROMOTE_PS[]"):]
    assert pr.index("PasswordComplexity = 0") < pr.index("$a.SetPassword($pw)") < pr.index("Install-ADDSForest")
    assert "-ComplexityEnabled $false -MinPasswordLength 0 -PasswordHistoryCount 0" in agent
    assert "Add-ADGroupMember -Identity 'Domain Admins' -Members $n" in agent

    # Management: commands, protected accounts, secrets kept off logs.
    for c in ('"domain_list"', '"domain_user_set:%s:%d:%s:%d:%s"', '"domain_user_delete:%s"',
              '"domain_group_add:%s"', '"domain_group_delete:%s"'):
        assert c in app, c
    assert "static BOOL domain_protected_user(" in app
    assert 'strncmp(command, "domain_", 7) == 0' in link
    assert 'agent_log("Command: %.20s<redacted>", cmd);' in agent
    assert "SetEnvironmentVariableA(\"ASB_DOM_PW\"" in agent
    # Replies of kilobytes survive both ends.
    assert "char           rsp[ASB_DOMAIN_REPLY_MAX];" in link
    assert "size_t size = strlen(tag) + strlen(msg) + 1;" in agent

    # Managed VMs: create option, appliance link, join every boot.
    assert "if (config->domain_member) {" in core and 'L"asbadmin"' in core
    assert 'fwprintf(f, L"DomainMember=1\\n");' in core
    assert "config->domain_member && g_appliance.status.domain_enabled" in app
    assert "vm->domain_member) &&" in link
    assert "shared_appliance_domain_join_command(vm->share_host_ip," in link
    assert "Add-DnsClientNrptRule -Namespace ('.'+$env:ASB_DOM_FQDN)" in agent
    assert "Add-Computer -DomainName $env:ASB_DOM_FQDN -Credential $cred" in agent
    assert "shutdown.exe /r /t 15" in agent
    # New Windows VMs install Pro (index 1 is Home, which can't join); Home says so.
    assert "int image_index = windows_pro_image_index(args->config.image_path);" in core
    assert "-match '^Windows \\\\d+ Pro$'" in core
    assert "@(98,99,100,101) -contains $sku" in agent
    # VM admin rights come from the AppSandbox VM Admins group, applied every boot.
    join = agent[agent.index("static const char DOMAIN_JOIN_PS[]"):]
    join = join[:join.index("static const char *const DOMAIN_ENV[]")]
    assert "/Domain Users')" not in join
    assert join.count("/AppSandbox VM Admins')") == 2
    assert join.index("/AppSandbox VM Admins')") < join.index("result:already")
    assert agent.count("New-ADGroup -Name 'AppSandbox VM Admins'") == 2
    assert "if (document.getElementById('domain-user-vmadmin').checked) groups.push(DOMAIN_VM_ADMINS);" in js
    assert 'cfg.domain_member = !is_tpl && _wcsicmp(account_buf, L"managed") == 0;' in ui
    assert 'id="account-mode"' in html and "managedOption.disabled = !ready;" in js
    # Existing VMs can be made managed from Manage.
    setter = core[core.index("ASB_API HRESULT asb_vm_set_domain_member("):]
    setter = setter[:setter.index("\n}\n")]
    assert "if (!appliance.domain_enabled)" in setter and "inst->domain_member = member" in setter
    assert 'wcscmp(action, L"setVmDomainMember") == 0' in ui
    assert 'id="manage-domain-member"' in html
    assert "sendCmd('setVmDomainMember'" in js

    # SMB on a DC uses the domain account.
    assert 'swprintf_s(out, chars, L"%S\\\\%s", ASB_DOMAIN_NETBIOS, APPLIANCE_SERVICE_USER);' in app
    assert "if(-not $u.Contains('\\\\')){$u=$h+'\\\\'+$u}" in agent

    # UI: Domain button beside Shared Resources, worker-thread bridge.
    assert 'onclick="openDomainModal()"' in html
    assert "static DWORD WINAPI domain_work_thread(" in ui and "WM_DOMAIN_DONE" in ui
    assert "case 'domainResult': onDomainResult(msg); break;" in js
    # Setup progress and its end reach the page; Refresh re-reads the status.
    assert "PostMessageW(g_hwnd_main, WM_DOMAIN_PROGRESS, 0, 0);" in ui
    assert "} while (s.domain_busy);" in ui
    assert "sendCmd('getState');" in js
    # The automatic load runs once (a failure must not loop), and the AD
    # scripts wait for ADWS, which starts minutes after the DC boots.
    auto = js[js.index("Became ready while the dialog was open"):]
    assert auto.index("domainLoaded = true;") < auto.index("domainSend('domainList');")
    assert agent.count("Start-Service ADWS") == 5
    # Remote management can't fail readiness on a DC whose Kerberos is starting.
    assert "}catch{Write-Output 'storage: remote management is not ready yet" in agent
    print("[ok] appliance domain controller and managed VMs contract")


if __name__ == "__main__":
    main()
