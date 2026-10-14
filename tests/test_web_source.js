const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('web/index.html', 'utf8');
const css = fs.readFileSync('web/app.css', 'utf8');
const names = ['CD', 'TUNER', 'PHONO', 'TV1', 'TV2', 'TAPE1', 'TAPE2'];
// Six selectable sources; the TAPE2 button is the separate MONITOR control.
for (const name of names.slice(0, 6)) assert.match(html, new RegExp(`data-source="${name}"`));
assert.doesNotMatch(html, /data-source="TAPE2"/);
assert.match(html, /id="monitor-button" disabled type="button" data-monitor-state="UNKNOWN"[^>]*><span>TAPE2<\/span><small>MONITOR<\/small>/);
assert.match(css, /\.source-grid #monitor-button small/);
assert.match(css, /#monitor-button \{ position:relative; display:flex; align-items:center; justify-content:center; \}/);
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
let reply = null; // { ok, data } for the next source POST; default verified.
const context = {
  AbortController, setTimeout, clearTimeout,
  document: {
    getElementById(id) {
      if (id === 'mute-button') return mute;
      if (id === 'sync-button') return sync;
      if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
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
      const next = reply || { ok: true, data: { result: 'SERVICE_TX_OK', receiver_execution: 'UNVERIFIED' } };
      reply = null;
      return { ok: next.ok, json: async () => next.data };
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

  // Leaving CD: the Gateway's CD_STOP outcome is shown; the browser never
  // retries and never claims a stopped changer or a selected source.
  const text = () => elements.get('availability-text').textContent;
  const stop = { gpio_tx: 'TX_OK', waveform: 'MATCH', receiver_execution: 'UNVERIFIED' };
  const cases = [
    [1, { ok: true, data: { result: 'SERVICE_TX_OK', source: 'TUNER', cd_stop: stop,
      receiver_execution: 'UNVERIFIED' } },
    /^CD STOP TX REPORTED · TUNER TX REPORTED · SELECTION UNVERIFIED$/],
    [2, { ok: false, data: { result: 'SERVICE_UNCERTAIN', stage: 'CD_STOP', source: 'PHONO',
      source_sent: false, physical_check_required: true } },
    /^CD STOP SERVICE_UNCERTAIN · PHONO NOT SENT · CHECK CHANGER$/],
    [5, { ok: false, data: { result: 'REJECTED', stage: 'CD_STOP', source: 'TAPE1',
      source_sent: false, physical_check_required: false } },
    /^CD STOP REJECTED · TAPE1 NOT SENT$/],
    [1, { ok: false, data: { result: 'SERVICE_UNCERTAIN', stage: 'SELECT_SOURCE', source: 'TUNER',
      source_sent: true, cd_stop: stop, physical_check_required: true } },
    /^CD STOP TX REPORTED · SOURCE SERVICE_UNCERTAIN · CHECK RECEIVER$/],
    [0, { ok: true, data: { result: 'SERVICE_TX_OK', receiver_execution: 'UNVERIFIED' } },
    /^CD TX REPORTED · SELECTION UNVERIFIED$/],
  ];
  for (const [index, response, message] of cases) {
    now += 1001;
    const before = posts.length;
    reply = response;
    await buttons[index].click();
    assert.equal(posts.length, before + 1);
    assert.match(text(), message);
    now += 1001;
    refresh();
    await settle();
    assert.equal(posts.length, before + 1, 'no automatic retry');
  }

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
