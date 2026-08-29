/* App Sandbox - WebView2 Frontend */
'use strict';

/* ---- State ---- */
let vms = [];
let selectedVm = -1;
let selectedSnap = {};  /* vmIndex -> string value: 'current', 'base', 'base-N', 'S', 'S-N' */
let editModeRow = -1;
let editingCell = null; /* {row, col, element} */
let pendingConfirm = null; /* {resolve} */
let minSizeReported = false;
let lastHostInfo = null;
let rowCache = {};          /* vm.name -> <tr> — persistent rows so the status spinner doesn't reset on every update */
let rowSigCache = {};       /* vm.name -> last render signature; skip rebuild when unchanged */
let manageVmIndex = -1;
let managePending = false;
let sharedResources = [];
let sharedAppliance = {};
let appSettings = {};

/* ---- Collapsible sections ---- */
function toggleSection(id) {
    var section = document.getElementById(id);
    var collapsed = section.classList.toggle('collapsed');
    localStorage.setItem('collapse_' + id, collapsed ? '1' : '0');
}
(function restoreCollapse() {
    var defaults = { 'log-section': '1' };
    Object.keys(defaults).forEach(function(id) {
        var val = localStorage.getItem('collapse_' + id);
        if (val === null) val = defaults[id];
        if (val === '1') document.getElementById(id).classList.add('collapsed');
    });
})();

const netNames = ['None', 'NAT', 'External', 'Internal'];

/* ---- Message bridge ----
 *
 * Two host environments are supported:
 *   - WebView2 on Windows  (window.chrome.webview)
 *   - WKWebView on macOS   (window.webkit.messageHandlers.host)
 *
 * Native code on both platforms calls window.onHostMessage(obj) with a
 * parsed message object; the JS side only sees one uniform surface. On
 * Windows we keep using the native chrome.webview event path because it
 * is the existing, tested route — onHostMessage is simply wired into the
 * same listener.
 */

var hostBridge = (function() {
    var isWebView2 = !!(window.chrome && window.chrome.webview);
    var isWKWebView = !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.host);

    function send(action, data) {
        var msg = Object.assign({ action: action }, data || {});
        if (isWebView2) {
            window.chrome.webview.postMessage(msg);
        } else if (isWKWebView) {
            /* WKWebView only accepts JSON-serializable values; strings round-trip
             * most reliably so we hand the native side the raw JSON text. */
            window.webkit.messageHandlers.host.postMessage(JSON.stringify(msg));
        } else {
            console.warn('[hostBridge] no native host available; dropping', msg);
        }
    }

    return { send: send, isWebView2: isWebView2, isWKWebView: isWKWebView, isMac: isWKWebView };
})();

function sendCmd(action, data) { hostBridge.send(action, data); }

/* On a macOS host, hide the Windows-*host*-only features (templates, snapshots,
 * test-mode, build-template — none supported when the host is a Mac) and the
 * dormant Linux-version row. The OS-type dropdown stays ENABLED so the user can
 * pick Windows (built from a Microsoft ISO via QEMU) or macOS (VZ restore image).
 * Per-OS field visibility — including the .needs-iso picker — is driven by
 * applyOsTypeUI(), which runs on both hosts. */
if (hostBridge.isMac) {
    var hide = document.querySelectorAll('.win-only, .win-host-only, .needs-linux-version');
    for (var i = 0; i < hide.length; i++) hide[i].style.display = 'none';
}

/* OS Type dropdown: drop guest types that aren't available on this host.
 * Windows host: macOS unavailable (Apple Virtualization is Mac-only).
 * macOS host:   Linux unavailable (Windows IS supported — QEMU+ivshmem). */
{
    var unavailable = hostBridge.isMac ? ['Linux'] : ['macOS'];
    unavailable.forEach(function(v) {
        var opt = document.querySelector('#os-type option[value="' + v + '"]');
        if (opt) opt.remove();
    });
}

/* Apply Create-modal visibility rules for the currently selected OS type.
 *   Windows: .win-only shown, .needs-iso shown,             .needs-linux-version hidden
 *   Linux:   .win-only hidden, .needs-iso hidden,           .needs-linux-version shown
 *   macOS:   handled by the isMac branch above; this function is a no-op there.
 *
 * Linux is back to user-picks-an-ISO (Ubuntu Desktop ISO etc.), same as
 * Windows. The version-dropdown / cloud-image flow is preserved in
 * asb_core.c under #if 0 in case we need to bring it back. */
function applyOsTypeUI() {
    var osType = document.getElementById('os-type').value;
    var isWindows = osType === 'Windows';
    var isLinux = osType === 'Linux';
    var winOnly = document.querySelectorAll('.win-only');
    var needsIso = document.querySelectorAll('.needs-iso');
    var needsWindows = document.querySelectorAll('.needs-windows');
    var needsLinuxVersion = document.querySelectorAll('.needs-linux-version');
    /* .win-only = template/snapshot features that exist only on a Windows *host*;
       never shown on a Mac host, even for a Windows guest. */
    for (var i = 0; i < winOnly.length; i++)
        winOnly[i].style.display = (!hostBridge.isMac && isWindows) ? '' : 'none';
    /* .needs-windows = Windows-*guest* options (Test Mode); shown for a Windows
       guest on EITHER host (a Windows-on-Mac VM uses it too), hidden otherwise. */
    for (var w = 0; w < needsWindows.length; w++) needsWindows[w].style.display = isWindows ? '' : 'none';
    /* ISO picker shows for both Windows and Linux now. */
    for (var j = 0; j < needsIso.length; j++) needsIso[j].style.display = (isWindows || isLinux) ? '' : 'none';
    /* Linux distribution dropdown is dormant — kept in the DOM but always
       hidden so the cloud-image code path can be revived without
       re-adding the markup. */
    for (var k = 0; k < needsLinuxVersion.length; k++) needsLinuxVersion[k].style.display = 'none';
    /* Swap the default VM name between OS conventions, but only when the
       field still holds the *other* OS's untouched default — never clobber a
       name the user typed. Linux hostnames must be lowercase. */
    var nameEl = document.getElementById('vm-name');
    if (isLinux && nameEl.value === 'MyAppSandbox') nameEl.value = 'myappsandbox';
    else if (!isLinux && nameEl.value === 'myappsandbox') nameEl.value = 'MyAppSandbox';
    revalidateVmName();
    revalidateUsername();
    revalidatePassword();
    updateCreateButtons();
}

/* Unified dispatch. Native code on either platform calls
 * window.onHostMessage(obj) with an already-parsed object. WebView2 also
 * delivers messages through chrome.webview.addEventListener('message'),
 * which we route into the same handler so both paths end up in one place. */
window.onHostMessage = function(msg) {
    if (!msg || typeof msg !== 'object') return;
    switch (msg.type) {
        case 'fullState':     onFullState(msg); break;
        case 'vmListChanged': vms = msg.vms; renderVmTable(); updateHostInfo(msg.hostInfo); revalidateVmName(); refreshManageVm(); break;
        case 'vmStateChanged': onVmStateChanged(msg); break;
        case 'snapListChanged': break; /* snapshots now inline in vmListChanged */
        case 'log':           appendLog(msg.message); break;
        case 'hostInfo':      updateHostInfo(msg); break;
        case 'browseResult':  onBrowseResult(msg.path); break;
        case 'manageBrowseResult': onManageBrowseResult(msg); break;
        case 'createStorageBrowseResult': document.getElementById('storage-parent').value = msg.path || ''; break;
        case 'applianceBrowseResult': document.getElementById(msg.kind === 'iso' ? 'appliance-server-iso' : 'appliance-storage-parent').value = msg.path || ''; break;
        case 'sharedApplianceResult': onSharedApplianceResult(msg); break;
        case 'sharedResourceResult': onSharedResourceResult(msg); break;
        case 'sharedDependencyUnavailable': onSharedDependencyUnavailable(msg); break;
        case 'manageResult':  onManageResult(msg); break;
        case 'confirmResult': if (pendingConfirm) pendingConfirm.resolve(msg.confirmed); break;
        case 'adapters':      populateAdapters(msg.adapters, msg.defaultIndex); break;
        case 'templates':     populateTemplates(msg.templates); break;
        case 'alert':         showModal('Error', msg.message, 'OK'); break;
        case 'prereqRequired': onPrereqRequired(); break;
        case 'prereqReboot':   onPrereqReboot(); break;
        case 'prereqProgress': onPrereqProgress(msg); break;
        case 'prereqResult':   onPrereqResult(msg); break;
    }
};

/* WebView2 delivers events as DOM CustomEvents; forward them into
 * window.onHostMessage so both transports converge on the same handler. */
if (hostBridge.isWebView2) {
    window.chrome.webview.addEventListener('message', function(event) {
        window.onHostMessage(event.data);
    });
}

/* ---- Initial state ---- */

function onFullState(msg) {
    vms = msg.vms || [];
    renderVmTable();
    revalidateVmName();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
    if (msg.adapters) populateAdapters(msg.adapters, msg.defaultAdapter);
    if (msg.templates) populateTemplates(msg.templates);
    lastStorageParent = msg.lastStorageParent || lastStorageParent;
    sharedResources = msg.sharedResources || [];
    sharedAppliance = msg.sharedAppliance || {};
    appSettings = msg.appSettings || {};
    renderSharedAppliance();
    renderSettingsResources();
    renderCreateSharedResources();
    refreshManageVm(true);
    if (!minSizeReported) {
        minSizeReported = true;
        setTimeout(reportMinSize, 50);
    }
}

/* Hyper-V/HCS requires VM memory aligned to 2 MB, so RAM (MB) must be even;
   round an odd value down by 1 (an odd value is rejected and the VM won't boot). */
function alignRamMb(mb) { return mb - (mb % 2); }

function applySmartDefaults(info) {
    var ram = Math.min(Math.floor(info.hostRamMb / 2), 16384);
    var cores = Math.min(Math.floor(info.hostCores / 2), 8);
    if (ram < 512) ram = 512;
    if (cores < 1) cores = 1;
    document.getElementById('ram-size').value = alignRamMb(ram);
    document.getElementById('cpu-cores').value = cores;
}

function onVmStateChanged(msg) {
    if (msg.vmIndex >= 0 && msg.vmIndex < vms.length) {
        Object.assign(vms[msg.vmIndex], msg);
    }
    renderVmTable();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
}

/* ---- Host info ---- */

function updateHostInfo(info) {
    if (!info) return;
    lastHostInfo = info;
    var el;
    el = document.getElementById('host-cpu');
    if (el) el.textContent = 'Host: ' + info.hostCores + ' cores | VMs using: ' + info.vmCores;
    el = document.getElementById('host-ram');
    if (el) el.textContent = 'Host: ' + info.hostRamMb + ' MB | VMs using: ' + info.vmRamMb + ' MB';
    el = document.getElementById('host-hdd');
    if (el) el.textContent = 'Free: ' + info.freeGb + ' GB | VMs allocated: ' + info.vmHddGb + ' GB';
}

