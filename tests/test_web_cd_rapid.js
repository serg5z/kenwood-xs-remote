const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Rapid intentional CD taps under the Gateway-configured completion interval.
function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

function load(minIntervalMs) {
  const elements = new Map();
  const button = (cdAction) => ({ dataset: { cdAction }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } });
  const cd = { 'cd-previous': button('PREVIOUS'), 'cd-next': button('NEXT') };
  const env = { cd, elements, posts: [], now: 10000, pending: null, reply: null, poll: null };
  const ok = { result: 'SERVICE_TX_OK', cd_result: 'TX_VERIFIED', receiver_execution: 'UNVERIFIED' };
  const transport = { enabled: true, available_actions: ['NEXT', 'PREVIOUS'] };
  if (minIntervalMs !== undefined) transport.min_interval_ms = minIntervalMs;
  const context = {
    AbortController, setTimeout, clearTimeout,
    Date: { now: () => env.now },
    setInterval(handler) { env.poll = handler; },
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
          cd_release_owed: '0', cd_lease_expired: '0', cd_last_result: 'TX_VERIFIED' } }) };
      // The Gateway rotates the one-use token after every control POST.
      if (url === '/api/v1/session') return { ok: true, json: async () => ({
        csrf_token: `token-${env.posts.length + 1}`, cd_track_available: true }) };
      if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
        operations: { cd_transport: transport } }) };
      if (url === '/api/v1/cd/track') {
        env.posts.push({ token: options.headers['X-CSRF-Token'], body: JSON.parse(options.body) });
        if (env.pending) await env.pending.promise;
        const reply = env.reply || { ok: true, data: ok };
        env.reply = null;
        return { ok: reply.ok, json: async () => reply.data };
      }
      throw new Error(`unexpected request ${url}`);
    },
  };
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  return env;
}

async function settle() { for (let i = 0; i < 8; i += 1) await new Promise(setImmediate); }
const actions = (env) => env.posts.map((post) => post.body.action);
const tokensDistinct = (env) => new Set(env.posts.map((post) => post.token)).size === env.posts.length;

async function twoTapsAfterInterval() {
  // Generic spacing; a quick PREV pair is a double-PREV gesture (test_web_cd_prev_gesture.js).
  const env = load(250);
  await settle();
  const next = env.cd['cd-next'];
  await next.click();
  assert.equal(env.posts.length, 1);
  assert.equal(next.disabled, false, 'fresh token restores the control');
  env.now += 249;
  await next.click();
  assert.equal(env.posts.length, 1, 'tap inside the completion interval is dropped');
  env.now += 1;
  await next.click();
  assert.deepEqual(actions(env), ['NEXT', 'NEXT'], 'two separate NEXT requests');
  assert.deepEqual(env.posts.map((post) => post.token), ['token-1', 'token-2']);
  env.now += 10000;
  await settle();
  assert.equal(env.posts.length, 2, 'a dropped tap is never queued or repeated');
}

async function intervalCountsFromCompletion() {
  const env = load(250);
  await settle();
  env.pending = deferred();
  const first = env.cd['cd-next'].click();
  await settle();
  // Overlap: every tap while the first request is in flight is ignored.
  env.now += 5000;
  await Promise.all([env.cd['cd-next'].click(), env.cd['cd-previous'].click()]);
  assert.equal(env.posts.length, 1);
  env.pending.resolve();
  await first;
  env.pending = null;
  await env.cd['cd-previous'].click();
  assert.equal(env.posts.length, 1, 'spacing starts when the request completes');
  env.now += 250;
  await env.cd['cd-previous'].click();
  assert.deepEqual(actions(env), ['NEXT', 'PREVIOUS']);
  assert.ok(tokensDistinct(env));
}

async function serverTooFastIsReportedAndTokenRotates() {
  const env = load(250);
  await settle();
  env.reply = { ok: false, data: { result: 'TOO_FAST', min_interval_ms: 250,
    receiver_execution: 'UNVERIFIED' } };
  await env.cd['cd-previous'].click();
  assert.equal(env.elements.get('availability-text').textContent, 'PREV TOO_FAST · NOTHING SENT');
  env.now += 250;
  await env.cd['cd-previous'].click();
  assert.deepEqual(env.posts.map((post) => post.token), ['token-1', 'token-2']);
}

async function invalidIntervalFallsBackToOneSecond() {
  for (const value of [undefined, -1, 1, 100, 199, 1001, 5000, '250', '0', 250.5, null]) {
    const env = load(value);
    await settle();
    await env.cd['cd-next'].click();
    env.now += 999;
    await env.cd['cd-next'].click();
    assert.equal(env.posts.length, 1, `interval ${value} must fall back to 1000 ms`);
    env.now += 1;
    await env.cd['cd-next'].click();
    assert.equal(env.posts.length, 2);
  }
  for (const value of [200, 1000]) {
    const env = load(value);
    await settle();
    await env.cd['cd-next'].click();
    env.now += value - 1;
    await env.cd['cd-next'].click();
    env.now += 1;
    await env.cd['cd-next'].click();
    assert.equal(env.posts.length, 2, `interval ${value} is honored`);
  }
  // 0 is the experimental zero cooldown, not a missing value: no 1000 ms fallback.
  const zero = load(0);
  await settle();
  await zero.cd['cd-next'].click();
  await zero.cd['cd-next'].click(); // Same instant the first completed and refreshed.
  assert.equal(zero.posts.length, 2, 'interval 0 adds no client cooldown');
  assert.notEqual(zero.posts[0].token, zero.posts[1].token, 'still a fresh token each time');
  // Overlap is still impossible: a tap while a request is in flight is dropped.
  zero.pending = deferred();
  const inFlight = zero.cd['cd-next'].click();
  await settle();
  await zero.cd['cd-next'].click();
  zero.pending.resolve();
  await inFlight;
  zero.pending = null;
  assert.equal(zero.posts.length, 3, 'no overlapping request at interval 0');
}

async function main() {
  await twoTapsAfterInterval();
  await intervalCountsFromCompletion();
  await serverTooFastIsReportedAndTokenRotates();
  await invalidIntervalFallsBackToOneSecond();
  process.stdout.write('Web CD rapid-tap offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
