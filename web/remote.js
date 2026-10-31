// Use a VM from a browser. See src/backend_win/vm_web.c for the other end.
'use strict';

const MSG = { video: 1, audio: 2, cursor: 3, cursorState: 4, clip: 5, status: 6,
              input: 0x10, resize: 0x11, clipOut: 0x12, keyRequest: 0x13 };
const INPUT = { move: 0, button: 1, wheel: 2, key: 3, rel: 4, hwheel: 5 };

const $ = (id) => document.getElementById(id);
const canvas = $('screen');
const ctx2d = canvas.getContext('2d', { alpha: false, desynchronized: true });

const vmName = decodeURIComponent(location.pathname.replace(/^\/vm\//, ''));
document.title = vmName + ' - App Sandbox';
$('bar-name').textContent = vmName;

let ws = null;
let everOpen = false;
let replaced = false;
let retryMs = 1000;
let decoder = null;
let decoderCodec = '';
let waitingForKey = true;
let timestamp = 0;
let frameW = 0;
let frameH = 0;
let drawn = 0;
let audioCtx = null;
let audioNode = null;
let audioGain = null;
let audioFormat = '';
let muted = false;
try { muted = localStorage.getItem('asb-muted') === '1'; } catch (e) { /* storage blocked */ }
let lastClip = null;
let cursorCss = 'default';
let cursorVisible = true;
let lastShape = null;
const heldKeys = new Set();
const heldButtons = new Set();

/* ---- Messages on the page ---- */

function panel(title, text, opts) {
  opts = opts || {};
  $('overlay').hidden = false;
  $('panel-title').textContent = title;
  $('panel-text').textContent = text || '';
  $('login').hidden = !opts.login;
  $('btn-retry').hidden = !opts.retry;
  if (opts.login) $('key').focus();
}

function hidePanel() {
  $('overlay').hidden = true;
  canvas.focus();
}

/* ---- Signing in. The key arrives in the URL fragment, which is never sent to a
   server; it is exchanged for a cookie, then wiped from the address bar. ---- */

async function login(key) {
  const r = await fetch('/api/login', {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: vmName + '\n' + key,
    credentials: 'same-origin',
  });
  if (r.ok) return null;
  return (await r.text()) || 'That access key was not accepted.';
}

async function start() {
  if (!window.isSecureContext) {
    panel('This address is not secure',
      'Browsers only allow video decoding and keyboard capture on HTTPS or 127.0.0.1. Open the link the app shows.');
    return;
  }
  if (!('VideoDecoder' in window)) {
    panel('This browser can\'t show the VM',
      'It has no WebCodecs video decoder. Use a current version of Edge, Chrome, Firefox or Safari.');
    return;
  }
  const match = location.hash.match(/^#k=([0-9a-f]{64})$/);
  history.replaceState(null, '', location.pathname);
  if (match) {
    const error = await login(match[1]).catch((e) => String(e));
    if (error) {
      panel('Sign in', error, { login: true });
      return;
    }
  }
  connect();
}

// A fresh link opened in a tab that's already on this page.
window.addEventListener('hashchange', () => {
  if (location.hash) start();
});

$('login').addEventListener('submit', async (e) => {
  e.preventDefault();
  const key = $('key').value.trim();
  const error = await login(key).catch((err) => String(err));
  $('key').value = '';
  if (error) {
    panel('Sign in', error, { login: true });
    return;
  }
  connect();
});

$('btn-retry').addEventListener('click', () => {
  replaced = false;
  connect();
});

/* ---- Connection ---- */

function connect() {
  panel('Connecting', vmName);
  const scheme = location.protocol === 'https:' ? 'wss://' : 'ws://';
  const socket = new WebSocket(scheme + location.host + '/ws/' + encodeURIComponent(vmName));
  socket.binaryType = 'arraybuffer';
  let opened = false;
  ws = socket;

  socket.onopen = () => {
    opened = true;
    everOpen = true;
    retryMs = 1000;
    waitingForKey = true;
    panel('Waiting for the VM', 'The picture appears as soon as the VM has something to show.');
    sendResize();
    send(new Uint8Array([MSG.keyRequest]));
  };
  socket.onmessage = (e) => onMessage(e.data);
  socket.onclose = () => {
    ws = null;
    closeDecoder();
    if (replaced) return;
    if (!opened && !everOpen) {
      panel('Sign in', 'Enter this VM\'s access key. It\'s in the App Sandbox window, under Manage > Remote.', { login: true });
      return;
    }
    if (!opened && retryMs >= 8000) {
      // The server keeps refusing: most likely access was turned off, or the key changed.
      everOpen = false;
      panel('Sign in', 'Browser access was turned off or its key changed. Enter the current access key.', { login: true });
      return;
    }
    panel('Reconnecting', 'The connection to App Sandbox was lost.');
    setTimeout(connect, retryMs);
    retryMs = Math.min(retryMs * 2, 8000);
  };
}

function send(data) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(data);
}

function sendInput(type, p1, p2, p3) {
  const b = new ArrayBuffer(17);
  const d = new DataView(b);
  d.setUint8(0, MSG.input);
  d.setUint32(1, type, true);
  d.setUint32(5, p1 >>> 0, true);
  d.setUint32(9, p2 >>> 0, true);
  d.setUint32(13, (p3 || 0) >>> 0, true);
  send(b);
}

function onMessage(data) {
  const view = new DataView(data);
  switch (view.getUint8(0)) {
    case MSG.video: onVideo(view, data); break;
    case MSG.audio: onAudio(view, data); break;
    case MSG.cursor: onCursorShape(view, data); break;
    case MSG.cursorState:
      cursorVisible = view.getUint32(1, true) !== 0;
      applyCursor();
      break;
    case MSG.clip: onGuestClipboard(new TextDecoder().decode(new Uint8Array(data, 1))); break;
    case MSG.status: onStatus(JSON.parse(new TextDecoder().decode(new Uint8Array(data, 1)))); break;
  }
}

function onStatus(s) {
  if (s.replaced) {
    replaced = true;
    panel('Opened somewhere else', 'This VM is now open in another browser window.', { retry: true });
    return;
  }
  if (s.error) {
    panel('Can\'t show the VM', s.error, { retry: true });
    return;
  }
  const parts = [];
  if (!s.running) {
    parts.push('VM is off');
  } else if (s.width) {
    parts.push(s.width + '×' + s.height);
    parts.push(s.encoder === 'gpu' ? 'GPU encoding' : s.encoder === 'software' ? 'software encoding' : 'starting');
    parts.push(drawn / 2 + ' fps');
  }
  drawn = 0;
  $('bar-status').textContent = parts.join(' · ');
  if (!s.running && !$('overlay').hidden) {
    panel('The VM is off', 'The picture appears here when it starts.');
  } else if (s.running && !s.width && !$('overlay').hidden) {
    panel('Waiting for the VM', s.online ? 'Connecting to its display.' : 'Windows is still starting.');
  }
}

/* ---- Video ---- */

// avc1.PPCCLL comes from the SPS at the start of a key frame.
function codecFromKeyFrame(bytes) {
  for (let i = 0; i + 8 < bytes.length; i++) {
    if (bytes[i] === 0 && bytes[i + 1] === 0 && bytes[i + 2] === 1) {
      if ((bytes[i + 3] & 0x1f) === 7) {
        const hex = (n) => n.toString(16).padStart(2, '0');
        return 'avc1.' + hex(bytes[i + 4]) + hex(bytes[i + 5]) + hex(bytes[i + 6]);
      }
      i += 3;
    }
  }
  return null;
}

function closeDecoder() {
  if (decoder && decoder.state !== 'closed') {
    try { decoder.close(); } catch (e) { /* already closing */ }
  }
  decoder = null;
  decoderCodec = '';
}

function decoderFailed(e) {
  console.warn('video decoder', e);
  closeDecoder();
  waitingForKey = true;
  send(new Uint8Array([MSG.keyRequest]));
}

function onFrame(frame) {
  if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
    canvas.width = frame.displayWidth;
    canvas.height = frame.displayHeight;
    frameW = frame.displayWidth;
    frameH = frame.displayHeight;
    if (lastShape) drawCursorShape();
  }
  ctx2d.drawImage(frame, 0, 0);
  frame.close();
  drawn++;
  if (!$('overlay').hidden && !$('overlay').dataset.sticky) hidePanel();
}

function onVideo(view, data) {
  const key = view.getUint8(1) !== 0;
  const w = view.getUint32(2, true);
  const h = view.getUint32(6, true);
  const body = new Uint8Array(data, 10);
  if (waitingForKey && !key) return;
  if (key) {
    const codec = codecFromKeyFrame(body);
    if (!codec) {
      if (waitingForKey) return;
    } else if (!decoder || codec !== decoderCodec || decoder.codedWidthSeen !== w || decoder.codedHeightSeen !== h) {
      closeDecoder();
      decoder = new VideoDecoder({ output: onFrame, error: decoderFailed });
      decoder.configure({
        codec,
        codedWidth: w,
        codedHeight: h,
        optimizeForLatency: true,
        hardwareAcceleration: 'prefer-hardware',
        // Host's conversion is full-range BT.601.
        colorSpace: { primaries: 'smpte170m', transfer: 'bt709', matrix: 'smpte170m', fullRange: true },
      });
      decoder.codedWidthSeen = w;
      decoder.codedHeightSeen = h;
      decoderCodec = codec;
    }
    waitingForKey = false;
  }
  if (!decoder) return;
  timestamp += 16667;
  try {
    decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp, data: body }));
  } catch (e) {
    decoderFailed(e);
  }
}