/* ---- Adapters ---- */

var currentAdapters = [];
var currentDefaultAdapter = '';

function populateAdapters(adapters, defaultIdx) {
    var sel = document.getElementById('net-adapter');
    sel.innerHTML = '<option value="">(Auto)</option>';
    currentAdapters = adapters || [];
    if (adapters) {
        adapters.forEach(function(a) {
            var opt = document.createElement('option');
            opt.value = a;
            opt.textContent = a;
            sel.appendChild(opt);
        });
    }
    if (typeof defaultIdx === 'number' && defaultIdx >= 0 && defaultIdx < sel.options.length) {
        sel.selectedIndex = defaultIdx;
        currentDefaultAdapter = sel.value;
    } else if (adapters && adapters.length > 0) {
        currentDefaultAdapter = adapters[0];
    }
}

/* ---- Templates ---- */

var currentTemplates = [];
var lastStorageParent = '';

function templateDefaultLabel() {
    var n = currentTemplates.length;
    if (n === 0) return '(None)';
    return '(' + n + ' template' + (n === 1 ? '' : 's') + ' available)';
}

function populateTemplates(templates) {
    currentTemplates = templates || [];
    var list = document.getElementById('template-dropdown-list');
    var hidden = document.getElementById('template-select');
    list.innerHTML = '';

    /* Default (None) item — always shows "None" inside the list */
    var noneItem = document.createElement('div');
    noneItem.className = 'template-dropdown-item';
    noneItem.innerHTML = '<span class="tpl-name">(None)</span>';
    noneItem.addEventListener('click', function() { selectTemplate('', templateDefaultLabel()); });
    list.appendChild(noneItem);

    currentTemplates.forEach(function(t) {
        var item = document.createElement('div');
        item.className = 'template-dropdown-item';

        var nameSpan = document.createElement('span');
        nameSpan.className = 'tpl-name';
        nameSpan.textContent = t.name + ' [' + t.osType + ']';
        item.appendChild(nameSpan);

        var delBtn = document.createElement('span');
        delBtn.className = 'tpl-delete';
        delBtn.textContent = '\uD83D\uDDD1\uFE0F';
        delBtn.title = 'Delete template';
        delBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            closeTemplateDropdown();
            onDeleteTemplate(t.name);
        });
        item.appendChild(delBtn);

        item.addEventListener('click', function() {
            selectTemplate(t.name, t.name + ' [' + t.osType + ']');
        });
        list.appendChild(item);
    });

    /* If the currently selected template was deleted, reset */
    if (hidden.value !== '') {
        var found = currentTemplates.some(function(t) { return t.name === hidden.value; });
        if (!found) selectTemplate('', templateDefaultLabel());
    } else {
        /* No template selected — update default label in case count changed */
        document.getElementById('template-dropdown-selected').textContent = templateDefaultLabel();
    }
}

function selectTemplate(value, label) {
    document.getElementById('template-select').value = value;
    document.getElementById('template-dropdown-selected').textContent = label;
    closeTemplateDropdown();
    if (value !== '') {
        document.getElementById('image-path').value = '';
    }
    updateCreateButtons();
}

function closeTemplateDropdown() {
    document.getElementById('template-dropdown').classList.remove('open');
}

document.getElementById('template-dropdown-selected').addEventListener('click', function() {
    document.getElementById('template-dropdown').classList.toggle('open');
});

/* Close dropdown when clicking outside */
document.addEventListener('click', function(e) {
    if (!e.target.closest('#template-dropdown')) {
        closeTemplateDropdown();
    }
});

function onDeleteTemplate(name) {
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete template "' + name + '"?\n\nThis will permanently delete the template disk image.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteTemplate', { name: name });
        }
    });
}

/* ---- Browse result ---- */

function onBrowseResult(path) {
    if (path) {
        document.getElementById('image-path').value = path;
        selectTemplate('', templateDefaultLabel());
        updateCreateButtons();
    }
}

/* ---- Create buttons state ---- */

function updateCreateButtons() {
    var osType = document.getElementById('os-type').value;
    var hasImage = (document.getElementById('image-path').value.trim() !== '');
    var hasTpl = document.getElementById('template-select').value !== '';
    /* macOS guests auto-download their restore image (no path needed). Windows
       and Linux guests build from a user-picked ISO — or, on a Windows host, a
       saved template. Holds on both hosts: on a Mac the template UI is hidden so
       hasTpl stays false and a Windows guest genuinely requires the ISO. */
    var createOk = (osType === 'macOS') ? true : (hasImage || hasTpl);
    document.getElementById('btn-create').disabled = !createOk;
    /* Templates are Windows-only; disabling create-as-template for Linux
       (and macOS) is fine since hasImage is the only signal we check. */
    document.getElementById('btn-create-template').disabled = (osType !== 'Windows') || !hasImage;
}

/* Wire up change events */
document.getElementById('image-path').addEventListener('input', function() {
    if (this.value.trim() !== '') {
        selectTemplate('', templateDefaultLabel());
    }
    updateCreateButtons();
});

/* RAM must be 2 MB-aligned: snap an odd entry down by 1 when the field is committed. */
document.getElementById('ram-size').addEventListener('change', function() {
    var mb = parseInt(this.value, 10);
    if (!isNaN(mb)) this.value = alignRamMb(mb);
});

function revalidateVmName() {
    var name = document.getElementById('vm-name').value.trim();
    document.getElementById('vm-name-warn').textContent = validateVmName(name) || '';
}
document.getElementById('vm-name').addEventListener('input', revalidateVmName);

function revalidateUsername() {
    var u = document.getElementById('admin-user').value.trim();
    document.getElementById('admin-user-warn').textContent = validateUsername(u) || '';
}
function revalidatePassword() {
    var p = document.getElementById('admin-pass').value;
    document.getElementById('admin-pass-warn').textContent = validatePassword(p) || '';
}
document.getElementById('admin-user').addEventListener('input', revalidateUsername);

function checkPasswordMatch() {
    var pass = document.getElementById('admin-pass').value;
    var confirm = document.getElementById('admin-confirm');
    if (confirm.value === '' && pass === '') {
        confirm.classList.remove('pass-mismatch', 'pass-match');
        return;
    }
    if (confirm.value === pass) {
        confirm.classList.remove('pass-mismatch');
        confirm.classList.add('pass-match');
    } else {
        confirm.classList.remove('pass-match');
        confirm.classList.add('pass-mismatch');
    }
}
document.getElementById('admin-pass').addEventListener('input', function() {
    checkPasswordMatch();
    revalidatePassword();
});
document.getElementById('admin-confirm').addEventListener('input', checkPasswordMatch);
checkPasswordMatch();

function showPassword() {
    document.getElementById('admin-pass').type = 'text';
    document.getElementById('admin-confirm').type = 'text';
}
function hidePassword() {
    document.getElementById('admin-pass').type = 'password';
    document.getElementById('admin-confirm').type = 'password';
}

function onNetModeChange() {
    /* Adapter dropdown only relevant for External */
    var mode = parseInt(document.getElementById('net-mode').value);
    var show = (mode === 2) ? '' : 'none';
    document.getElementById('net-adapter').style.display = show;
    document.getElementById('net-adapter-label').style.display = show;
}
onNetModeChange();

/* ---- Create VM ---- */

function gatherConfig() {
    var osType = document.getElementById('os-type').value;
    /* Same ISO-picker path for Windows and Linux. The cloud-image
       Linux-version dropdown is dormant (see applyOsTypeUI). */
    var imagePath = document.getElementById('image-path').value.trim();
    var excluded = [];
    document.querySelectorAll('#create-shared-resource-list input[data-resource-id]').forEach(function(cb) {
        if (!cb.checked) excluded.push(cb.dataset.resourceId);
    });
    return {
        name:        document.getElementById('vm-name').value.trim(),
        osType:      osType,
        imagePath:   imagePath,
        storageParent: document.getElementById('storage-parent').value.trim(),
        sharedResourceExclusions: excluded.join(','),
        templateName: document.getElementById('template-select').value,
        hddGb:       parseInt(document.getElementById('hdd-size').value) || 64,
        ramMb:       alignRamMb(parseInt(document.getElementById('ram-size').value) || 16384),
        cpuCores:    parseInt(document.getElementById('cpu-cores').value) || 8,
        gpuMode:     parseInt(document.getElementById('gpu-mode').value),
        networkMode: parseInt(document.getElementById('net-mode').value),
        netAdapter:  document.getElementById('net-adapter').value,
        adminUser:   document.getElementById('admin-user').value.trim(),
        adminPass:   document.getElementById('admin-pass').value,
        adminConfirm: document.getElementById('admin-confirm').value,
        testMode:    document.getElementById('test-mode').checked,
        sshEnabled:  document.getElementById('ssh-enabled').checked,
        sshDeployKey: document.getElementById('ssh-deploy-key').checked
    };
}

/* "Deploy SSH key" depends on "SSH Server": grey it out (and clear it) unless
   SSH is enabled. The core also gates deploy on ssh_enabled as a backstop. */
function onSshToggle() {
    var ssh = document.getElementById('ssh-enabled').checked;
    var dep = document.getElementById('ssh-deploy-key');
    dep.disabled = !ssh;
    if (!ssh) dep.checked = false;
}

function clearCreateForm() {
    document.getElementById('image-path').value = '';
    selectTemplate('', templateDefaultLabel());
    updateCreateButtons();
}

/* VM name / hostname validation. Per-guest-OS rules, keyed off the
   selected OS Type (on a macOS host the dropdown is locked to 'macOS',
   so osType is an accurate guest discriminator on all hosts). */
function validateVmName(name) {
    if (!name) return 'VM name is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'macOS') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (macOS LocalHostName limit).';
    } else if (osType === 'Linux') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (Linux hostname limit).';
        if (/[A-Z]/.test(name)) return 'Linux hostname must be lowercase.';
    } else { /* Windows */
        if (name.length > 15) return 'VM name cannot exceed 15 characters (NetBIOS limit).';
    }
    if (/[^a-zA-Z0-9-]/.test(name)) return 'VM name can only contain letters, digits, and hyphens.';
    if (/^\d+$/.test(name)) return 'VM name cannot be only digits.';
    if (name.startsWith('-') || name.endsWith('-')) return 'VM name cannot start or end with a hyphen.';
    var lower = name.toLowerCase();
    for (var i = 0; i < vms.length; i++) {
        if (vms[i].name.toLowerCase() === lower) return 'A VM with this name already exists.';
    }
    for (var j = 0; j < currentTemplates.length; j++) {
        if (currentTemplates[j].name.toLowerCase() === lower) return 'A template with this name already exists.';
    }
    return null;
}

/* Username validation. Per-guest-OS rules keyed off osType. Each branch
   is explicit so it's clear which OS's account rules apply. */
