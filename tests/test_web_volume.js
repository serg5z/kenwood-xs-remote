const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('web/index.html', 'utf8');
assert.match(html, /<button id="volume-up" disabled type="button" aria-label="Volume up" data-volume-direction="UP">\+<\/button>/);
assert.match(html, /<button id="volume-down" disabled type="button" aria-label="Volume down" data-volume-direction="DOWN">−<\/button>/);
assert.equal((html.match(/data-volume-direction=/g) || []).length, 2);

const posts = [];
const events = [];
const elements = new Map();
const stub = () => ({ disabled: true, dataset: {}, addEventListener() {} });
function volumeButton(direction) {
  return { dataset: { volumeDirection: direction }, disabled: true,
    addEventListener(event, handler) { events.push(event); this.click = handler; } };
}
const volume = { 'volume-up': volumeButton('UP'), 'volume-down': volumeButton('DOWN') };
const up = volume['volume-up'];
const down = volume['volume-down'];
const mute = stub();
const sync = stub();
let state = 'IDLE';
let cdFields = true;
let sessionAvailable = true;
let capability = { enabled: true, available_directions: ['UP', 'DOWN'], min_interval_ms: 250 };
let postReply = { ok: true, data: { result: 'SERVICE_TX_OK', direction: 'UP',
  volume_result: 'TX_VERIFIED', receiver_execution: 'UNVERIFIED' } };
let postThrows = false;
let postHangs = false;
let duringPost = null;
// Models the Gateway: one current token per session, consumed by every POST that
// passes the CSRF check, whatever the outcome; GET /api/v1/session returns it.
let gatewayToken = 'token-0';
let rotations = 0;
let sessionGets = 0;
const forbidden = [];
let browserTimeout = null;
let now = 10000;
let poll;

const context = {
  AbortController, clearTimeout,
  // The browser's 6 s POST timeout fires only when the test says so.
  setTimeout: (handler, ms) => (ms === 6000 ? (browserTimeout = handler, -1)
    : setTimeout(handler, ms)),
  Date: { now: () => now },
  setInterval(handler) { poll = handler; },
  document: {
    getElementById(id) {
      if (id === 'mute-button') return mute;
      if (id === 'sync-button') return sync;
      if (volume[id]) return volume[id];
      if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
      return elements.get(id);
    },
    querySelectorAll: () => [],
  },
  async fetch(url, options) {
    if (url === '/api/v1/status') {
      const status = { state, monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
        last_byte: '0x07', last_gpio_tx: 'TX_OK', last_waveform: 'MATCH' };
      if (cdFields) Object.assign(status, { cd_state: 'IDLE', cd_action: 'NONE',
        cd_release_owed: '0', cd_lease_expired: '0', cd_last_result: 'NONE' });
      return { ok: true, json: async () => ({ gateway: 'online', xs8_service: 'connected', status }) };
    }
    if (url === '/api/v1/session') sessionGets += 1;
    if (url === '/api/v1/session') return { ok: true, json: async () => ({
      csrf_token: gatewayToken, mute_available: false, source_available: false,
      receiver_query_available: false, cd_track_available: false,
      volume_step_available: sessionAvailable }) };
    if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
      operations: capability ? { volume_step: capability } : {} }) };
    if (url === '/api/v1/volume/step') {
      posts.push(options);
      if (options.headers['X-CSRF-Token'] !== gatewayToken) {
        forbidden.push(options.headers['X-CSRF-Token']);
        return { ok: false, json: async () => ({ error: 'Invalid CSRF token' }) };
      }
      gatewayToken = `token-${++rotations}`;
      if (duringPost) duringPost();
      if (postHangs) {
        return new Promise((_, reject) => options.signal.addEventListener('abort',
          () => reject(new Error('aborted'))));
      }
      if (postThrows) throw new Error('network');
      return { ok: postReply.ok, json: async () => postReply.data };
    }
    throw new Error(`unexpected request ${url}`);
  },
};