/* ---- Sound. Browsers only start audio after a click or key press. ---- */

async function setupAudio(rate, channels) {
  const format = rate + '/' + channels;
  if (audioFormat === format) return;
  audioFormat = format;
  if (audioCtx) audioCtx.close();
  audioNode = null;
  audioCtx = new AudioContext({ sampleRate: rate, latencyHint: 'interactive' });
  await audioCtx.audioWorklet.addModule('/remote-audio.js');
  if (audioFormat !== format) return;
  audioNode = new AudioWorkletNode(audioCtx, 'remote-audio', {
    numberOfInputs: 0,
    outputChannelCount: [2],
    processorOptions: { channels },
  });
  audioGain = audioCtx.createGain();
  audioGain.gain.value = muted ? 0 : 1;
  audioNode.connect(audioGain);
  audioGain.connect(audioCtx.destination);
}

function showMuted() {
  const b = $('btn-mute');
  b.textContent = muted ? 'Unmute' : 'Mute';
  b.setAttribute('aria-pressed', String(muted));
  if (audioGain) audioGain.gain.value = muted ? 0 : 1;
}

$('btn-mute').addEventListener('click', () => {
  muted = !muted;
  try { localStorage.setItem('asb-muted', muted ? '1' : '0'); } catch (e) { /* just this visit, then */ }
  showMuted();
  canvas.focus();
});
showMuted();