function validateUsername(name) {
    if (!name) return 'Username is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        /* Ubuntu useradd/adduser: lowercase, start with a letter or
           underscore, then [a-z0-9_-], max 32 chars. */
        if (name.length > 32) return 'Username cannot exceed 32 characters (Linux limit).';
        if (!/^[a-z_][a-z0-9_-]*$/.test(name))
            return 'Lowercase alphanumeric only.';
        return null;
    }
    /* macOS and Windows: keep the existing Windows-account ruleset.
       (macOS-specific shortname rules are not yet verified; treated the
       same as Windows for now — see validatePassword note.) */
    if (name.length > 20) return 'Username cannot exceed 20 characters.';
    if (/["\\/\[\]:;|=,+*?<>]/.test(name)) return 'Username contains invalid characters.';
    if (/^[.\s]+$/.test(name)) return 'Username cannot be only dots or spaces.';
    if (name.endsWith('.')) return 'Username cannot end with a period.';
    var reserved = ['CON','PRN','AUX','NUL',
        'COM1','COM2','COM3','COM4','COM5','COM6','COM7','COM8','COM9',
        'LPT1','LPT2','LPT3','LPT4','LPT5','LPT6','LPT7','LPT8','LPT9'];
    if (reserved.indexOf(name.toUpperCase()) >= 0) return 'Username is a reserved name.';
    return null;
}

/* Password validation. Per-guest-OS rules keyed off osType.
   - Linux: Ubuntu accepts ALL characters via the host's $6$ hash path
     (usermod -p bypasses pwquality), so the only limits are non-empty
     and a sane byte ceiling.
   - macOS / Windows: no extra content rule enforced here today. */
function validatePassword(pass) {
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        if (!pass) return 'Password is required.';
        /* UTF-8 byte length (encodeURIComponent escapes multibyte). */
        var bytes = unescape(encodeURIComponent(pass)).length;
        if (bytes > 255) return 'Password is too long (max 255 bytes).';
        return null;
    }
    /* macOS / Windows: no additional constraints today. */
    return null;
}

