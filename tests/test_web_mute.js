const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

let click;
let refresh;
let serviceState = 'IDLE';
let sessionAvailable = true;
const posts = [];
const elements = new Map();
const muteButton = { disabled: true, addEventListener: (_, handler) => { click = handler; } };
const syncButton = { disabled: true, addEventListener() {} };
const sourceButtons = [];

function element(id) {
  if (id === 'mute-button') return muteButton;
  if (id === 'sync-button') return syncButton;
  if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
  return elements.get(id);
}

const context = {
  AbortController, setTimeout, clearTimeout,
  document: { getElementById: element, querySelectorAll: () => sourceButtons },
  setInterval: (handler) => { refresh = handler; },
  fetch: async (url, options) => {
    if (url === '/api/v1/status') return {
      ok: true, json: async () => ({ gateway: 'online', xs8_service: 'connected',
        status: { state: serviceState, monitor: 'HEALTHY', stop_owed: '0',
          context_may_be_open: '0', last_byte: 'NONE', last_gpio_tx: 'NONE',
          last_waveform: 'NOT_OBSERVED' } }),
    };
    if (url === '/api/v1/session') return {
      ok: sessionAvailable,
      json: async () => ({ csrf_token: 'test-token', mute_available: sessionAvailable && serviceState === 'IDLE' }),
    };
    if (url === '/api/v1/capabilities') return { ok: true,
      json: async () => ({ operations: { source: { enabled: false, available_sources: [] } } }) };
    if (url === '/api/v1/mute') {
      posts.push(options);
      return { ok: true, json: async () => ({ result: 'SERVICE_TX_OK', receiver_execution: 'UNVERIFIED' }) };
    }
    throw new Error(`Unexpected request: ${url}`);
  },
};

async function settle() { await new Promise(setImmediate); }

async function main() {
  const html = fs.readFileSync('web/index.html', 'utf8');
  assert.match(html, /id="mute-button"[^>]*disabled/);
  assert.match(html, /<script src="\/app\.js" defer><\/script>/);
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(posts.length, 0, 'page load must not send MUTE');
  assert.equal(muteButton.disabled, false, 'eligible service and session enable MUTE');

  const first = click();
  const duplicate = click();
  await Promise.all([first, duplicate]);
  assert.equal(posts.length, 1, 'one press dispatches one POST');
  assert.equal(posts[0].body, '{}');
  assert.equal(posts[0].headers['X-CSRF-Token'], 'test-token');

  serviceState = 'BOOT_INHIBIT';
  refresh();
  await settle();
  assert.equal(muteButton.disabled, true, 'ineligible service disables MUTE');
  sessionAvailable = false;
  serviceState = 'IDLE';
  refresh();
  await settle();
  assert.equal(muteButton.disabled, true, 'missing session disables MUTE');
  assert.equal(posts.length, 1, 'refresh never sends MUTE');
  process.stdout.write('Web MUTE offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
