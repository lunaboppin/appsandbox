/* Game Streaming dashboard: sign in, pick a game or the desktop, then browser view or
   Moonlight. The server does the work; this page starts a session and follows it. */
'use strict';

const $ = (id) => document.getElementById(id);

let catalog = [];
let picked = null;
let pollTimer = null;
let autoOpened = false;
let sessionMode = 'browser';

async function api(path, body) {
  const options = body === undefined
    ? { credentials: 'same-origin' }
    : { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body, credentials: 'same-origin' };
  const r = await fetch(path, options);
  const text = await r.text();
  if (!r.ok) {
    const error = new Error(text || 'Something went wrong.');
    error.status = r.status;
    throw error;
  }
  try { return JSON.parse(text); } catch (e) { return {}; }
}

function show(view) {
  $('signin').hidden = view !== 'signin';
  $('library').hidden = view !== 'library';
  $('session').hidden = view !== 'session';
  $('top').hidden = view === 'signin';
}

/* ---- Signing in ---- */

$('signin').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('signin-error').textContent = '';
  $('btn-signin').disabled = true;
  $('btn-signin').textContent = 'Checking...';
  const user = $('user').value.trim();
  try {
    await api('/api/app/login', user + '\n' + $('password').value);
    $('password').value = '';
    rememberName(user);
    await enter();
  } catch (err) {
    $('signin-error').textContent = err.message;
  }
  $('btn-signin').disabled = false;
  $('btn-signin').textContent = 'Sign in';
});

/* Only the name, to show who's signed in. */
function rememberName(user) {
  try { localStorage.setItem('asb-dash-user', user); } catch (e) { /* the page works without it */ }
}

function showWho() {
  let user = '';
  try { user = localStorage.getItem('asb-dash-user') || ''; } catch (e) { /* the page works without it */ }
  $('who').textContent = user;
}

$('btn-signout').addEventListener('click', async () => {
  clearTimeout(pollTimer);
  await api('/api/app/logout', '').catch(() => {});
  show('signin');
});

/* ---- The library ---- */

function tile(item) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'tile';
  let art;
  if (item.art) {
    art = document.createElement('img');
    art.src = '/api/app/art/' + encodeURIComponent(item.art);
    art.alt = '';
  } else {
    art = document.createElement('span');
    art.textContent = item.name.charAt(0).toUpperCase();
  }
  art.className = 'tile-art';
  const name = document.createElement('span');
  name.className = 'tile-name';
  name.textContent = item.name;
  const source = document.createElement('span');
  source.className = 'tile-source';
  source.textContent = item.source;
  button.append(art, name, source);
  button.addEventListener('click', () => choose(item));
  return button;
}

function renderLibrary() {
  const tiles = $('tiles');
  tiles.replaceChildren(tile({ id: 'desktop', name: 'Desktop', source: 'The whole VM', art: '' }));
  catalog.forEach((game) => tiles.append(tile(game)));
  $('library-note').textContent = catalog.length ? '' : 'No games were found in this VM, so only the desktop is here.';
}

function choose(item) {
  picked = item;
  $('choose-title').textContent = item.name;
  $('choose-error').textContent = '';
  $('choose-password').hidden = true;
  $('choose-pass').value = '';
  $('choose').showModal();
}

async function start(mode) {
  const password = $('choose-password').hidden ? '' : $('choose-pass').value;
  if (!$('choose-password').hidden && !password) {
    $('choose-error').textContent = 'Enter your password.';
    return;
  }
  $('btn-browser').disabled = $('btn-moonlight').disabled = true;
  $('choose-error').textContent = '';
  let body = mode + '\n' + picked.id;
  if (password) body += '\n' + password;
  try {
    await api('/api/app/session', body);
    $('choose-pass').value = '';
    $('choose').close();
    autoOpened = false;
    poll();
  } catch (err) {
    if (err.status === 401 && err.message === 'password') {
      $('choose-password').hidden = false;
      $('choose-pass').focus();
    } else if (err.status === 401) {
      $('choose').close();
      show('signin');
    } else {
      $('choose-error').textContent = err.message;
    }
  }
  $('btn-browser').disabled = $('btn-moonlight').disabled = false;
}