function onCreateVm() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    var userErr = validateUsername(cfg.adminUser);
    if (userErr) { sendCmd('log', { message: userErr }); return; }
    var passErr = validatePassword(cfg.adminPass);
    if (passErr) { sendCmd('log', { message: passErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

function onCreateTemplate() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    cfg.isTemplate = true;
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

/* ---- Create Sandbox modal ---- */

function openCreateModal() {
    /* Reset to defaults every time the modal opens */
    document.getElementById('vm-name').value = 'MyAppSandbox';
    document.getElementById('image-path').value = '';
    document.getElementById('storage-parent').value = lastStorageParent;
    renderCreateSharedResources();
    selectTemplate('', templateDefaultLabel());
    document.getElementById('hdd-size').value = 64;
    document.getElementById('gpu-mode').value = '1';
    document.getElementById('net-mode').value = '1';
    document.getElementById('admin-user').value = 'user';
    document.getElementById('admin-pass').value = 'test123';
    document.getElementById('admin-confirm').value = 'test123';
    document.getElementById('test-mode').checked = false;
    document.getElementById('ssh-enabled').checked = false;
    document.getElementById('ssh-deploy-key').checked = false;
    onSshToggle();   /* re-grey "Deploy SSH key" to match the cleared SSH checkbox */
    /* Reset OS type to Windows on each open. Valid on both hosts (a Mac host
       supports Windows via QEMU); the user can switch to macOS on a Mac. */
    document.getElementById('os-type').value = 'Windows';

    /* Smart defaults (RAM/cores) from latest host info */
    if (lastHostInfo) applySmartDefaults(lastHostInfo);

    /* Clear validation state */
    document.getElementById('vm-name-warn').textContent = '';
    document.getElementById('admin-user-warn').textContent = '';
    document.getElementById('admin-pass-warn').textContent = '';
    checkPasswordMatch();
    onNetModeChange();
    applyOsTypeUI();   /* fires updateCreateButtons + revalidateVmName */

    document.getElementById('create-vm-overlay').classList.add('active');
    setTimeout(function() { document.getElementById('vm-name').focus(); }, 0);
}

function closeCreateModal() {
    document.getElementById('create-vm-overlay').classList.remove('active');
}

/* Close on backdrop click — but only when the press also STARTED on the backdrop.
   A click targets the common ancestor of the mousedown and mouseup, so pressing
   inside the modal (e.g. selecting text in a field) and releasing on the backdrop
   would otherwise close it. */
let createBackdropPress = false;
document.getElementById('create-vm-overlay').addEventListener('mousedown', function(e) {
    createBackdropPress = (e.target === this);
});
document.getElementById('create-vm-overlay').addEventListener('click', function(e) {
    if (e.target === this && createBackdropPress) closeCreateModal();
    createBackdropPress = false;
});

/* Close on Escape */
document.addEventListener('keydown', function(e) {
    if (e.key !== 'Escape') return;
    if (document.getElementById('create-vm-overlay').classList.contains('active')) {
        closeCreateModal();
    } else if (document.getElementById('manage-vm-overlay').classList.contains('active')) {
        closeManageVm();
    }
});

/* ---- VM Table ---- */

/* Update the status <td> in place. Preserves the spinner element across
   updates so its CSS animation doesn't restart on every staging-file tick. */
function updateStatusCell(td, vm) {
    var needsSpinner = false;
    var label = '';
    var className = '';

    if (vm.buildingVhdx) {
        needsSpinner = true;
        label = vm.vhdxStaging ? 'Staging files... ' : 'Building Disk (' + (vm.vhdxProgress || 0) + '%) ';
        className = 'status-building';
    } else if (vm.running && vm.shuttingDown) {
        className = 'status-shutting-down';
        label = 'Shutting Down';
    } else if (vm.running && vm.isTemplate) {
        needsSpinner = true;
        label = 'Building Template ';
        className = 'status-building';
    } else if (vm.running && !vm.installComplete && !vm.isTemplate) {
        needsSpinner = true;
        var defaultLabel;
        if (vm.osType === 'macOS')      defaultLabel = 'Installing macOS ';
        else if (vm.osType === 'Linux') defaultLabel = 'Installing Linux ';
        else                            defaultLabel = 'Installing Windows ';
        label = (vm.installStatus && vm.installStatus.length > 0)
            ? (vm.installStatus + ' ')
            : defaultLabel;
        className = 'status-building';
    } else if (vm.running) {
        className = 'status-running';
        label = 'Running';
    } else {
        className = 'status-stopped';
        label = 'Stopped';
    }

    td.className = className;

    var existingSpinner = td.querySelector('.spinner');

    if (needsSpinner) {
        /* Drop any existing children except the spinner, then insert the new text
           before it. The spinner stays in the document the whole time, so its
           CSS animation clock isn't reset. */
        if (existingSpinner) {
            var child = td.firstChild;
            while (child) {
                var next = child.nextSibling;
                if (child !== existingSpinner) td.removeChild(child);
                child = next;
            }
            td.insertBefore(document.createTextNode(label), existingSpinner);
        } else {
            td.textContent = '';
            td.appendChild(document.createTextNode(label));
            var spin = document.createElement('span');
            spin.className = 'spinner';
            td.appendChild(spin);
        }
    } else {
        /* No spinner needed — wipe and set plain text. Any existing spinner is
           removed along with the old text. */
        td.textContent = label;
    }
}

/* Build the list of <td> cells for a row. The status cell is passed in and
   updated in place (rather than recreated) so the spinner animation survives. */
function buildRowCells(vm, i, statusTd) {
    updateStatusCell(statusTd, vm);

    var agentTd = document.createElement('td');
    var agentOff = !vm.running || vm.isTemplate;
    var dotClass = 'agent-dot' + (vm.agentOnline ? ' online' : '') + (agentOff ? ' disabled' : '');
    agentTd.innerHTML = '<span class="' + dotClass + '"></span>';
    agentTd.title = vm.isTemplate
        ? 'Templates do not run the in-VM agent'
        : (!vm.running
            ? 'VM is not running'
            : (vm.agentOnline
                ? 'In-VM agent is connected — host can manage the guest'
                : 'In-VM agent is not connected'));

    var bld = vm.buildingVhdx;
    var snapVal = selectedSnap[i] || 'current';

    var sshActive = vm.sshEnabled && (vm.sshState === 2 || vm.sshState === 4) && vm.running && !bld;
    var sshCell = makeIconCell('ssh', '>_', sshActive, (function(idx) { return function() { sendCmd('sshConnect', {vmIndex: idx}); }; })(i), !vm.sshEnabled ? 'hidden' : '');
    if (vm.sshEnabled) {
        var sshBtn = sshCell.querySelector('.icon-btn');
        if (vm.sshState === 1) sshBtn.title = 'Installing OpenSSH in the guest...';
        else if (vm.sshState === 4) sshBtn.title = 'Open an SSH terminal (localhost:' + vm.sshPort + '; AppSandbox key deployed — key auth works)';
        else if (vm.sshState === 2) sshBtn.title = 'Open an SSH terminal to the VM (localhost:' + vm.sshPort + ', tunneled over HvSocket)';
        else if (vm.sshState === 3) sshBtn.title = 'SSH install failed';
        else sshBtn.title = 'SSH: waiting for the in-VM agent to come online';
    }

    var cells = [
        makeCell(vm.name, i, 0),
        makeCell(vm.osType, i, 1),
        statusTd,
        agentTd,
        makeCell(vm.cpuCores, i, 4, 'Number of virtual CPU cores assigned to this VM'),
        makeCell(vm.ramMb + ' MB', i, 5, 'Memory allocated to this VM, in megabytes'),
        makeCell(vm.hddGb + ' GB', i, 6, 'Virtual disk size, in gigabytes'),
        makeCell(vm.gpuName || (vm.gpuMode === 2 ? 'Try all' : vm.gpuMode === 1 ? 'Default GPU' : 'None'), i, 7, 'GPU passed through to the VM via GPU-PV, or None'),
        makeCell(netNames[vm.networkMode] || 'None', i, 8, 'Networking mode: NAT (shared), External (bridged), Internal (host-only), or None'),
    ];
    if (!hostBridge.isMac) cells.push(makeCheckpointSummaryCell(vm, i));
    cells.push(
        makeIconCell('start', '\u25B6\uFE0F', !vm.running && !bld && !vm.managementBusy, (function(vmIdx, sv, vmObj) { return function() {
            var p = parseSnapValue(sv);
            if ((p.snapIndex >= 0 || p.snapIndex === -2) && p.branchIndex < 0) {
                /* Creating a new branch — prompt for name */
                var parentName = p.snapIndex === -2 ? 'Base' : ((vmObj.snapshots && vmObj.snapshots[p.snapIndex]) ? vmObj.snapshots[p.snapIndex].name : 'Checkpoint');
                var now = new Date();
                var pad = function(n) { return n < 10 ? '0' + n : '' + n; };
                var defaultName = now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + ' ' + pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
                showModal('New Branch', 'A new branch will be created from ' + parentName + '. Branches are independent working copies \u2014 changes in one branch don\u2019t affect others or modify the checkpoint.', 'Boot', {
                    confirmClass: 'primary',
                    input: { label: 'Branch name:', value: defaultName }
                }).then(function(result) {
                    if (result === false) return;
                    selectedSnap[vmIdx] = 'current';
                    sendCmd('startVm', { vmIndex: vmIdx, snapIndex: p.snapIndex, branchIndex: p.branchIndex, branchName: result });
                });
            } else {
                sendCmd('startVm', { vmIndex: vmIdx, snapIndex: p.snapIndex, branchIndex: p.branchIndex });
            }
        }; })(i, snapVal, vm), '', 'Start the VM (boots from the selected checkpoint/branch)'),
        makeIconCell('connect-idd', '\uD83D\uDCFA', vm.running && !bld, function() { sendCmd('connectIddVm', {vmIndex: i}); }, '', 'Open the VM display window (IDD virtual monitor)'),
        sshCell,
        makeIconCell('shutdown', '\u23FB', vm.running && !bld, function() { sendCmd('shutdownVm', {vmIndex: i}); }, '', 'Request a graceful shutdown from the guest OS'),
        makeIconCell('stop', '\u2715\uFE0F', vm.running && !bld, function() { onStopVm(i); }, '', 'Force power off the VM immediately (may lose unsaved guest data)'),
        makeIconCell('delete', '\uD83D\uDDD1\uFE0F', !bld && !vm.managementBusy, function() { onDeleteVm(i); }, vm.running ? 'running' : '', 'Delete this VM and its virtual disks'),
        makeIconCell('manage', '\u2699\uFE0F', !bld, function() { openManageVm(i); }, '', 'Manage installer media, storage, disk, display, and checkpoints'),
        makeIconCell('edit', editModeRow === i ? '\u2714\uFE0F' : '\u270F\uFE0F', !vm.running && !bld && !vm.managementBusy, function() { toggleEditMode(i); }, '', 'Edit VM configuration (CPU, RAM, GPU, network) — VM must be stopped and idle'),
    );
    return cells;
}

function renderVmTable() {
    var tbody = document.getElementById('vm-tbody');

    if (vms.length === 0) {
        rowCache = {};
        rowSigCache = {};
        tbody.innerHTML = '';
        var tr = document.createElement('tr');
        var td = document.createElement('td');
        td.colSpan = hostBridge.isMac ? 16 : 18;
        td.className = 'empty-state';
        var btn = document.createElement('button');
        btn.className = 'primary empty-state-btn';
        btn.textContent = '+ Create your first sandbox';
        btn.onclick = openCreateModal;
        td.appendChild(btn);
        tr.appendChild(td);
        tbody.appendChild(tr);
        return;
    }

    /* Drop cached rows for VMs that no longer exist. */
    var seen = {};
    vms.forEach(function(vm) { seen[vm.name] = true; });
    Object.keys(rowCache).forEach(function(name) {
        if (!seen[name]) {
            var stale = rowCache[name];
            if (stale.parentNode) stale.parentNode.removeChild(stale);
            delete rowCache[name];
            delete rowSigCache[name];
        }
    });

    /* Remove any non-cached tbody children (e.g. leftover empty-state row). */
    var kids = Array.prototype.slice.call(tbody.children);
    kids.forEach(function(c) {
        var cached = false;
        for (var n in rowCache) { if (rowCache[n] === c) { cached = true; break; } }
        if (!cached) tbody.removeChild(c);
    });

    /* Skip the cell rebuild when button-relevant fields are unchanged; the
     * install progress tick would otherwise destroy the button DOM mid-click. */
    vms.forEach(function(vm, i) {
        var tr = rowCache[vm.name];
        var firstBuild = !tr;
        if (!tr) {
            tr = document.createElement('tr');
            rowCache[vm.name] = tr;
        }

        var statusTd = tr.children[2] || document.createElement('td');
        updateStatusCell(statusTd, vm);

        var sig = [
            i === selectedVm, editModeRow === i,
            vm.running, vm.buildingVhdx, vm.managementBusy, vm.shuttingDown, vm.agentOnline,
            vm.installComplete, vm.isTemplate,
            vm.sshEnabled, vm.sshState, vm.sshPort,
            vm.osType, vm.ramMb, vm.hddGb, vm.cpuCores,
            vm.gpuMode, vm.gpuName, vm.networkMode,
            vm.imagePath, vm.storageDir, vm.autoOpenDisplay, vm.managementBusy,
            vm.sharedResourceExclusions, vm.sharedResourceTransport,
            vm.sharedResourceError, vm.sharedResourcePending,
            selectedSnap[i] || 'current',
            /* Snapshot tree: take/delete/rename/branch must trigger a row rebuild
               so makeSnapCell re-runs. These fields only change on user snapshot
               actions (never on install-progress ticks — a VM can't be snapshotted
               while running), so the rebuild-skip optimization above is preserved. */
            vm.hasSnapshots, vm.snapCurrent, vm.snapCurrentBranch,
            JSON.stringify(vm.snapshots || []), JSON.stringify(vm.baseBranches || [])
        ].join('|');

        if (!firstBuild && rowSigCache[vm.name] === sig) {
            if (tbody.children[i] !== tr) {
                tbody.insertBefore(tr, tbody.children[i] || null);
            }
            return;
        }
        rowSigCache[vm.name] = sig;

        tr.className = (i === selectedVm ? 'selected ' : '') +
                       (vm.running ? 'running' : 'stopped');
        tr.onclick = function(e) {
            if (e.target.closest('.icon-btn')) return;
            if (e.target.closest('.editing')) return;
            if (e.target.closest('.snap-cell')) return;
            selectVm(i);
        };

        var cells = buildRowCells(vm, i, statusTd);

        for (var c = 0; c < cells.length; c++) {
            var newCell = cells[c];
            var oldCell = tr.children[c];
            if (oldCell === newCell) continue;
            if (oldCell) tr.replaceChild(newCell, oldCell);
            else tr.appendChild(newCell);
        }
        while (tr.children.length > cells.length) tr.removeChild(tr.lastChild);

        if (tbody.children[i] !== tr) {
            tbody.insertBefore(tr, tbody.children[i] || null);
        }
    });
}

function makeCell(text, row, col, title) {
    var td = document.createElement('td');
    td.textContent = text;
    if (title) td.title = title;

    /* Editable columns: 4=CPU, 5=RAM, 7=GPU, 8=Network */
    if (editModeRow === row && (col === 4 || col === 5 || col === 7 || col === 8)) {
        td.style.cursor = 'pointer';
        td.title = 'Click to edit';
        td.onclick = function(e) {
            e.stopPropagation();
            startInlineEdit(row, col, td);
        };
    }
    return td;
}

function makeIconCell(cls, icon, active, handler, extraClass, title) {
    var td = document.createElement('td');
    td.className = 'icon-col';
    var btn = document.createElement('button');
    btn.className = 'icon-btn ' + cls + (active ? '' : ' inactive') + (extraClass ? ' ' + extraClass : '');
    btn.textContent = icon;
    if (title) btn.title = title;
    if (active) btn.onclick = handler;
    else btn.disabled = true;
    td.appendChild(btn);
    return td;
}

/* ---- VM Selection ---- */

function selectVm(idx) {
    if (editingCell) commitInlineEdit();
    if (editModeRow >= 0 && editModeRow !== idx) editModeRow = -1;
    selectedVm = idx;
    renderVmTable();
    sendCmd('selectVm', { vmIndex: idx });
}

/* ---- Inline Editing ---- */

function toggleEditMode(row) {
    if (editingCell) commitInlineEdit();
    if (vms[row] && vms[row].running) return;
    editModeRow = (editModeRow === row) ? -1 : row;
    renderVmTable();
}

function startInlineEdit(row, col, td) {
    if (editingCell) commitInlineEdit();
    var vm = vms[row];
    if (!vm || vm.running) return;

    var oldValue;
    /* Lock cell width before swapping content to prevent column resize */
    var cellWidth = td.getBoundingClientRect().width;
    td.style.width = cellWidth + 'px';
    td.style.maxWidth = cellWidth + 'px';
    td.classList.add('editing');

    if (col === 7) {
        /* GPU combo */
        var sel = document.createElement('select');
        sel.innerHTML = '<option value="0">None</option><option value="1">Default GPU</option><option value="2">Try all</option>';
        sel.value = String(vm.gpuMode);
        sel.onclick = function(e) { e.stopPropagation(); };
        sel.onchange = function() { commitInlineEdit(); };
        sel.onblur = function() { setTimeout(commitInlineEdit, 100); };
        td.textContent = '';
        td.appendChild(sel);
        editingCell = { row: row, col: col, element: sel };
        sel.focus();
        setTimeout(function() { try { sel.showPicker(); } catch(e) {} }, 0);
    } else if (col === 8) {
        /* Network combo */
        var sel = document.createElement('select');
        sel.innerHTML = '<option value="0">None</option><option value="1">NAT</option><option value="2">External</option><option value="3">Internal</option>';
        sel.value = String(vm.networkMode);
        sel.onclick = function(e) { e.stopPropagation(); };
        sel.onchange = function() { commitInlineEdit(); };
        sel.onblur = function() { setTimeout(commitInlineEdit, 100); };
        td.textContent = '';
        td.appendChild(sel);
        editingCell = { row: row, col: col, element: sel };
        sel.focus();
        setTimeout(function() { try { sel.showPicker(); } catch(e) {} }, 0);
    } else {
        /* Text/number input */
        var inp = document.createElement('input');
        inp.type = 'number';
        inp.value = col === 4 ? String(vm.cpuCores) : String(vm.ramMb);
        inp.onkeydown = function(e) {
            if (e.key === 'Enter') commitInlineEdit();
            else if (e.key === 'Escape') cancelInlineEdit();
        };
        inp.onblur = function() { commitInlineEdit(); };
        td.textContent = '';
        td.appendChild(inp);
        inp.select();
        inp.focus();
        editingCell = { row: row, col: col, element: inp };
    }
}

function commitInlineEdit() {
    if (!editingCell) return;
    var el = editingCell.element;
    var row = editingCell.row;
    var col = editingCell.col;
    var value = el.value;
    editingCell = null;

    var field;
    if (col === 4) field = 'cpuCores';
    else if (col === 5) field = 'ramMb';
    else if (col === 7) field = 'gpuMode';
    else if (col === 8) field = 'networkMode';

    /* RAM must be 2 MB-aligned (HCS requirement): round an odd entry down by 1. */
    if (field === 'ramMb') {
        var mb = parseInt(value, 10);
        if (!isNaN(mb)) value = String(alignRamMb(mb));
    }

    if (field) {
        sendCmd('editVm', { vmIndex: row, field: field, value: value });
        if (field === 'networkMode' && value === '2' && currentDefaultAdapter) {
            sendCmd('editVm', { vmIndex: row, field: 'netAdapter', value: currentDefaultAdapter });
        }
    }
}

function cancelInlineEdit() {
    editingCell = null;
    renderVmTable();
}

/* ---- Force Stop VM ---- */

function onStopVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    if (vm.isTemplate) {
        showModal(
            'Cancel Template Build',
            'Stopping a template build will delete the incomplete template "' + vm.name + '".\n\nAre you sure?',
            'Stop & Delete'
        ).then(function(confirmed) {
            if (confirmed) {
                sendCmd('stopVm', { vmIndex: idx });
                sendCmd('deleteVm', { vmIndex: idx });
            }
        });
    } else {
        if (localStorage.getItem('suppress_force_stop_warn') === '1') {
            sendCmd('stopVm', { vmIndex: idx });
        } else {
            showForceStopModal(idx);
        }
    }
}

function showForceStopModal(idx) {
    document.getElementById('modal-title').textContent = 'Force Stop';
    document.getElementById('modal-message').textContent =
        'Force Stop will immediately power-off "' + vms[idx].name + '" which may result in corruption of its data.';
    document.getElementById('modal-confirm-btn').textContent = 'Force Stop';

    var cb = document.getElementById('modal-dont-show');
    if (cb) { cb.checked = false; cb.parentElement.style.display = ''; }

    document.getElementById('modal-overlay').classList.add('active');
    pendingConfirm = { resolve: function(confirmed) {
        if (confirmed) {
            if (cb && cb.checked) localStorage.setItem('suppress_force_stop_warn', '1');
            sendCmd('stopVm', { vmIndex: idx });
        }
        if (cb) cb.parentElement.style.display = 'none';
    }};
}

/* ---- Delete VM ---- */

function onDeleteVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete VM "' + vm.name + '"?\n\nThis will permanently delete all disk data and checkpoints.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteVm', { vmIndex: idx });
        }
    });
}