function onAudio(view, data) {
  const rate = view.getUint32(1, true);
  const channels = view.getUint8(5);
  setupAudio(rate, channels).catch((e) => console.warn('audio', e));
  if (!audioNode) return;
  const samples = new Int16Array(data.slice(6));
  audioNode.port.postMessage(samples, [samples.buffer]);
}

function resumeAudio() {
  if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
}

/* ---- Cursor ---- */

function onCursorShape(view, data) {
  lastShape = {
    w: view.getUint32(1, true),
    h: view.getUint32(5, true),
    hotX: view.getUint32(9, true),
    hotY: view.getUint32(13, true),
    rgba: new Uint8ClampedArray(data.slice(17)),
  };
  drawCursorShape();
}

// A guest pixel is smaller or larger than a CSS pixel when the window doesn't match
// the VM's size, and browsers size CSS cursors in CSS pixels.
function drawCursorShape() {
  const s = lastShape;
  const scale = frameW ? Math.min(canvas.clientWidth / frameW, canvas.clientHeight / frameH) : 1;
  const w = Math.max(1, Math.round(s.w * scale));
  const h = Math.max(1, Math.round(s.h * scale));
  const source = document.createElement('canvas');
  source.width = s.w;
  source.height = s.h;
  source.getContext('2d').putImageData(new ImageData(s.rgba, s.w, s.h), 0, 0);
  const sized = document.createElement('canvas');
  sized.width = w;
  sized.height = h;
  sized.getContext('2d').drawImage(source, 0, 0, w, h);
  cursorCss = 'url(' + sized.toDataURL() + ') ' + Math.round(s.hotX * scale) + ' ' + Math.round(s.hotY * scale) + ', default';
  applyCursor();
}

