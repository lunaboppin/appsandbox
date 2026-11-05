"""Each VM maps the shared appliance's shares with an SMB account of its own,
and the appliance admits that account only to the shares the VM has. The
shared service account stays the host's; a VM is given it only by an
appliance whose agent predates per-VM accounts."""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def between(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i + len(start))]


def main():
    appliance = read("src/backend_win/shared_appliance.c")
    appliance_h = read("src/backend_win/shared_appliance.h")
    core = read("src/backend_win/asb_core.c")
    core_h = read("src/backend_win/asb_core.h")
    hcs_h = read("src/backend_win/hcs_vm.h")
    host_agent = read("src/backend_win/vm_agent.c")
    win_agent = read("tools/agent/agent.c")
    linux_agent = read("tools/linux/agent/appsandbox-agent.c")

    # Naming: random, stable, never the VM's name. asbvm- plus 12 hex fits a
    # sAMAccountName (20) and a Linux user name.
    assert "wchar_t     smb_account[24];" in hcs_h
    assert '#define APPLIANCE_VM_USER_PREFIX L"asbvm-"' in appliance
    assert "#define APPLIANCE_VM_USER_HEX 12" in appliance
    new = between(appliance, "HRESULT shared_appliance_new_vm_account", "static void vm_account_dir")
    assert "BCryptGenRandom" in new and "name" not in new.split("{", 1)[1]
    assert len("asbvm-") + 12 <= 20

    # Kept in vms.cfg, made for new VMs and for VMs from before this.
    assert 'fwprintf(f, L"SmbAccount=%s\\n", g_vms[i].smb_account);' in core
    load = between(core, 'L"SmbAccount=", 11', "else if")
    assert "shared_appliance_vm_account_valid(line + 11)" in load
    create = between(core, "ZeroMemory(inst, sizeof(VmInstance));", "secure_boot_off")
    assert "shared_appliance_new_vm_account(inst->smb_account" in create
    init = between(core, "load_vm_list();\n", "shared_resources_init();")
    assert "shared_appliance_new_vm_account(g_vms[ai].smb_account" in init and "save_vm_list();" in init

    # The password stays on the host: DPAPI, admin-only file, one per account.
    save = between(appliance, "static HRESULT vm_account_save_password", "static HRESULT vm_account_password")
    assert "protect_text(password, &blob)" in save
    assert "credential_security_attributes(&sa, &descriptor)" in save
    assert "CreateDirectoryW(dir, &sa)" in save
    assert 'L"%s\\\\smb-accounts", root' in appliance
    assert "unprotect_text(blob, read, out, chars)" in appliance

    # Clients get their own account, not the shared one.
    assert "shared_appliance_get_vm_smb_credentials(vm," in host_agent
    assert "shared_appliance_get_vm_smb_credentials(instance," in host_agent
    assert "shared_appliance_get_smb_credentials(" not in host_agent
    creds = between(appliance, "BOOL shared_appliance_get_vm_smb_credentials", "const char *shared_appliance_server_ip")
    # The shared account only for an appliance agent that can't do better.
    legacy = creds.index("mode == VM_ACCOUNTS_LEGACY")
    assert legacy < creds.index("shared_appliance_get_smb_credentials(")
    assert "vm_account_ready_locked(vm->smb_account)" in creds
    assert "smb_account_user(NULL, vm->smb_account" in creds
    # A VM not provisioned yet stays pending and asks for a publish pass,
    # which maps it; the publish pass itself never asks for another.
    boot = between(host_agent, "shared_appliance_get_vm_smb_credentials(vm,", "shared_mapping_done:")
    assert "vm->shared_resource_pending = TRUE;" in boot
    assert "asb_request_shared_resource_sync();" in boot
    assert "if (FAILED(cred_hr)) continue;" in boot
    assert "user_drives:%s" in boot  # per-user drives still sent
    runtime = between(host_agent, "BOOL vm_agent_map_shared_resource", "BOOL vm_agent_unmap_shared_resource")
    assert "asb_request_shared_resource_sync" not in runtime

    # Publishing: prune first (also the capability probe), then accounts, then
    # each share with exactly the VMs that have it.
    reconcile = between(appliance, "static HRESULT reconcile_internal(void)\n{", "static BOOL appliance_client_ip_in_use")
    assert reconcile.index('"appliance_account:%s:%s"') < reconcile.index("provision_vm_accounts(")
    assert reconcile.index("provision_vm_accounts(") < reconcile.index("shared_resources_build_attachments(")
    assert "asb_vm_share_access(vms, ASB_MAX_VMS)" in reconcile
    assert "vm_accounts_sweep_host(vms, vm_count)" in reconcile
    assert "shared_resources_is_excluded(vms[j].exclusions, resources[i].id)" in reconcile
    assert "!granted[j]" in reconcile
    assert '"appliance_reconcile:%s:%s:%s:%s"' in reconcile
    # A group-restricted share is mapped per user, never by a VM account.
    assert "for (j = 0; j < vm_count && !group_only; ++j)" in reconcile
    provision = between(appliance, "static LONG provision_vm_accounts", "static HRESULT reconcile_internal(void)\n{")
    assert provision.index('"appliance_vm_prune:%s"') < provision.index('"appliance_account:%s:%s"')
    assert 'strcmp(response, "error:unknown") == 0' in provision
    assert "VM_ACCOUNTS_LEGACY" in provision and "VM_ACCOUNTS_SUPPORTED" in provision
    assert "SecureZeroMemory(password, sizeof(password));" in provision
    assert "SecureZeroMemory(password_w, sizeof(password_w));" in provision
    assert "vm_account_set_ready(vms[i].smb_account, TRUE);" in provision
    # Every appliance boot starts over: new agent, accounts given again.
    assert re.search(r"vm_accounts_reset\(\);\s*hr = reconcile_internal\(\);", appliance)

    # Snapshot taken under the VM list lock.
    assert "int asb_vm_share_access(AsbVmShareAccess *out, int capacity);" in core_h
    snap = between(core, "int asb_vm_share_access(AsbVmShareAccess *out, int capacity)", "/* Runs off")
    assert "EnterCriticalSection(&g_cs);" in snap and "shared_resource_exclusions" in snap
    assert "AsbVmShareAccess;" in appliance_h

    # Exclusion changes and deletes republish; a deleted VM's account goes.
    toggle = between(core, "ASB_API HRESULT asb_vm_set_shared_resource_enabled", "ASB_API void asb_shared_appliance_get_status")
    assert "asb_request_shared_resource_sync();" in toggle
    remove = between(core, "static HRESULT vm_remove(AsbVm vm, BOOL keep_files)", "static HRESULT session_clone_drop")
    assert "wcscpy_s(account, _countof(account), inst->smb_account);" in remove
    assert remove.index("g_vm_count--;") < remove.index("shared_appliance_forget_vm_account(account);")
    assert "asb_request_shared_resource_sync();" in remove
    forget = between(appliance, "void shared_appliance_forget_vm_account", "static void vm_accounts_sweep_host")
    assert "DeleteFileW(path);" in forget and "vm_account_set_ready(name, FALSE);" in forget

    # VM accounts can't be managed or listed as domain users.
    protected = between(appliance, "static BOOL domain_protected_user", "static void domain_message")
    assert "APPLIANCE_VM_USER_PREFIX" in protected
    assert win_agent.count("$_.SamAccountName -notlike 'asbvm-*'") == 2

    # Windows appliance: only the listed VM accounts on the share and folder,
    # stale VM rules removed, unlisted VM accounts deleted.
    win_share = between(win_agent, "static DWORD appliance_reconcile_share", "static DWORD appliance_purge_share")
    assert "appliance_vm_list_ok(users)" in win_share
    assert 'SetEnvironmentVariableA("ASB_SHARE_USERS"' in win_share
    assert "$n -like 'asbvm-*' -and $keep -notcontains $n" in win_share
    assert "RemoveAccessRuleSpecific" in win_share
    assert "$who+=$u" in win_share
    assert "strlen(arguments) >= sizeof(copy)" in win_share
    win_prune = between(win_agent, "static DWORD appliance_prune_vm_accounts", "static DWORD appliance_reconcile_share")
    assert "appliance_vm_list_ok(keep)" in win_prune
    assert "$_ -like 'asbvm-*' -and $keep -notcontains $_" in win_prune
    assert "$root.Delete('User',$n)" in win_prune
    assert 'strncmp(cmd, "appliance_vm_prune:", 19) == 0' in win_agent

    # Linux (Samba) appliance: valid users is the service account plus the VMs.
    lin_share = between(linux_agent, "static int appliance_reconcile_share", "static int appliance_purge_share")
    assert "appliance_vm_list_ok(users)" in lin_share
    assert "valid users = %s" in lin_share and "force user = AppSandboxShare" in lin_share
    assert "strlen(arguments) >= sizeof(copy)" in lin_share
    lin_prune = between(linux_agent, "static int appliance_prune_vm_accounts", "static int appliance_reconcile_share")
    assert "grep '^asbvm-'" in lin_prune and "userdel" in lin_prune and "smbpasswd -x" in lin_prune
    assert 'strncmp(cmd, "appliance_vm_prune:", 19) == 0' in linux_agent

    # Both agents validate VM account lists the same strict way.
    for agent in (win_agent, linux_agent):
        ok = between(agent, "appliance_vm_list_ok(const char *list)", "appliance_prune_vm_accounts")
        assert 'strncmp(p, "asbvm-", 6)' in ok and "n < 12" in ok

    # Password-bearing commands stay redacted on both ends.
    assert 'strncmp(command, "appliance_account:", 18) == 0' in host_agent
    assert 'strncmp(command, "shared_smb_map:", 15) == 0' in host_agent
    assert 'strncmp(cmd, "appliance_account:", 18) == 0' in linux_agent

    print("per-VM share accounts contract: ok")


if __name__ == "__main__":
    main()