/* ---- Per-VM management (Windows host) ---- */

function currentCheckpointLabel(vm) {
    var snaps = vm.snapshots || [];
    var baseBranches = vm.baseBranches || [];
    if (vm.snapCurrent >= 0 && snaps[vm.snapCurrent]) {
        var label = snaps[vm.snapCurrent].name;
        var branches = snaps[vm.snapCurrent].branches || [];
        if (vm.snapCurrentBranch >= 0 && branches[vm.snapCurrentBranch])
            label += ' / ' + (branches[vm.snapCurrentBranch].name || 'branch ' + (vm.snapCurrentBranch + 1));
        return label;
    }
    if (vm.snapCurrent === -2) {
        var base = 'Base';
        if (vm.snapCurrentBranch >= 0 && baseBranches[vm.snapCurrentBranch])
            base += ' / ' + (baseBranches[vm.snapCurrentBranch].name || 'branch ' + (vm.snapCurrentBranch + 1));
        return base;
    }
    return 'Base';
}

function makeCheckpointSummaryCell(vm, vmIdx) {
    var td = document.createElement('td');
    td.className = 'checkpoint-summary-cell';
    var button = document.createElement('button');
    button.className = 'checkpoint-summary';
    var count = (vm.snapshots || []).length;
    button.textContent = count ? currentCheckpointLabel(vm) + ' (' + count + ')' : 'None';
    button.title = 'Open Manage to create, select, rename, or delete checkpoints and branches';
    button.onclick = function(e) { e.stopPropagation(); openManageVm(vmIdx); };
    td.appendChild(button);
    return td;
}

function openManageVm(idx) {
    if (hostBridge.isMac || !vms[idx]) return;
    manageVmIndex = idx;
    managePending = false;
    document.getElementById('manage-vm-overlay').classList.add('active');
    refreshManageVm(true);
}

function closeManageVm() {
    if (managePending) return;
    document.getElementById('manage-vm-overlay').classList.remove('active');
    manageVmIndex = -1;
}

function setManageDisabled(id, disabled) {
    var el = document.getElementById(id);
    if (el) el.disabled = !!disabled;
}

function populateManageCheckpointSelect(vm) {
    var select = document.getElementById('manage-checkpoint-select');
    var wanted = selectedSnap[manageVmIndex] || 'current';
    var values = [];
    select.innerHTML = '';
    function option(value, text) {
        var o = document.createElement('option');
        o.value = value; o.textContent = text; select.appendChild(o); values.push(value);
    }
    option('current', 'Current: ' + currentCheckpointLabel(vm));
    if (vm.hasSnapshots) {
        option('base', 'Base [create a new branch on Start]');
        (vm.baseBranches || []).forEach(function(br, i) {
            option('base-' + i, 'Base / ' + (br.name || 'branch ' + (i + 1)));
        });
        (vm.snapshots || []).forEach(function(cp, i) {
            option(String(i), cp.name + ' [create a new branch on Start]');
            (cp.branches || []).forEach(function(br, b) {
                option(i + '-' + b, cp.name + ' / ' + (br.name || 'branch ' + (b + 1)));
            });
        });
    }
    if (values.indexOf(wanted) < 0) wanted = 'current';
    select.value = wanted;
    selectedSnap[manageVmIndex] = wanted;
}

function refreshManageVm(forceFields) {
    var overlay = document.getElementById('manage-vm-overlay');
    if (!overlay || !overlay.classList.contains('active') || manageVmIndex < 0) return;
    var vm = vms[manageVmIndex];
    if (!vm) { closeManageVm(); return; }
    var stoppedIdle = !vm.running && !vm.buildingVhdx && !vm.managementBusy && !managePending;
    var hasChildren = (vm.snapshots || []).length > 0 || (vm.baseBranches || []).length > 0;
    document.getElementById('manage-vm-name').textContent = vm.name;
    document.getElementById('manage-state').textContent = managePending || vm.managementBusy
        ? 'A management operation is running. Do not close the app or move these files manually.'
        : (vm.running ? 'Stop the VM to change media, storage, disk size, or checkpoints.' : 'VM is stopped and ready to manage.');
    if (forceFields || document.activeElement !== document.getElementById('manage-iso-path'))
        document.getElementById('manage-iso-path').value = vm.imagePath || '';
    document.getElementById('manage-storage-current').value = vm.storageDir || '';
    document.getElementById('manage-disk-current').textContent = vm.hddGb;
    var sizeInput = document.getElementById('manage-disk-size');
    sizeInput.min = vm.hddGb + 1;
    if (forceFields || document.activeElement !== sizeInput) sizeInput.value = vm.hddGb + 1;
    document.getElementById('manage-auto-display').checked = !!vm.autoOpenDisplay;
    renderManageSharedResources(vm);
    populateManageCheckpointSelect(vm);

    ['manage-iso-attach','manage-iso-eject','manage-storage-move','manage-disk-grow',
     'manage-checkpoint-create','manage-checkpoint-rename','manage-checkpoint-delete']
        .forEach(function(id) { setManageDisabled(id, !stoppedIdle); });
    setManageDisabled('manage-disk-grow', !stoppedIdle || hasChildren);
    setManageDisabled('manage-auto-display', managePending);
    onManageCheckpointSelection();
}

function resourceIsExcluded(vm, id) {
    return (vm.sharedResourceExclusions || '').split(',').indexOf(id) >= 0;
}

function renderCreateSharedResources() {
    var box = document.getElementById('create-shared-resource-list');
    if (!box) return;
    box.innerHTML = '';
    sharedResources.filter(function(r) { return r.enabled; }).forEach(function(r) {
        var label=document.createElement('label'), cb=document.createElement('input');
        cb.type='checkbox';cb.checked=true;cb.dataset.resourceId=r.id;
        label.appendChild(cb);label.appendChild(document.createTextNode(' '+r.driveLetter+': '+r.name+(r.readOnly?' (read-only)':'')));
        box.appendChild(label);
    });
    if (!box.children.length) box.textContent='No global shared resources configured.';
}

function renderManageSharedResources(vm) {
    var box=document.getElementById('manage-shared-resource-list');
    if(!box)return;box.innerHTML='';
    sharedResources.filter(function(r){return r.enabled;}).forEach(function(r){
        var label=document.createElement('label'),cb=document.createElement('input');
        cb.type='checkbox';cb.checked=!resourceIsExcluded(vm,r.id);cb.disabled=!!vm.managementBusy||managePending;
        cb.onchange=function(){sendCmd('setVmSharedResource',{vmIndex:manageVmIndex,id:r.id,enabled:cb.checked});};
        label.appendChild(cb);label.appendChild(document.createTextNode(' '+r.driveLetter+': '+r.name+(r.readOnly?' (read-only)':'')));
        box.appendChild(label);
    });
    if(!box.children.length)box.textContent='No global shared resources configured.';
    if(vm.sharedResourceError){var err=document.createElement('span');err.className='field-warn';err.textContent=vm.sharedResourceError;box.appendChild(err);}
}

/* Global settings (not per-VM).
 *
 * The capture key is picked by pressing it rather than typing its name: the
 * host parser accepts a small vocabulary, and asking someone to spell
 * "ScrollLock" correctly is a worse experience than listening for the key.
 * Names produced here match what idd_parse_bind accepts.
 */
var captureBindListening = false;

var KEYBIND_NAMED = {
    Pause: 'Pause', ScrollLock: 'ScrollLock', NumLock: 'NumLock',
    Insert: 'Insert', Delete: 'Delete', Home: 'Home', End: 'End',
    PageUp: 'PageUp', PageDown: 'PageDown', Space: 'Space', Tab: 'Tab',
    Backspace: 'Backspace', CapsLock: 'CapsLock', ContextMenu: 'Apps'
};

/* Map a KeyboardEvent.code to a name the host understands, or null. */
function keybindNameFor(code) {
    if (Object.prototype.hasOwnProperty.call(KEYBIND_NAMED, code)) return KEYBIND_NAMED[code];
    if (code.length > 1 && code.charAt(0) === 'F') {
        var n = parseInt(code.substring(1), 10);
        if (n >= 1 && n <= 24 && String(n) === code.substring(1)) return code;
    }
    if (code.length === 4 && code.substring(0, 3) === 'Key') return code.charAt(3);
    if (code.length === 6 && code.substring(0, 5) === 'Digit') return code.charAt(5);
    return null;
}

function beginCaptureBind() {
    var el = document.getElementById('app-capture-hotkey');
    if (!el || captureBindListening) return;
    captureBindListening = true;
    el.dataset.previous = el.value;
    el.value = '';
    el.placeholder = 'Press a key...';
    el.classList.add('listening');
}

function cancelCaptureBind() {
    var el = document.getElementById('app-capture-hotkey');
    if (!el || !captureBindListening) return;
    captureBindListening = false;
    el.value = el.dataset.previous || '';
    el.placeholder = 'Click, then press a key';
    el.classList.remove('listening');
}

