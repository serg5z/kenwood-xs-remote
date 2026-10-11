const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

const reply = (data, ok = true) => ({ ok, json: async () => data });
const status = () => reply({ gateway: 'online', xs8_service: 'connected', status: {
  state: 'IDLE', monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
  last_byte: 'NONE', last_gpio_tx: 'NONE', last_waveform: 'NOT_OBSERVED',
} });
const session = (token, mute = true, source = true) =>
  reply({ csrf_token: token, mute_available: mute, source_available: source });
const capabilities = (enabled = true) => reply({ operations: { source: {
  enabled, available_sources: enabled ? ['CD', 'TUNER', 'PHONO', 'TAPE1'] : [],
} } });

const elements = new Map();
const buttons = ['CD', 'TUNER', 'PHONO', 'TV1', 'TV2', 'TAPE1', 'TAPE2'].map(
  (source) => ({ dataset: { source }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } }));
const mute = { disabled: true, addEventListener(_event, handler) { this.click = handler; } };
const sync = { disabled: true, addEventListener(_event, handler) { this.click = handler; } };
const calls = [];
const queued = new Map();
let poll;
let token = 'token-1';
let sourceAvailable = true;
let muteAvailable = true;
let serviceAvailable = true;
let capabilitiesAvailable = true;
let now = 10000;
let postResponse = null;
let postCount = 0;
const timers = new Map();
let nextTimer = 0;

function queue(url, value) {
  if (!queued.has(url)) queued.set(url, []);
  queued.get(url).push(value);
}

const context = {
  AbortController,
  Date: { now: () => now },
  setTimeout(callback) { const id = ++nextTimer; timers.set(id, callback); return id; },
  clearTimeout(id) { timers.delete(id); },
  setInterval(handler) { poll = handler; },
  document: {
    getElementById(id) {
      if (id === 'mute-button') return mute;
      if (id === 'sync-button') return sync;
      if (!elements.has(id)) elements.set(id, { textContent: '' });
      return elements.get(id);
    },
    querySelectorAll() { return buttons; },
  },
  fetch(url, options) {
    calls.push({ url, options });
    const overrides = queued.get(url);
    if (overrides && overrides.length) return Promise.resolve(overrides.shift());
    if (url === '/api/v1/status') {
      return Promise.resolve(serviceAvailable ? status()
        : reply({ gateway: 'online', xs8_service: 'unavailable' }, false));
    }
    if (url === '/api/v1/session') {
      return Promise.resolve(session(token, muteAvailable, sourceAvailable));
    }
    if (url === '/api/v1/capabilities') {
      return Promise.resolve(capabilities(capabilitiesAvailable));
    }
    if (url === '/api/v1/source' || url === '/api/v1/mute') {
      postCount++;
      token = `token-${postCount + 1}`; // Gateway rotates on an accepted POST.
      return Promise.resolve(postResponse || reply({ result: 'SERVICE_TX_OK' }));
    }
    throw new Error(`unexpected request ${url}`);
  },
};

async function settle() { await new Promise(setImmediate); }

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(mute.disabled, false);
  assert.equal(buttons[0].disabled, false);
  assert.equal(buttons[3].disabled, true);

  // A pre-POST session GET may return an old token after the POST refresh.
  const oldSession = deferred();
  queue('/api/v1/session', oldSession.promise);
  const oldPoll = poll();
  await settle();
  const first = buttons[0].click();
  assert.equal(mute.disabled, true, 'MUTE cannot reuse SOURCE token mid-POST');
  await mute.click();
  await first;
  assert.equal(postCount, 1);
  assert.equal(buttons[0].disabled, false);
  oldSession.resolve(session('token-1'));
  await oldPoll;
  assert.equal(mute.disabled, false, 'stale response must not disable newer state');
  now += 1001;
  await buttons[1].click();
  const sourcePosts = calls.filter((call) => call.url === '/api/v1/source');
  assert.equal(sourcePosts.length, 2);
  assert.equal(sourcePosts[1].options.headers['X-CSRF-Token'], 'token-2',
    'stale session must not replace the rotated token');

  const oldStatus = deferred();
  queue('/api/v1/status', oldStatus.promise);
  const oldFailedPoll = poll();
  await settle();
  await mute.click();
  oldStatus.resolve(Promise.reject(new Error('delayed network failure')));
  await oldFailedPoll;
  assert.equal(mute.disabled, false, 'stale failure must not erase newer readiness');
  assert.equal(postCount, 3);

  // Capability failure affects SOURCE only; MUTE remains independently usable.
  capabilitiesAvailable = false;
  await poll();
  assert.equal(mute.disabled, false);
  assert.equal(buttons[0].disabled, true);
  capabilitiesAvailable = true;
  await poll();
  assert.equal(buttons[0].disabled, false, 'SOURCE recovers on next poll');

  sourceAvailable = false;
  await poll();
  assert.equal(mute.disabled, false);
  assert.equal(buttons[0].disabled, true);
  sourceAvailable = true;
  muteAvailable = false;
  await poll();
  assert.equal(mute.disabled, true);
  assert.equal(buttons[0].disabled, false);
  muteAvailable = true;

  serviceAvailable = false;
  await poll();
  assert.equal(mute.disabled, true);
  assert.equal(buttons[0].disabled, true);
  serviceAvailable = true;
  await poll();
  assert.equal(mute.disabled, false);
  assert.equal(buttons[0].disabled, false, 'transient service loss recovers');

  queue('/api/v1/session', reply({ error: 'temporary gateway error' }, false));
  await poll();
  assert.equal(mute.disabled, true);
  assert.equal(buttons[0].disabled, true);
  await poll();
  assert.equal(mute.disabled, false, 'session request failure recovers');
  assert.equal(buttons[0].disabled, false);

  const hangingStatus = deferred();
  queue('/api/v1/status', hangingStatus.promise);
  const timedOutPoll = poll();
  await settle();
  const timeout = [...timers.values()][0];
  timeout();
  hangingStatus.resolve(status()); // Simulate a response after abort.
  await timedOutPoll;
  assert.equal(mute.disabled, true, 'timed-out response cannot authorize controls');
  await poll();
  assert.equal(mute.disabled, false, 'polling continues after timeout');

  // An uncertain POST is never retried automatically. Its token is refreshed.
  postResponse = Promise.reject(new Error('connection lost'));
  now += 1001;
  const before = postCount;
  await buttons[0].click();
  assert.equal(postCount, before + 1);
  assert.match(elements.get('availability-text').textContent, /RESULT UNKNOWN/);
  await poll();
  assert.equal(postCount, before + 1);
  assert.equal(buttons[0].disabled, false);

  assert.equal(timers.size, 0, 'request timeout timers are cleaned up');
  process.stdout.write('Web recovery offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