$('btn-browser').addEventListener('click', () => start('browser'));
$('btn-moonlight').addEventListener('click', () => start('moonlight'));

/* ---- The session ---- */

function itemName(id) {
  if (id === 'desktop') return 'Desktop';
  const game = catalog.find((g) => g.id === id);
  return game ? game.name : 'Your session';
}

function moonlightAddress(s) {
  const address = (s.moonlightAddresses || [])[0] || '';
  return s.moonlightPort === 47989 ? address : address + ':' + s.moonlightPort;
}

async function openBrowserView() {
  try {
    const r = await api('/api/app/session/open', '');
    location.assign(r.path);
  } catch (err) {
    $('session-error').textContent = err.message;
  }
}

function renderMode(ready) {
  $('session-browser').hidden = !ready || sessionMode !== 'browser';
  $('session-moonlight').hidden = !ready || sessionMode !== 'moonlight';
  $('btn-switch').hidden = !ready;
  $('btn-switch').textContent = sessionMode === 'browser' ? 'Use Moonlight instead' : 'Use browser view instead';
}

function renderSession(s) {
  const ready = s.state === 'ready';
  show('session');
  $('session-title').textContent = itemName(s.item);
  $('session-wait').hidden = s.state !== 'starting';
  $('session-error').textContent = s.state === 'failed' ? s.detail : '';
  $('session-detail').textContent = s.state === 'starting' ? s.detail + '...'
    : s.state === 'ending' ? 'Removing your VM...'
    : ready ? 'Your VM is ready.' : '';
  $('btn-end').textContent = s.state === 'failed' ? 'Start over' : 'End session';
  $('btn-end').hidden = s.state === 'ending';
  renderMode(ready);
  if (ready) $('moonlight-address').textContent = moonlightAddress(s);
}

async function poll() {
  clearTimeout(pollTimer);
  try {
    const s = await api('/api/app/session');
    if (s.state === 'none') {
      show('library');
    } else {
      if (s.state !== 'ready') autoOpened = false;
      else if (s.mode === 'browser' && !autoOpened && sessionMode === 'browser' && document.visibilityState === 'visible') {
        autoOpened = true;
        renderSession(s);
        openBrowserView();
        return;
      }
      if (s.state === 'starting') sessionMode = s.mode;
      renderSession(s);
    }
  } catch (err) {
    if (err.status === 401) { show('signin'); return; }
  }
  pollTimer = setTimeout(poll, 3000);
}

$('btn-open').addEventListener('click', openBrowserView);
$('btn-switch').addEventListener('click', () => {
  sessionMode = sessionMode === 'browser' ? 'moonlight' : 'browser';
  autoOpened = true;
  renderMode(true);
});
$('btn-end').addEventListener('click', async () => {
  $('btn-end').disabled = true;
  await api('/api/app/session/end', '').catch(() => {});
  $('btn-end').disabled = false;
  poll();
});

$('pair').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('pair-result').textContent = 'Pairing...';
  try {
    await api('/api/app/session/pair', $('pin').value);
    $('pair-result').textContent = 'Paired. Open Desktop in Moonlight.';
    $('pin').value = '';
  } catch (err) {
    $('pair-result').textContent = err.message;
  }
});

/* ---- Start ---- */

async function enter() {
  showWho();
  try {
    const list = await api('/api/app/catalog');
    catalog = Array.isArray(list) ? list : [];
  } catch (err) {
    if (err.status === 401) { show('signin'); return; }
    catalog = [];
  }
  renderLibrary();
  show('library');
  poll();
}

// Returning from browser view shouldn't send you straight back there.
autoOpened = document.referrer !== '' || performance.getEntriesByType('navigation')[0]?.type === 'back_forward';
api('/api/app/session')
  .then((s) => { if (s.mode) sessionMode = s.mode; return enter(); })
  .catch(() => show('signin'));