function onCaptureBindKey(e) {
    if (!captureBindListening) return;
    var el = document.getElementById('app-capture-hotkey');
    if (!el) return;

    e.preventDefault();
    e.stopPropagation();

    /* Esc backs out. Binding it would be hostile: it is the one key a stuck
       user reaches for, and every dialog here treats it as "get me out". */
    if (e.code === 'Escape') { cancelCaptureBind(); el.blur(); return; }

    /* A modifier on its own is the user still assembling the combination. */
    if (e.code === 'ControlLeft' || e.code === 'ControlRight' ||
        e.code === 'AltLeft' || e.code === 'AltRight' ||
        e.code === 'ShiftLeft' || e.code === 'ShiftRight' ||
        e.code === 'MetaLeft' || e.code === 'MetaRight') return;

    var name = keybindNameFor(e.code);
    if (!name) return;   /* not in the host vocabulary: keep listening */

    var text = (e.ctrlKey ? 'Ctrl+' : '') + (e.altKey ? 'Alt+' : '') +
               (e.shiftKey ? 'Shift+' : '') + name;

    captureBindListening = false;
    el.value = text;
    el.dataset.previous = text;
    el.placeholder = 'Click, then press a key';
    el.classList.remove('listening');
    el.blur();
}

document.addEventListener('keydown', onCaptureBindKey, true);

function openAppSettingsModal() {
    var el = document.getElementById('app-capture-hotkey');
    captureBindListening = false;
    el.classList.remove('listening');
    el.value = appSettings.captureHotkey || 'Pause';
    el.dataset.previous = el.value;
    el.placeholder = 'Click, then press a key';
    document.getElementById('app-settings-overlay').classList.add('active');
}

function closeAppSettingsModal() {
    captureBindListening = false;
    document.getElementById('app-settings-overlay').classList.remove('active');
}

function saveAppSettings() {
    var v = (document.getElementById('app-capture-hotkey').value || '').trim();
    if (!v) { showModal('Settings', 'Click the box and press the key you want to use.', 'OK'); return; }
    sendCmd('saveAppSettings', { captureHotkey: v });
    closeAppSettingsModal();
}

function openSettingsModal(){if(hostBridge.isMac)return;populateResourceLetters();clearResourceEditor();renderSharedAppliance();renderSettingsResources();document.getElementById('settings-overlay').classList.add('active');}
function closeSettingsModal(){document.getElementById('settings-overlay').classList.remove('active');}
function populateResourceLetters(){var guest=document.getElementById('resource-letter'),host=document.getElementById('resource-host-letter');if(!guest.options.length)for(var c=68;c<=90;c++){var o=document.createElement('option');o.value=String.fromCharCode(c);o.textContent=o.value+':';guest.appendChild(o);}if(host.options.length===1)for(var h=68;h<=90;h++){var x=document.createElement('option');x.value=String.fromCharCode(h);x.textContent=x.value+':';host.appendChild(x);}}
function clearResourceEditor(){document.getElementById('resource-id').value='';document.getElementById('resource-name').value='';document.getElementById('resource-letter').value='R';document.getElementById('resource-host-letter').value='';document.getElementById('resource-enabled').checked=true;document.getElementById('resource-readonly').checked=false;document.getElementById('resource-editor-title').textContent='Add shared resource';}
function editResource(id){var r=sharedResources.find(function(x){return x.id===id;});if(!r)return;populateResourceLetters();document.getElementById('resource-id').value=r.id;document.getElementById('resource-name').value=r.name;document.getElementById('resource-letter').value=r.driveLetter;document.getElementById('resource-host-letter').value=r.hostDriveLetter||'';document.getElementById('resource-enabled').checked=!!r.enabled;document.getElementById('resource-readonly').checked=!!r.readOnly;document.getElementById('resource-editor-title').textContent='Edit shared resource';}
function renderSettingsResources(){
    var box=document.getElementById('settings-resource-list');
    if(!box)return;
    box.innerHTML='';
    sharedResources.forEach(function(r){
        var row=document.createElement('div');
        row.className='settings-resource-row';
        var details=document.createElement('div');
        details.className='settings-resource-details';
        var name=document.createElement('span');
        name.className='settings-resource-name';
        name.textContent=r.driveLetter+': '+r.name;
        var meta=document.createElement('span');
        meta.className='settings-resource-meta';
        meta.textContent=(r.readOnly?'Read-only':'Read/write')+(r.enabled?'':' · Disabled')+(r.legacyHostPath?' · Legacy folder disconnected and untouched: '+r.legacyHostPath:'');
        details.append(name,meta);
        var actions=document.createElement('div');
        actions.className='settings-resource-actions';
        var edit=document.createElement('button');
        edit.type='button';edit.textContent='Edit';edit.title='Edit this shared resource definition.';
        edit.onclick=function(){editResource(r.id);};
        var mount=document.createElement('button');
        mount.type='button';mount.textContent='Mount Host';mount.title='Mount this resource on the host using its configured host drive letter.';
        mount.onclick=function(){mountHostResource(r.id);};
        var open=document.createElement('button');
        open.type='button';open.textContent='Open Share';open.title='Open this resource in File Explorer on the host.';
        open.onclick=function(){sendCmd('openHostResource',{id:r.id});};
        var unmount=document.createElement('button');
        unmount.type='button';unmount.textContent='Unmount';unmount.title='Unmount this resource from the host.';
        unmount.onclick=function(){sendCmd('unmountHostResource',{id:r.id});};
        var purge=document.createElement('button');
        purge.type='button';purge.className='danger';purge.textContent='Purge Data';purge.title='Permanently delete all data stored in this resource.';
        purge.onclick=function(){showModal('Permanently purge data','Permanently delete all appliance data for '+r.name+'? This cannot be undone.','Purge').then(function(ok){if(ok)sendCmd('purgeSharedResource',{id:r.id});});};
        var del=document.createElement('button');
        del.type='button';del.className='danger';del.textContent='Remove';del.title='Remove this resource definition while retaining its appliance data.';
        del.onclick=function(){showModal('Remove shared resource','Unpublish '+r.name+'? Its appliance directory will be retained.','Remove').then(function(ok){if(ok)sendCmd('deleteSharedResource',{id:r.id});});};
        actions.append(edit,mount,open,unmount,purge,del);
        row.append(details,actions);
        box.appendChild(row);
    });
    if(!box.children.length)box.textContent='No shared resources configured.';
}
function saveResourceEditor(){var payload={id:document.getElementById('resource-id').value,name:document.getElementById('resource-name').value.trim(),driveLetter:document.getElementById('resource-letter').value,hostDriveLetter:document.getElementById('resource-host-letter').value,enabled:document.getElementById('resource-enabled').checked,readOnly:document.getElementById('resource-readonly').checked};if(!payload.name){showModal('Shared resource','A unique name is required.','OK',{confirmClass:'primary'});return;}sendCmd('saveSharedResource',payload);}
function onSharedResourceResult(msg){if(!msg.success)showModal('Shared resource error',msg.message||'The resource operation failed. Check the appliance state, name, and drive-letter collisions.','OK',{confirmClass:'primary'});else clearResourceEditor();}

function renderSharedAppliance(){var s=sharedAppliance||{},el=document.getElementById('appliance-status');if(!el)return;el.textContent=(s.configured?(s.backend||'appliance'):'Not configured')+' — '+(s.progressText||['Unconfigured','Provisioning','Stopped','Starting','Ready','Updating','Failed','Stopping'][s.state]||'Unknown')+(s.lastError?' — '+s.lastError:'');if(s.storageRoot&&!document.getElementById('appliance-storage-parent').value)document.getElementById('appliance-storage-parent').value=s.storageRoot.replace(/[\\/][^\\/]+$/,'');if(s.dataSizeGb)document.getElementById('appliance-data-size').value=s.dataSizeGb;if(s.ramMb)document.getElementById('appliance-ram').value=s.ramMb;if(s.cpuCores)document.getElementById('appliance-cpu').value=s.cpuCores;if(s.backend)document.getElementById('appliance-backend').value=s.backend;}
function appliancePayload(){return{backend:document.getElementById('appliance-backend').value,storageParent:document.getElementById('appliance-storage-parent').value.trim(),dataSizeGb:Number(document.getElementById('appliance-data-size').value),adminUser:document.getElementById('appliance-admin-user').value.trim(),adminPassword:document.getElementById('appliance-admin-pass').value,ramMb:Number(document.getElementById('appliance-ram').value),cpuCores:Number(document.getElementById('appliance-cpu').value),windowsIsoPath:document.getElementById('appliance-server-iso').value.trim(),windowsImageName:document.getElementById('appliance-server-image').value.trim(),productKey:document.getElementById('appliance-product-key').value};}
function setupSharedAppliance(){sendCmd('setupSharedAppliance',appliancePayload());}
function startSharedAppliance(){sendCmd('startSharedAppliance');}
function stopSharedAppliance(){sendCmd('stopSharedAppliance');}
function updateSharedAppliance(){sendCmd('updateSharedAppliance');}
function growSharedAppliance(){sendCmd('growSharedAppliance',{dataSizeGb:Number(document.getElementById('appliance-data-size').value)});}
function rebuildSharedAppliance(){var p=appliancePayload();p.switchBackend=!!sharedAppliance.backend&&sharedAppliance.backend!==p.backend;showModal('Rebuild appliance',p.switchBackend?'Changing backend archives the old data VHDX and creates a new empty disk. Continue?':'Rebuild the appliance OS disk while preserving its data disk?','Rebuild').then(function(ok){if(ok)sendCmd('rebuildSharedAppliance',p);});}
function mountHostResource(id){sendCmd('mountHostResource',{id:id});}
function onSharedApplianceResult(msg){if(!msg.success)showModal('Shared appliance',msg.message||'The appliance operation failed.','OK');}

function onManageBrowseResult(msg) {
    if (manageVmIndex < 0) return;
    if (msg.kind === 'iso') document.getElementById('manage-iso-path').value = msg.path || '';
    if (msg.kind === 'storage') document.getElementById('manage-storage-parent').value = msg.path || '';
}

function onManageResult(msg) {
    managePending = false;
    if (!msg.success) showModal('Management error', msg.message || 'Operation failed.', 'OK');
    refreshManageVm(true);
}

function attachManageIso() {
    var path = document.getElementById('manage-iso-path').value.trim();
    if (!path || manageVmIndex < 0) return;
    managePending = true; refreshManageVm(false);
    sendCmd('setVmInstallerIso', {vmIndex: manageVmIndex, path: path});
}

function ejectManageIso() {
    if (manageVmIndex < 0) return;
    showModal('Eject installer ISO', 'Detach the installer ISO from this VM? The host file and resources.iso will not be deleted.', 'Eject', {confirmClass:'primary'}).then(function(ok) {
        if (!ok) return;
        managePending = true; refreshManageVm(false);
        sendCmd('setVmInstallerIso', {vmIndex: manageVmIndex, path: ''});
    });
}

