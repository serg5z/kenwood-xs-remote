const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Consecutive legitimate CD actions and duplicate browser events.
function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

const elements = new Map();
const button = (cdAction) => ({ dataset: { cdAction }, disabled: true,
  addEventListener(event, handler) { this.event = event; this.click = handler; } });
const cd = { 'cd-previous': button('PREVIOUS'), 'cd-next': button('NEXT') };
const posts = [];
let sessionFetches = 0;
let now = 10000;
let pending = null;
let poll;

const ok = { result: 'SERVICE_TX_OK', cd_result: 'TX_VERIFIED', receiver_execution: 'UNVERIFIED' };
const context = {
  AbortController, setTimeout, clearTimeout,
  Date: { now: () => now },
  setInterval(handler) { poll = handler; },
  document: {
    getElementById(id) {
      if (cd[id]) return cd[id];
      if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
      return elements.get(id);
    },
    querySelectorAll: () => [],
  },
  async fetch(url, options) {
    if (url === '/api/v1/status') return { ok: true, json: async () => ({
      gateway: 'online', xs8_service: 'connected', status: { state: 'IDLE',
        monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0', last_byte: '0x72',
        last_gpio_tx: 'TX_OK', last_waveform: 'MATCH', cd_state: 'IDLE', cd_action: 'NONE',
        cd_release_owed: '0', cd_lease_expired: '0', cd_last_result: 'NONE' } }) };
    if (url === '/api/v1/session') {
      // The Gateway rotates the one-use token after every control POST.
      sessionFetches += 1;
      return { ok: true, json: async () => ({ csrf_token: `token-${posts.length + 1}`,
        cd_track_available: true }) };
    }
    if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
      operations: { cd_transport: { enabled: true, available_actions: ['NEXT', 'PREVIOUS'] } } }) };
    if (url === '/api/v1/cd/track') {
      posts.push({ token: options.headers['X-CSRF-Token'], body: JSON.parse(options.body) });
      if (pending) await pending.promise;
      return { ok: true, json: async () => ok };
    }
    throw new Error(`unexpected request ${url}`);
  },
};

async function settle() { for (let i = 0; i < 8; i += 1) await new Promise(setImmediate); }

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(cd['cd-next'].event, 'click', 'only click activates CD controls');
  assert.equal(cd['cd-previous'].event, 'click');
  assert.equal(cd['cd-next'].disabled, false);

  // 1. Two consecutive legitimate actions: each uses a fresh token, one dispatch each.
  await cd['cd-next'].click();
  assert.equal(posts.length, 1);
  assert.deepEqual(posts[0], { token: 'token-1', body: { action: 'NEXT' } });
  assert.equal(cd['cd-previous'].disabled, false, 'post-action refresh re-enables controls');
  now += 1000;
  await cd['cd-previous'].click();
  assert.equal(posts.length, 2);
  assert.deepEqual(posts[1], { token: 'token-2', body: { action: 'PREVIOUS' } });
  assert.notEqual(posts[0].token, posts[1].token);
  assert.ok(sessionFetches >= 3, 'a session refresh preceded each action');

  // 2. Repeated events while a request is in flight never dispatch again.
  now += 1000;
  pending = deferred();
  const first = cd['cd-next'].click();
  await settle();
  assert.equal(cd['cd-next'].disabled, true, 'buttons disable while in flight');
  assert.equal(cd['cd-previous'].disabled, true);
  now += 5000; // Even after the client interval, the in-flight lock holds.
  await Promise.all([cd['cd-next'].click(), cd['cd-previous'].click(), cd['cd-next'].click()]);
  assert.equal(posts.length, 3);
  pending.resolve();
  await first;
  pending = null;
  assert.equal(posts[2].token, 'token-3');

  // 3. Same-tick double click dispatches once.
  now += 1000;
  await Promise.all([cd['cd-previous'].click(), cd['cd-previous'].click()]);
  assert.equal(posts.length, 4);
  assert.equal(posts[3].token, 'token-4');

  // 4. A click inside the 1 s client interval after completion is ignored even
  // though a fresh token is already available.
  now += 999;
  assert.equal(cd['cd-next'].disabled, false);
  await cd['cd-next'].click();
  assert.equal(posts.length, 4, 'client interval stays 1 s');

  // 5. A disabled button (no token yet) cannot dispatch, e.g. a polling refresh in flight.
  now += 2;
  cd['cd-next'].disabled = true;
  await cd['cd-next'].click();
  assert.equal(posts.length, 4);
  poll();
  await settle();
  await cd['cd-next'].click();
  assert.equal(posts.length, 5);

  // Every dispatched request used a distinct one-use token.
  assert.equal(new Set(posts.map((post) => post.token)).size, posts.length);
  process.stdout.write('Web CD sequence offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