function applyCursor() {
  canvas.style.cursor = cursorVisible ? cursorCss : 'none';
}

/* ---- Mouse ---- */

function toGuest(e) {
  const r = canvas.getBoundingClientRect();
  if (!frameW || !frameH || !r.width || !r.height) return null;
  const scale = Math.min(r.width / frameW, r.height / frameH);
  const x = (e.clientX - r.left - (r.width - frameW * scale) / 2) / scale;
  const y = (e.clientY - r.top - (r.height - frameH * scale) / 2) / scale;
  return {
    x: Math.max(0, Math.min(frameW - 1, Math.round(x))),
    y: Math.max(0, Math.min(frameH - 1, Math.round(y))),
  };
}

const locked = () => document.pointerLockElement === canvas;
const BUTTONS = [0, 2, 1, 3, 4];   // browser's left, middle, right, back, forward

function sendMove(e) {
  if (locked()) {
    if (e.movementX || e.movementY) sendInput(INPUT.rel, e.movementX | 0, e.movementY | 0);
    return;
  }
  const p = toGuest(e);
  if (p) sendInput(INPUT.move, p.x, p.y);
}

canvas.addEventListener('pointermove', (e) => {
  const moves = e.getCoalescedEvents ? e.getCoalescedEvents() : [];
  if (locked() && moves.length) moves.forEach(sendMove);
  else sendMove(e);
});

canvas.addEventListener('pointerdown', (e) => {
  e.preventDefault();
  canvas.focus();
  resumeAudio();
  const button = BUTTONS[e.button];
  if (button === undefined) return;
  canvas.setPointerCapture(e.pointerId);
  sendMove(e);
  heldButtons.add(button);
  sendInput(INPUT.button, button, 1);
});

canvas.addEventListener('pointerup', (e) => {
  const button = BUTTONS[e.button];
  if (button === undefined) return;
  sendMove(e);
  heldButtons.delete(button);
  sendInput(INPUT.button, button, 0);
});

canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('auxclick', (e) => e.preventDefault());

let wheelY = 0;
let wheelX = 0;
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  // Lines and pixels to Windows wheel units (120 a notch; browsers report ~100 pixels a notch).
  const unit = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 120 : 1.2;
  wheelY += -e.deltaY * unit;
  wheelX += e.deltaX * unit;
  const y = Math.trunc(wheelY);
  const x = Math.trunc(wheelX);
  if (y) { sendInput(INPUT.wheel, y, 0); wheelY -= y; }
  if (x) { sendInput(INPUT.hwheel, x, 0); wheelX -= x; }
}, { passive: false });

$('btn-capture').addEventListener('click', () => {
  canvas.focus();
  resumeAudio();
  const p = canvas.requestPointerLock({ unadjustedMovement: true });
  if (p && p.catch) p.catch(() => canvas.requestPointerLock());
});

/* ---- Keyboard: physical keys, by event.code, as [virtual key, scan code, extended]. ---- */