function moveManageStorage() {
    var parent = document.getElementById('manage-storage-parent').value.trim();
    var vm = vms[manageVmIndex];
    if (!parent || !vm) return;
    showModal('Move managed storage', 'Copy and verify the complete VM folder under "' + parent + '", switch the configuration, then remove the old folder?', 'Move', {confirmClass:'primary'}).then(function(ok) {
        if (!ok) return;
        managePending = true; refreshManageVm(false);
        sendCmd('moveVmStorage', {vmIndex: manageVmIndex, destinationParent: parent});
    });
}

function growManageDisk() {
    var vm = vms[manageVmIndex];
    var size = parseInt(document.getElementById('manage-disk-size').value, 10);
    if (!vm || isNaN(size) || size <= vm.hddGb) return;
    showModal('Grow virtual disk', 'Grow the host VHDX from ' + vm.hddGb + ' GB to ' + size + ' GB? This cannot be undone. Guest expansion will be attempted once on next boot.', 'Grow', {confirmClass:'primary'}).then(function(ok) {
        if (!ok) return;
        managePending = true; refreshManageVm(false);
        sendCmd('resizeVmDisk', {vmIndex: manageVmIndex, sizeGb: size});
    });
}

function saveManageDisplay() {
    if (manageVmIndex < 0) return;
    var enabled = document.getElementById('manage-auto-display').checked;
    managePending = true; refreshManageVm(false);
    sendCmd('setVmAutoOpenDisplay', {
        vmIndex: manageVmIndex,
        enabled: enabled
    });
}

function onManageCheckpointSelection() {
    if (manageVmIndex < 0) return;
    var vm = vms[manageVmIndex];
    var select = document.getElementById('manage-checkpoint-select');
    if (!vm || !select) return;
    selectedSnap[manageVmIndex] = select.value;
    var p = parseSnapValue(select.value);
    var stoppedIdle = !vm.running && !vm.buildingVhdx && !vm.managementBusy && !managePending;
    var canDelete = stoppedIdle && p.snapIndex !== -1 && !(p.snapIndex === -2 && p.branchIndex < 0);
    if (p.snapIndex >= 0 && p.branchIndex < 0) {
        var cp = (vm.snapshots || [])[p.snapIndex];
        if (cp && (cp.branches || []).length) canDelete = false;
    }
    setManageDisabled('manage-checkpoint-delete', !canDelete);
    setManageDisabled('manage-checkpoint-rename', !stoppedIdle || p.snapIndex === -1 || (p.snapIndex === -2 && p.branchIndex < 0));
    renderVmTable();
}

function createManageCheckpoint() {
    var vm = vms[manageVmIndex];
    if (!vm) return;
    var name = 'Checkpoint ' + ((vm.snapshots || []).length + 1);
    showModal('New Checkpoint', 'Create a frozen checkpoint and its first working branch.', 'Create', {
        confirmClass:'primary', input:{label:'Checkpoint name:', value:name}
    }).then(function(result) {
        if (result === false) return;
        sendCmd('snapTake', {vmIndex:manageVmIndex, name:result});
    });
}

function deleteManageCheckpoint() {
    var vm = vms[manageVmIndex];
    var p = parseSnapValue(document.getElementById('manage-checkpoint-select').value);
    if (!vm) return;
    if (p.branchIndex >= 0) {
        showModal('Delete Branch', 'Delete this working branch?', 'Delete').then(function(ok) {
            if (ok) sendCmd('snapDeleteBranch', {vmIndex:manageVmIndex, snapIndex:p.snapIndex, branchIndex:p.branchIndex});
        });
    } else if (p.snapIndex >= 0) {
        var cp = (vm.snapshots || [])[p.snapIndex];
        if (cp && (cp.branches || []).length) {
            showModal('Checkpoint has branches', 'Delete every child branch before deleting this checkpoint.', 'OK');
            return;
        }
        showModal('Delete Checkpoint', 'Delete checkpoint "' + (cp ? cp.name : '') + '"?', 'Delete').then(function(ok) {
            if (ok) sendCmd('snapDelete', {vmIndex:manageVmIndex, snapIndex:p.snapIndex});
        });
    }
}

function renameManageCheckpoint() {
    var vm = vms[manageVmIndex];
    var p = parseSnapValue(document.getElementById('manage-checkpoint-select').value);
    var name = '';
    if (!vm) return;
    if (p.snapIndex === -2 && p.branchIndex >= 0)
        name = (vm.baseBranches[p.branchIndex] || {}).name || '';
    else if (p.snapIndex >= 0) {
        var cp = vm.snapshots[p.snapIndex];
        name = p.branchIndex >= 0 ? ((cp.branches[p.branchIndex] || {}).name || '') : cp.name;
    }
    showModal('Rename', 'Enter a new name:', 'Rename', {confirmClass:'primary', input:{label:'Name:', value:name}}).then(function(result) {
        if (result === false || result === name) return;
        var data = {vmIndex:manageVmIndex, snapIndex:p.snapIndex, name:result};
        if (p.branchIndex >= 0) data.branchIndex = p.branchIndex;
        sendCmd('snapRename', data);
    });
}

let manageBackdropPress = false;
document.getElementById('manage-vm-overlay').addEventListener('mousedown', function(e) {
    manageBackdropPress = (e.target === this);
});
document.getElementById('manage-vm-overlay').addEventListener('click', function(e) {
    if (e.target === this && manageBackdropPress) closeManageVm();
    manageBackdropPress = false;
});

/* ---- Checkpoints ---- */

/* Parse select value string into {snapIndex, branchIndex} */
function parseSnapValue(val) {
    if (!val || val === 'current') return {snapIndex: -1, branchIndex: -1};
    if (val === 'base') return {snapIndex: -2, branchIndex: -1};
    if (val.substring(0, 5) === 'base-') return {snapIndex: -2, branchIndex: parseInt(val.substring(5))};
    var parts = val.split('-');
    if (parts.length === 1) return {snapIndex: parseInt(parts[0]), branchIndex: -1};
    return {snapIndex: parseInt(parts[0]), branchIndex: parseInt(parts[1])};
}

