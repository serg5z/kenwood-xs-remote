const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('web/index.html', 'utf8');
const css = fs.readFileSync('web/app.css', 'utf8');
const names = ['CD', 'TUNER', 'PHONO', 'TV1', 'TV2', 'TAPE1', 'TAPE2'];
for (const name of names) assert.match(html, new RegExp(`data-source="${name}"`));
assert.match(html, /data-source="TAPE2"><span>TAPE2<\/span><small>MONITOR<\/small>/);
assert.match(css, /\.source-grid button\[data-source="TAPE2"\] small/);
assert.match(css, /button\[data-source="TAPE2"\] \{ position:relative; display:flex; align-items:center; justify-content:center; \}/);
assert.match(css, /\.source-grid button \{ height: 54px; \}/);
assert.match(css, /\.source-grid button\{height:52px\}/);

const posts = [];
const buttons = names.map((source) => ({
  dataset: { source }, disabled: true,
  addEventListener(_event, callback) { this.click = callback; },
}));
const elements = new Map();
const mute = { disabled: true, addEventListener() {} };
const sync = { disabled: true, addEventListener() {} };
let state = 'IDLE';
let sourceEnabled = true;
let now = 10000;
let refresh;
const context = {
  AbortController, setTimeout, clearTimeout,
  document: {
    getElementById(id) {
      if (id === 'mute-button') return mute;
      if (id === 'sync-button') return sync;
      if (!elements.has(id)) elements.set(id, { textContent: '' });
      return elements.get(id);
    },
    querySelectorAll: () => buttons,
  },
  Date: { now: () => now },
  setInterval: (handler) => { refresh = handler; },
  fetch: async (url, options) => {
    if (url === '/api/v1/status') return { ok: true, json: async () => ({
      gateway: 'online', xs8_service: 'connected', status: {
        state, monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
        last_byte: 'NONE', last_gpio_tx: 'NONE', last_waveform: 'NOT_OBSERVED',
      },
    }) };
    if (url === '/api/v1/session') return { ok: true, json: async () => ({
      csrf_token: 'test-token', mute_available: false, source_available: sourceEnabled,
    }) };
    if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
      operations: { source: { enabled: sourceEnabled,
        available_sources: ['CD', 'TUNER', 'PHONO', 'TAPE1'] } },
    }) };
    if (url === '/api/v1/source') {
      posts.push(options);
      return { ok: true, json: async () => ({
        result: 'SERVICE_TX_OK', receiver_execution: 'UNVERIFIED',
      }) };
    }
    throw new Error(`unexpected request ${url}`);
  },
};

async function settle() { await new Promise(setImmediate); }

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(posts.length, 0);
  for (const button of buttons) {
    assert.equal(button.disabled, !['CD', 'TUNER', 'PHONO', 'TAPE1'].includes(button.dataset.source));
  }
  await Promise.all([buttons[0].click(), buttons[0].click()]);
  assert.equal(posts.length, 1);
  assert.deepEqual(JSON.parse(posts[0].body), { source: 'CD' });
  assert.match(elements.get('availability-text').textContent, /SELECTION UNVERIFIED/);
  await buttons[0].click();
  assert.equal(posts.length, 1, 'rapid repeat must not dispatch');
  now += 1001;
  state = 'FAULT_LATCHED';
  refresh();
  await settle();
  assert.equal(buttons[0].disabled, true);
  state = 'IDLE';
  sourceEnabled = false;
  refresh();
  await settle();
  assert.equal(buttons[0].disabled, true);
  process.stdout.write('Web source offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