const KEYS = {
  Escape: [0x1B, 0x01], Digit1: [0x31, 0x02], Digit2: [0x32, 0x03], Digit3: [0x33, 0x04], Digit4: [0x34, 0x05],
  Digit5: [0x35, 0x06], Digit6: [0x36, 0x07], Digit7: [0x37, 0x08], Digit8: [0x38, 0x09], Digit9: [0x39, 0x0A],
  Digit0: [0x30, 0x0B], Minus: [0xBD, 0x0C], Equal: [0xBB, 0x0D], Backspace: [0x08, 0x0E], Tab: [0x09, 0x0F],
  KeyQ: [0x51, 0x10], KeyW: [0x57, 0x11], KeyE: [0x45, 0x12], KeyR: [0x52, 0x13], KeyT: [0x54, 0x14],
  KeyY: [0x59, 0x15], KeyU: [0x55, 0x16], KeyI: [0x49, 0x17], KeyO: [0x4F, 0x18], KeyP: [0x50, 0x19],
  BracketLeft: [0xDB, 0x1A], BracketRight: [0xDD, 0x1B], Enter: [0x0D, 0x1C], ControlLeft: [0xA2, 0x1D],
  KeyA: [0x41, 0x1E], KeyS: [0x53, 0x1F], KeyD: [0x44, 0x20], KeyF: [0x46, 0x21], KeyG: [0x47, 0x22],
  KeyH: [0x48, 0x23], KeyJ: [0x4A, 0x24], KeyK: [0x4B, 0x25], KeyL: [0x4C, 0x26], Semicolon: [0xBA, 0x27],
  Quote: [0xDE, 0x28], Backquote: [0xC0, 0x29], ShiftLeft: [0xA0, 0x2A], Backslash: [0xDC, 0x2B],
  KeyZ: [0x5A, 0x2C], KeyX: [0x58, 0x2D], KeyC: [0x43, 0x2E], KeyV: [0x56, 0x2F], KeyB: [0x42, 0x30],
  KeyN: [0x4E, 0x31], KeyM: [0x4D, 0x32], Comma: [0xBC, 0x33], Period: [0xBE, 0x34], Slash: [0xBF, 0x35],
  ShiftRight: [0xA1, 0x36], NumpadMultiply: [0x6A, 0x37], AltLeft: [0xA4, 0x38], Space: [0x20, 0x39],
  CapsLock: [0x14, 0x3A], F1: [0x70, 0x3B], F2: [0x71, 0x3C], F3: [0x72, 0x3D], F4: [0x73, 0x3E],
  F5: [0x74, 0x3F], F6: [0x75, 0x40], F7: [0x76, 0x41], F8: [0x77, 0x42], F9: [0x78, 0x43], F10: [0x79, 0x44],
  NumLock: [0x90, 0x45], ScrollLock: [0x91, 0x46], Numpad7: [0x67, 0x47], Numpad8: [0x68, 0x48],
  Numpad9: [0x69, 0x49], NumpadSubtract: [0x6D, 0x4A], Numpad4: [0x64, 0x4B], Numpad5: [0x65, 0x4C],
  Numpad6: [0x66, 0x4D], NumpadAdd: [0x6B, 0x4E], Numpad1: [0x61, 0x4F], Numpad2: [0x62, 0x50],
  Numpad3: [0x63, 0x51], Numpad0: [0x60, 0x52], NumpadDecimal: [0x6E, 0x53], IntlBackslash: [0xE2, 0x56],
  F11: [0x7A, 0x57], F12: [0x7B, 0x58], IntlRo: [0xC1, 0x73], IntlYen: [0xDC, 0x7D],
  Pause: [0x13, 0x45, 0], PrintScreen: [0x2C, 0x37, 1],
  NumpadEnter: [0x0D, 0x1C, 1], ControlRight: [0xA3, 0x1D, 1], NumpadDivide: [0x6F, 0x35, 1],
  AltRight: [0xA5, 0x38, 1], Home: [0x24, 0x47, 1], ArrowUp: [0x26, 0x48, 1], PageUp: [0x21, 0x49, 1],
  ArrowLeft: [0x25, 0x4B, 1], ArrowRight: [0x27, 0x4D, 1], End: [0x23, 0x4F, 1], ArrowDown: [0x28, 0x50, 1],
  PageDown: [0x22, 0x51, 1], Insert: [0x2D, 0x52, 1], Delete: [0x2E, 0x53, 1],
  MetaLeft: [0x5B, 0x5B, 1], MetaRight: [0x5C, 0x5C, 1], ContextMenu: [0x5D, 0x5D, 1],
};