function makeSnapCell(vm, vmIdx) {
    var td = document.createElement('td');
    td.className = 'snap-cell';
    var snaps = vm.snapshots || [];
    var baseBranches = vm.baseBranches || [];
    var curSnap = vm.snapCurrent;       /* -2=base, -1=pre-snapshot, >=0=snapshot index */
    var curBranch = vm.snapCurrentBranch; /* branch index or -1 */
    var hasSn = vm.hasSnapshots;
    var sel = selectedSnap[vmIdx] || 'current';

    var snapWrap = document.createElement('span');
    snapWrap.className = 'snap-wrap';

    var select = document.createElement('select');
    select.className = 'snap-select';
    select.disabled = vm.running;

    function addOpt(value, text, selected) {
        var o = document.createElement('option');
        o.value = value;
        o.textContent = text;
        if (selected) o.selected = true;
        select.appendChild(o);
    }

    if (!hasSn) {
        addOpt('current', 'No snapshots', true);
    } else {
        /*  Tree with multiple branches per node:
         *    Current (base, branch 1)
         *    \u251C Base                       <- new branch
         *    \u2502 \u251C branch 1 (date)     <- resume
         *    \u2502 \u2514 branch 2 (date)     <- resume
         *    \u251C Snapshot A (date)           <- new branch
         *    \u2502 \u2514 branch 1 (date)     <- resume
         *    \u2514 Snapshot B (date)           <- new branch
         */

        /* "Current" — resume whatever is active */
        addOpt('current', 'Current', sel === 'current');

        /* Base + its branches */
        addOpt('base', '\u251C Base [create new child branch]', sel === 'base');
        baseBranches.forEach(function(br, b) {
            var brChar = (b === baseBranches.length - 1) ? '\u2514' : '\u251C';
            var label = '\u2502\u00A0\u00A0' + brChar + ' ' + (br.name || 'branch ' + (b + 1));
            if (br.date) label += ' (' + br.date + ')';
            if (br.sizeGb) label += ' [' + br.sizeGb + ' GB]';
            addOpt('base-' + b, label, sel === 'base-' + b);
        });

        /* Snapshots + their branches */
        snaps.forEach(function(snap, i) {
            var isLast = (i === snaps.length - 1);
            var treePfx = isLast ? '\u2514 ' : '\u251C ';
            var contPfx = isLast ? '\u00A0\u00A0\u00A0' : '\u2502\u00A0\u00A0';
            var branches = snap.branches || [];

            addOpt(String(i), treePfx + snap.name + ' (' + snap.date + ') [create new child branch]', sel === String(i));

            branches.forEach(function(br, b) {
                var brChar = (b === branches.length - 1) ? '\u2514' : '\u251C';
                var label = contPfx + brChar + ' ' + (br.name || 'branch ' + (b + 1));
                if (br.date) label += ' (' + br.date + ')';
                if (br.sizeGb) label += ' [' + br.sizeGb + ' GB]';
                addOpt(i + '-' + b, label, sel === i + '-' + b);
            });
        });
    }

    select.onchange = function(e) {
        e.stopPropagation();
        selectedSnap[vmIdx] = select.value;
        renderVmTable();
    };
    snapWrap.appendChild(select);

    /* Chain overlay — shows selected path when dropdown is closed */
    if (hasSn) {
        var p = parseSnapValue(sel);
        var chainText = '';
        if (sel === 'current') {
            /* Show the currently active chain */
            if (curSnap >= 0 && snaps[curSnap]) {
                chainText = 'base \u2192 ' + snaps[curSnap].name;
                if (curBranch >= 0 && snaps[curSnap].branches && snaps[curSnap].branches[curBranch])
                    chainText += ' \u2192 ' + (snaps[curSnap].branches[curBranch].name || 'branch ' + (curBranch + 1));
            } else if (curSnap === -2) {
                chainText = 'base';
                if (curBranch >= 0 && baseBranches[curBranch])
                    chainText += ' \u2192 ' + (baseBranches[curBranch].name || 'branch ' + (curBranch + 1));
            } else {
                chainText = 'base';
            }
        } else if (p.snapIndex === -2) {
            chainText = 'base';
            if (p.branchIndex >= 0 && baseBranches[p.branchIndex])
                chainText += ' \u2192 ' + (baseBranches[p.branchIndex].name || 'branch ' + (p.branchIndex + 1));
            else
                chainText += ' [create new child branch]';
        } else if (p.snapIndex >= 0 && snaps[p.snapIndex]) {
            chainText = 'base \u2192 ' + snaps[p.snapIndex].name;
            if (p.branchIndex >= 0 && snaps[p.snapIndex].branches && snaps[p.snapIndex].branches[p.branchIndex])
                chainText += ' \u2192 ' + (snaps[p.snapIndex].branches[p.branchIndex].name || 'branch ' + (p.branchIndex + 1));
            else
                chainText += ' [create new child branch]';
        }
        var overlay = document.createElement('span');
        overlay.className = 'snap-overlay';
        overlay.textContent = chainText;
        snapWrap.appendChild(overlay);
    }
    td.appendChild(snapWrap);

    /* Take snapshot button — only when stopped */
    var takeBtn = document.createElement('button');
    takeBtn.className = 'snap-btn';
    takeBtn.textContent = '+';
    takeBtn.title = 'Take snapshot';
    takeBtn.disabled = vm.running;
    takeBtn.onclick = function(e) {
        e.stopPropagation();
        var defaultName = 'Snapshot ' + (snaps.length + 1);
        showModal('New Checkpoint', 'Create a new checkpoint of the base disk. Checkpoints are frozen points in time that you can create independent branches from.', 'Create', {
            confirmClass: 'primary',
            input: { label: 'Snapshot name:', value: defaultName }
        }).then(function(result) {
            if (result === false) return;
            sendCmd('snapTake', { vmIndex: vmIdx, name: result });
        });
    };
    td.appendChild(takeBtn);

    /* Delete button — context-sensitive */
    var parsed = parseSnapValue(sel);
    if (!vm.running && parsed.snapIndex >= 0) {
        var delBtn = document.createElement('button');
        delBtn.className = 'snap-btn danger';
        delBtn.textContent = '\u2715';

        if (parsed.branchIndex >= 0) {
            /* Delete a single branch */
            delBtn.title = 'Delete branch';
            delBtn.onclick = function(e) {
                e.stopPropagation();
                showModal('Delete Branch',
                    'Delete this branch? The snapshot will be kept.',
                    'Delete'
                ).then(function(confirmed) {
                    if (confirmed) {
                        sendCmd('snapDeleteBranch', { vmIndex: vmIdx, snapIndex: parsed.snapIndex, branchIndex: parsed.branchIndex });
                        selectedSnap[vmIdx] = 'current';
                    }
                });
            };
        } else {
            /* Delete entire snapshot + all branches */
            delBtn.title = 'Delete checkpoint';
            delBtn.onclick = function(e) {
                e.stopPropagation();
                var snapName = snaps[parsed.snapIndex] ? snaps[parsed.snapIndex].name : '';
                showModal('Delete Snapshot',
                    'Delete checkpoint "' + snapName + '" and all its branches?',
                    'Delete'
                ).then(function(confirmed) {
                    if (confirmed) {
                        sendCmd('snapDelete', { vmIndex: vmIdx, snapIndex: parsed.snapIndex });
                        selectedSnap[vmIdx] = 'current';
                    }
                });
            };
        }
        td.appendChild(delBtn);
    }

    /* Delete button for base branches */
    if (!vm.running && parsed.snapIndex === -2 && parsed.branchIndex >= 0) {
        var delBrBtn = document.createElement('button');
        delBrBtn.className = 'snap-btn danger';
        delBrBtn.textContent = '\u2715';
        delBrBtn.title = 'Delete base branch';
        delBrBtn.onclick = function(e) {
            e.stopPropagation();
            showModal('Delete Branch',
                'Delete this base branch?',
                'Delete'
            ).then(function(confirmed) {
                if (confirmed) {
                    sendCmd('snapDeleteBranch', { vmIndex: vmIdx, snapIndex: -2, branchIndex: parsed.branchIndex });
                    selectedSnap[vmIdx] = 'current';
                }
            });
        };
        td.appendChild(delBrBtn);
    }

    /* Rename button — when a snapshot or branch is selected */
    if (!vm.running && parsed.snapIndex !== -1) {
        var currentName = '';
        if (parsed.snapIndex === -2 && parsed.branchIndex >= 0 && baseBranches[parsed.branchIndex]) {
            currentName = baseBranches[parsed.branchIndex].name || '';
        } else if (parsed.snapIndex >= 0 && snaps[parsed.snapIndex]) {
            if (parsed.branchIndex >= 0) {
                var br = snaps[parsed.snapIndex].branches && snaps[parsed.snapIndex].branches[parsed.branchIndex];
                currentName = br ? br.name || '' : '';
            } else {
                currentName = snaps[parsed.snapIndex].name || '';
            }
        }
        if (currentName || parsed.snapIndex >= 0) {
            var renBtn = document.createElement('button');
            renBtn.className = 'snap-btn';
            renBtn.textContent = '\u270F';
            renBtn.title = 'Rename';
            renBtn.onclick = function(e) {
                e.stopPropagation();
                showModal('Rename', 'Enter a new name:', 'Rename', {
                    confirmClass: 'primary',
                    input: { label: 'Name:', value: currentName }
                }).then(function(result) {
                    if (result === false || result === currentName) return;
                    var cmd = { vmIndex: vmIdx, snapIndex: parsed.snapIndex, name: result };
                    if (parsed.branchIndex >= 0) cmd.branchIndex = parsed.branchIndex;
                    sendCmd('snapRename', cmd);
                });
            };
            td.appendChild(renBtn);
        }
    }

    return td;
}

/* ---- Log ---- */

function appendLog(msg) {
    var panel = document.getElementById('log-panel');
    var div = document.createElement('div');
    div.className = 'log-line';
    div.textContent = msg;
    panel.appendChild(div);
    panel.scrollTop = panel.scrollHeight;
}

/* ---- Prerequisite check ---- */

function onPrereqRequired() {
    document.getElementById('prereq-message').innerHTML =
        'App Sandbox requires the <strong>Virtual Machine Platform</strong> Windows feature to create and run VMs. This feature is not currently enabled.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Cancel</button>' +
        '<button class="primary" onclick="enableFeature()">Enable</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function onPrereqReboot() {
    document.getElementById('prereq-message').innerHTML =
        '<strong>Virtual Machine Platform</strong> has been enabled but a reboot is required before VMs can be created or started.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
        '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function enableFeature() {
    document.getElementById('prereq-message').innerHTML =
        'Enabling <strong>Virtual Machine Platform</strong>. This may take a minute...' +
        '<div class="prereq-progress"><div class="prereq-progress-bar" id="prereq-bar"></div></div>' +
        '<div class="prereq-pct" id="prereq-pct">0%</div>';
    document.getElementById('prereq-buttons').style.display = 'none';
    sendCmd('enableFeature');
}

function onPrereqProgress(msg) {
    var bar = document.getElementById('prereq-bar');
    var pctEl = document.getElementById('prereq-pct');
    if (bar) bar.style.width = msg.pct + '%';
    if (pctEl) pctEl.textContent = msg.pct + '%';
}

function onPrereqResult(msg) {
    if (msg.ok && !msg.reboot) {
        document.getElementById('prereq-overlay').classList.remove('active');
    } else if (msg.ok && msg.reboot) {
        document.getElementById('prereq-message').innerHTML =
            '<strong>Virtual Machine Platform</strong> has been enabled. A reboot is required for the change to take effect.';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
            '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
        document.getElementById('prereq-buttons').style.display = '';
    } else {
        document.getElementById('prereq-message').innerHTML =
            'Failed to enable <strong>Virtual Machine Platform</strong>.<br><br>' +
            'Try enabling it manually:<br>' +
            'Settings &gt; System &gt; Optional Features &gt; More Windows Features &gt; Virtual Machine Platform';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Close</button>';
        document.getElementById('prereq-buttons').style.display = '';
    }
}

/* ---- Modal ---- */

function showModal(title, message, confirmText, opts) {
    document.getElementById('modal-title').textContent = title;
    document.getElementById('modal-message').textContent = message;
    var confirmBtn = document.getElementById('modal-confirm-btn');
    confirmBtn.textContent = confirmText || 'Confirm';
    confirmBtn.className = (opts && opts.confirmClass) || 'danger';
    document.getElementById('modal-cancel-btn').textContent =
        (opts && opts.cancelText) || 'Cancel';
    var cb = document.getElementById('modal-dont-show');
    if (cb) cb.parentElement.style.display = 'none';
    var inputRow = document.getElementById('modal-input-row');
    var inputEl = document.getElementById('modal-input');
    if (opts && opts.input) {
        inputRow.style.display = 'block';
        inputEl.value = opts.input.value || '';
        if (opts.input.label) document.getElementById('modal-input-label').textContent = opts.input.label;
        inputEl.onkeydown = function(e) { if (e.key === 'Enter') modalResolve(true); };
        inputEl.oninput = function() {
            /* Strip characters that would break the INI-style .dat file */
            var clean = inputEl.value.replace(/[\n\r\t\[\]\\]/g, '');
            if (clean !== inputEl.value) inputEl.value = clean;
        };
        inputEl.maxLength = 127;
        setTimeout(function() { inputEl.select(); inputEl.focus(); }, 50);
    } else {
        inputRow.style.display = 'none';
    }
    document.getElementById('modal-overlay').classList.add('active');

    return new Promise(function(resolve) {
        pendingConfirm = { resolve: resolve, hasInput: !!(opts && opts.input) };
    });
}

function modalResolve(result) {
    document.getElementById('modal-overlay').classList.remove('active');
    if (pendingConfirm) {
        if (result && pendingConfirm.hasInput) {
            pendingConfirm.resolve(document.getElementById('modal-input').value);
        } else {
            pendingConfirm.resolve(result);
        }
        pendingConfirm = null;
    }
}

function onSharedDependencyUnavailable(msg) {
    showModal('Shared appliance unavailable',
        'The shared-storage appliance did not become ready. Retry the dependency, or start this VM without shared drives for this boot.',
        'Retry', { confirmClass: 'primary', cancelText: 'Start Without Shared Drives' })
        .then(function(retry) {
            sendCmd('startVm', {
                vmIndex: msg.vmIndex,
                snapIndex: msg.snapIndex,
                branchIndex: msg.branchIndex,
                branchName: msg.branchName || '',
                allowMissingSharedResources: !retry
            });
        });
}

/* ---- Minimum size reporting ---- */

function reportMinSize() {
    var minW = 0;

    /* Measure <table> elements directly — they always report true natural width */
    var tables = document.querySelectorAll('table');
    tables.forEach(function(t) {
        if (t.scrollWidth > minW) minW = t.scrollWidth;
    });

    /* Add wrapper border (2px) + body padding (24px) */
    minW += 28;

    /* Height: sum of all sections at minimum height (log just needs ~100px) */
    var minH = 0;
    var sections = document.querySelectorAll('section');
    sections.forEach(function(s, i) {
        if (i < sections.length - 1) {
            minH += s.scrollHeight + 12;
        } else {
            minH += 100;
        }
    });
    minH += 16;

    sendCmd('setMinSize', { width: minW, height: minH });
}

/* ---- Init ---- */
/* Signal to C that the UI is ready */
sendCmd('uiReady');

/* Report min size once layout is complete (covers case with no VMs) */
setTimeout(function() {
    if (!minSizeReported) {
        minSizeReported = true;
        reportMinSize();
    }
}, 300);