async function settle() { for (let i = 0; i < 6; i += 1) await new Promise(setImmediate); }
const availability = () => elements.get('availability-text').textContent;
async function refreshed() { poll(); await settle(); }

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(posts.length, 0, 'loading the page must not step');
  assert.equal(up.disabled, false);
  assert.equal(down.disabled, false);
  assert.deepEqual([...new Set(events)], ['click'], 'phase 1: no pointer, key or hold handlers');

  // One click, one POST; both controls disabled while it is in flight.
  duringPost = () => {
    assert.equal(up.disabled, true, 'VOL+ disabled in flight');
    assert.equal(down.disabled, true, 'VOL− disabled in flight');
  };
  const first = up.click();
  const duplicate = up.click();
  const other = down.click();
  await Promise.all([first, duplicate, other]);
  await settle();
  duringPost = null;
  assert.equal(posts.length, 1, 'duplicate and concurrent clicks are dropped');
  assert.equal(posts[0].method, 'POST');
  assert.equal(posts[0].body, '{"direction":"UP"}');
  assert.equal(posts[0].headers['X-CSRF-Token'], 'token-0');
  assert.equal(posts[0].headers['Content-Type'], 'application/json');
  assert.equal(availability(), 'VOL+ TX VERIFIED · VOLUME CHANGE UNVERIFIED');
  assert.equal(up.disabled, false, 'the refresh re-enables the controls');

  // A click inside the spacing interval is dropped, not queued.
  await down.click();
  assert.equal(posts.length, 1);
  now += 250;
  await down.click();
  await settle();
  assert.equal(posts.length, 2);
  assert.equal(posts[1].body, '{"direction":"DOWN"}');
  assert.equal(posts[1].headers['X-CSRF-Token'], 'token-1', 'fresh token per request');

  // Failures are displayed and never retried, by the click or by later polls.
  const failures = [
    [{ ok: false, data: { result: 'REJECTED', physical_check_required: false } },
      'VOL+ REJECTED · NOTHING SENT'],
    [{ ok: false, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } },
      'VOL+ SERVICE_UNCERTAIN · CHECK RECEIVER'],
    [{ ok: false, data: { result: 'TIMEOUT', physical_check_required: true } },
      'VOL+ TIMEOUT · CHECK RECEIVER'],
    [{ ok: false, data: { result: 'FAULT_LATCHED', physical_check_required: true } },
      'VOL+ FAULT_LATCHED · CHECK RECEIVER · RESTART REQUIRED'],
    [{ ok: false, data: { result: 'VOLUME_BUSY' } }, 'VOL+ VOLUME_BUSY · NOTHING SENT'],
  ];
  for (const [reply, message] of failures) {
    now += 1000;
    postReply = reply;
    const before = posts.length;
    await up.click();
    await settle();
    assert.equal(posts.length, before + 1);
    assert.equal(availability().startsWith(message), true, availability());
    await refreshed();
    now += 1000;
    await refreshed();
    assert.equal(posts.length, before + 1, 'no automatic retry');
  }
  now += 1000;
  postThrows = true;
  const before = posts.length;
  await up.click();
  await settle();
  assert.equal(posts.length, before + 1);
  assert.equal(availability().startsWith('VOLUME RESULT UNKNOWN · CHECK RECEIVER'), true);
  now += 1000;
  await refreshed();
  assert.equal(posts.length, before + 1, 'no retry after a network failure');
  postThrows = false;

  // Every failure above recovered a valid token by GET alone: no POST was ever
  // refused for CSRF, and each POST used the token current at that moment.
  assert.deepEqual(forbidden, []);
  assert.equal(new Set(posts.map((post) => post.headers['X-CSRF-Token'])).size, posts.length);

  // Browser timeout: the POST is aborted, the result is unknown, the token is
  // recovered by the refresh, and nothing is sent again.
  now += 1000;
  postHangs = true;
  let sent = posts.length;
  const pending = up.click();
  await settle();
  assert.equal(posts.length, sent + 1);
  assert.equal(up.disabled, true);
  assert.equal(typeof browserTimeout, 'function');
  postHangs = false;
  const gets = sessionGets;
  browserTimeout();
  await pending;
  await settle();
  assert.equal(availability().startsWith('VOLUME RESULT UNKNOWN · CHECK RECEIVER'), true);
  assert.equal(sessionGets, gets + 1, 'token recovered by one session GET');
  assert.equal(up.disabled, false);
  now += 1000;
  await refreshed();
  assert.equal(posts.length, sent + 1, 'no retry after a browser timeout');
  now += 1000;
  await up.click();
  await settle();
  assert.equal(posts.length, sent + 2);
  assert.deepEqual(forbidden, [], 'the recovered token is accepted');

  // Refusal while a CEC hold is active: no token while STATUS is not IDLE, then the
  // regular poll recovers it once the hold ends, still without a POST.
  now += 1000;
  postReply = { ok: false, data: { result: 'VOLUME_BUSY', receiver_execution: 'UNVERIFIED' } };
  duringPost = () => { state = 'HOLDING_UP'; };
  sent = posts.length;
  await up.click();
  await settle();
  duringPost = null;
  assert.equal(up.disabled, true, 'no token while the hold is reported');
  state = 'IDLE';
  now += 1000;
  await refreshed();
  assert.equal(up.disabled, false, 'poll recovers the token');
  assert.equal(posts.length, sent + 1, 'recovery never posts');
  postReply = { ok: true, data: { result: 'SERVICE_TX_OK', direction: 'UP',
    volume_result: 'TX_VERIFIED', receiver_execution: 'UNVERIFIED' } };
  await up.click();
  await settle();
  assert.equal(posts.length, sent + 2);
  assert.deepEqual(forbidden, []);
  const sentBeforeUnavailable = posts.length;

  // Controls stay disabled whenever the capability or service is not available.
  const unavailable = [
    () => { sessionAvailable = false; },
    () => { capability = { ...capability, enabled: false }; },
    () => { capability = null; },
    () => { capability = { enabled: true }; },
    () => { capability = { enabled: true, available_directions: ['UP'] }; },
    () => { state = 'BOOT_INHIBIT'; },
    () => { state = 'HOLDING_UP'; },
    () => { cdFields = false; },
  ];
  const reset = () => {
    sessionAvailable = true; state = 'IDLE'; cdFields = true;
    capability = { enabled: true, available_directions: ['UP', 'DOWN'], min_interval_ms: 250 };
  };
  for (const [index, change] of unavailable.entries()) {
    reset();
    change();
    now += 1000;
    await refreshed();
    if (index === 4) {
      assert.equal(up.disabled, false, 'only allowlisted directions are enabled');
      assert.equal(down.disabled, true);
    } else {
      assert.equal(up.disabled, true, `case ${index}`);
      assert.equal(down.disabled, true, `case ${index}`);
      await up.click();
      await down.click();
    }
  }
  assert.equal(posts.length, sentBeforeUnavailable, 'disabled controls never send');
  assert.equal(mute.disabled, true, 'volume availability does not enable MUTE');
  process.stdout.write('Web volume offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