function sendKey(code, up) {
  const k = KEYS[code];
  if (!k) return;
  sendInput(INPUT.key, k[0], k[1], (k[2] ? 1 : 0) | (up ? 2 : 0));
}

canvas.addEventListener('keydown', (e) => {
  e.preventDefault();
  resumeAudio();
  heldKeys.add(e.code);
  sendKey(e.code, false);
});

canvas.addEventListener('keyup', (e) => {
  e.preventDefault();
  heldKeys.delete(e.code);
  sendKey(e.code, true);
});

// Nothing stays held down in the VM when this page loses the keyboard.
function releaseAll() {
  heldKeys.forEach((code) => sendKey(code, true));
  heldKeys.clear();
  heldButtons.forEach((b) => sendInput(INPUT.button, b, 0));
  heldButtons.clear();
}
window.addEventListener('blur', releaseAll);
canvas.addEventListener('blur', releaseAll);
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });

$('btn-fullscreen').addEventListener('click', async () => {
  if (document.fullscreenElement) {
    document.exitFullscreen();
    return;
  }
  await document.documentElement.requestFullscreen();
  canvas.focus();
});

// In full screen a browser that has the Keyboard Lock API sends Alt+Tab, the Windows
// key and so on here instead of acting on them. Holding Esc leaves full screen.
document.addEventListener('fullscreenchange', () => {
  if (!navigator.keyboard || !navigator.keyboard.lock) return;
  if (document.fullscreenElement) navigator.keyboard.lock().catch((e) => console.warn('keyboard lock', e));
  else navigator.keyboard.unlock();
});

/* ---- Clipboard. Text only, between this browser and the VM. Browsers may ask
   first, and only allow it while the page has focus. ---- */

function onGuestClipboard(text) {
  lastClip = text;
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).catch(() => { /* not focused, or not allowed */ });
  }
}

async function pushClipboard() {
  if (!navigator.clipboard || !navigator.clipboard.readText) return;
  try {
    const text = await navigator.clipboard.readText();
    if (text && text !== lastClip) {
      lastClip = text;
      const bytes = new TextEncoder().encode(text);
      const out = new Uint8Array(bytes.length + 1);
      out[0] = MSG.clipOut;
      out.set(bytes, 1);
      if (out.length <= 2 * 1024 * 1024) send(out);
    }
  } catch (e) { /* not allowed: copy and paste just stay separate */ }
}
window.addEventListener('focus', pushClipboard);
canvas.addEventListener('focus', pushClipboard);

/* ---- The VM's resolution follows the window. ---- */

let resizeTimer = 0;
let sentW = 0;
let sentH = 0;

function sendResize() {
  const scale = window.devicePixelRatio || 1;
  const w = Math.round(canvas.clientWidth * scale) & ~1;
  const h = Math.round(canvas.clientHeight * scale) & ~1;
  if (!w || !h) return;
  sentW = w;
  sentH = h;
  const b = new ArrayBuffer(9);
  const d = new DataView(b);
  d.setUint8(0, MSG.resize);
  d.setUint32(1, w, true);
  d.setUint32(5, h, true);
  send(b);
}

new ResizeObserver(() => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    sendResize();
    if (lastShape) drawCursorShape();
  }, 400);
}).observe(canvas);

start();
