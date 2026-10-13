const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Double-PREV gesture: one explicit second PREV tap may wait for the first to
// complete, cool down and refresh; nothing else is ever queued or retried.
const OK = { ok: true, data: { result: 'SERVICE_TX_OK', cd_result: 'TX_VERIFIED',
  receiver_execution: 'UNVERIFIED' } };

function setup({ postMs = 331, refreshMs = 100 } = {}) {
  const elements = new Map();
  const button = (cdAction) => ({ dataset: { cdAction }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } });
  const cd = { 'cd-previous': button('PREVIOUS'), 'cd-next': button('NEXT') };
  const env = {
    cd, elements, now: 10000, timers: [], nextTimer: 1, posts: [], sessionFetches: [],
    replies: [], gate: null, connected: true, cdState: 'IDLE', source: 'CD',
    docListeners: {}, winListeners: {}, hidden: false,
  };
  const respond = (ok, data) => ({ ok, json: async () => data, headers: { get: () => null } });
  const context = {
    AbortController,
    Date: { now: () => env.now },
    setTimeout(fn, ms) {
      const id = env.nextTimer++;
      env.timers.push({ id, at: env.now + Math.max(0, ms || 0), fn });
      return id;
    },
    clearTimeout(id) { env.timers = env.timers.filter((timer) => timer.id !== id); },
    setInterval(handler) { env.poll = handler; },
    addEventListener(type, handler) { env.winListeners[type] = handler; },
    document: {
      get hidden() { return env.hidden; },
      addEventListener(type, handler) { env.docListeners[type] = handler; },
      getElementById(id) {
        if (cd[id]) return cd[id];
        if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
        return elements.get(id);
      },
      querySelectorAll: () => [],
    },
    async fetch(url, options) {
      if (url === '/api/v1/status') {
        env.now += refreshMs;
        if (!env.connected) return respond(false, { gateway: 'online', xs8_service: 'unavailable' });
        return respond(true, { gateway: 'online', xs8_service: 'connected',
          status: { state: 'IDLE', monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
            cd_state: env.cdState, cd_release_owed: env.cdState === 'HOLDING' ? '1' : '0',
            cd_last_result: 'TX_VERIFIED' },
          receiver: { integrity: 'HEALTHY', source: { value: env.source, freshness: 'FRESH' },
            monitor: { value: 'OFF', freshness: 'FRESH' } } });
      }
      if (url === '/api/v1/session') {
        env.now += refreshMs;
        // The Gateway rotates the one-use token after every control POST.
        const token = `token-${env.posts.length + 1}`;
        env.sessionFetches.push({ at: env.now, token });
        return respond(true, { csrf_token: token, cd_track_available: true });
      }
      if (url === '/api/v1/capabilities') {
        env.now += refreshMs;
        return respond(true, { operations: { cd_transport: { enabled: true,
          available_actions: ['NEXT', 'PREVIOUS'], min_interval_ms: 250 } } });
      }
      if (url === '/api/v1/cd/track') {
        env.posts.push({ at: env.now, token: options.headers['X-CSRF-Token'],
          action: JSON.parse(options.body).action });
        if (env.gate) await env.gate.promise;
        env.now += postMs;
        const reply = env.replies.shift() || OK;
        if (reply === 'throw') throw new Error('network');
        return respond(reply.ok, reply.data);
      }
      throw new Error(`unexpected request ${url}`);
    },
  };
  env.settle = async () => { for (let i = 0; i < 12; i += 1) await new Promise(setImmediate); };
  // Run fake timers that fall due within ms, in order, letting async work settle.
  env.advance = async (ms) => {
    const target = env.now + ms;
    for (;;) {
      env.timers.sort((a, b) => a.at - b.at);
      const timer = env.timers[0];
      if (!timer || timer.at > target) break;
      env.timers.shift();
      env.now = Math.max(env.now, timer.at);
      timer.fn();
      await env.settle();
    }
    env.now = Math.max(env.now, target);
  };
  // Holds the next POST(s) until released; a release clears only its own gate.
  env.hold = () => {
    let resolve;
    const gate = { promise: new Promise((done) => { resolve = done; }) };
    env.gate = gate;
    return async () => {
      if (env.gate === gate) env.gate = null;
      resolve();
      await env.settle();
    };
  };
  env.text = () => elements.get('availability-text').textContent;
  env.tap = async (id = 'cd-previous') => { cd[id].click(); await env.settle(); };
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  return env;
}

const actions = (env) => env.posts.map((post) => post.action);

async function singlePrevQueuesNothing() {
  const env = setup();
  await env.settle();
  await env.tap();
  await env.advance(5000);
  assert.deepEqual(actions(env), ['PREVIOUS'], 'single PREV sends exactly one command');
  assert.doesNotMatch(env.text(), /PENDING/);
  assert.equal(env.cd['cd-previous'].disabled, false);
  assert.equal(env.cd['cd-next'].disabled, false, 'gesture ends after the window');
}

async function rapidDoublePrevSendsTwoWithFreshTokenAfterCooldown() {
  const env = setup({ refreshMs: 10 }); // Fast refresh: the 250 ms cooldown governs.
  await env.settle();
  const release = env.hold();
  await env.tap();
  await env.advance(200);
  await env.tap(); // Natural second tap while the first PREV is in flight.
  assert.equal(env.cd['cd-previous'].disabled, false, 'PREV stays receptive');
  assert.equal(env.cd['cd-previous'].dataset.pending, 'true');
  assert.equal(env.cd['cd-next'].disabled, true, 'NEXT is not available during the gesture');
  assert.equal(env.posts.length, 1, 'never overlaps the first command');
  await env.tap(); // Third tap.
  assert.match(env.text(), /SECOND PREV PENDING · EXTRA TAP IGNORED/);
  await release();
  const completed = env.posts[0].at + 331 + 200;
  assert.equal(env.posts.length, 1, 'cooldown not yet elapsed after the refresh');
  await env.advance(completed + 249 - env.now);
  assert.equal(env.posts.length, 1, 'still inside the 250 ms cooldown');
  const releaseSecond = env.hold();
  await env.advance(1);
  assert.deepEqual(actions(env), ['PREVIOUS', 'PREVIOUS']);
  await env.tap(); // Tap while the second PREV is in flight.
  assert.match(env.text(), /SECOND PREV SENT · EXTRA TAP IGNORED/);
  await releaseSecond();
  assert.equal(env.posts[1].at - completed, 250, 'cooldown measured from completion');
  assert.deepEqual(env.posts.map((post) => post.token), ['token-1', 'token-2']);
  assert.ok(env.sessionFetches.some((fetch) => fetch.token === 'token-2' &&
    fetch.at > completed && fetch.at <= env.posts[1].at), 'fresh token fetched before dispatch');
  assert.equal(env.cd['cd-previous'].dataset.pending, undefined);
  await env.advance(5000);
  assert.equal(env.posts.length, 2, 'third and later taps never add commands');
  assert.match(env.text(), /^PREV TX VERIFIED · TRACK CHANGE UNVERIFIED/);
}

async function burstAfterSecondCompletesAddsNothing() {
  // Regression: the gesture used to end as soon as the second PREV completed, so a
  // continuing burst could send a third PREV once the 250 ms interval passed.
  const env = setup({ refreshMs: 100 });
  await env.settle();
  const release = env.hold();
  await env.tap();
  await env.advance(200);
  await env.tap(); // Queued second PREV.
  const releaseSecond = env.hold();
  await release();
  assert.equal(env.posts.length, 2);
  await env.advance(300);
  await env.tap(); // Third tap while the second runs, after the 800 ms window.
  await releaseSecond();
  assert.ok(env.now - env.posts[0].at > 800, 'well past the recognition window');
  // The refresh after the second PREV took 300 ms; the last tap was 631 ms ago.
  // Each following tap is under 800 ms after the previous one, and the first is
  // past the 250 ms interval with a fresh token, so the old code sent it.
  for (const gap of [100, 500, 700]) {
    await env.advance(gap);
    await env.tap(); // The burst continues after the second completed.
    assert.equal(env.posts.length, 2, `tap ${gap} ms later must not add a command`);
    assert.match(env.text(), /^PREV TX VERIFIED · TRACK CHANGE UNVERIFIED · EXTRA TAP IGNORED/,
      'the second result stays visible with the ignored-tap note');
  }
  assert.equal(env.cd['cd-next'].disabled, true, 'NEXT waits for the burst to end');
  // A pause of the full window after the last tap ends the gesture.
  await env.advance(801);
  assert.equal(env.cd['cd-previous'].disabled, false);
  assert.equal(env.cd['cd-next'].disabled, false);
  await env.tap();
  assert.equal(env.posts.length, 3, 'a deliberate later PREV is a new single action');
  await env.advance(5000);
  assert.equal(env.posts.length, 3);
}

async function failedSecondAlsoAbsorbsBurst() {
  const env = setup({ refreshMs: 10 });
  await env.settle();
  env.replies.push(OK, { ok: false, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } });
  const release = env.hold();
  await env.tap();
  await env.tap();
  await release();
  const releaseSecond = env.hold();
  await env.advance(250);
  assert.equal(env.posts.length, 2);
  await env.advance(200);
  await env.tap(); // During the second PREV.
  await releaseSecond();
  assert.match(env.text(), /^PREV SERVICE_UNCERTAIN · CHECK CHANGER · EXTRA TAP IGNORED/,
    'the uncertain result stays visible');
  await env.advance(300);
  await env.tap(); // Shortly after the failure, past the 250 ms interval.
  await env.advance(5000);
  assert.equal(env.posts.length, 2, 'no retry and no extra command after a failed second');
}

async function slowRefreshDispatchesRightAfterToken() {
  const env = setup({ refreshMs: 100 }); // Pi-like: refresh (300 ms) exceeds cooldown.
  await env.settle();
  const release = env.hold();
  await env.tap();
  await env.advance(150);
  await env.tap();
  await release();
  assert.deepEqual(actions(env), ['PREVIOUS', 'PREVIOUS']);
  assert.equal(env.posts[1].at - (env.posts[0].at + 150 + 331), 300);
}

async function laterTapIsASeparateAction() {
  const env = setup({ postMs: 900 }); // Slow first command outlasts the window.
  await env.settle();
  const release = env.hold();
  await env.tap();
  await env.advance(801);
  await env.tap();
  await release();
  await env.advance(5000);
  assert.deepEqual(actions(env), ['PREVIOUS'], 'a tap after the window is not queued');
  assert.doesNotMatch(env.text(), /PENDING/);
}

async function nextIsNeverQueued() {
  const env = setup();
  await env.settle();
  const release = env.hold();
  await env.tap('cd-next');
  await env.tap('cd-next');
  await env.tap('cd-previous'); // A NEXT in flight opens no PREV gesture.
  await release();
  await env.advance(5000);
  assert.deepEqual(actions(env), ['NEXT']);
}

async function cancelled(name, act, { reply } = {}) {
  const env = setup({ refreshMs: 10 });
  await env.settle();
  if (reply) env.replies.push(reply);
  const release = env.hold();
  await env.tap();
  await env.advance(100);
  await env.tap();
  assert.match(env.text(), /SECOND PREV PENDING/, name);
  await act(env, release);
  await env.advance(5000);
  assert.equal(env.posts.length, 1, `${name}: pending PREV cancelled`);
  assert.match(env.text(), /SECOND PREV CANCELLED/, name);
  assert.equal(env.cd['cd-previous'].dataset.pending, undefined, name);
  return env;
}

async function cancellations() {
  const failures = {
    '409 NOT_ELIGIBLE': { ok: false, data: { result: 'NOT_ELIGIBLE' } },
    '429 TOO_FAST': { ok: false, data: { result: 'TOO_FAST', min_interval_ms: 250 } },
    '502 SERVICE_UNCERTAIN': { ok: false, data: { result: 'SERVICE_UNCERTAIN',
      physical_check_required: true } },
    '504 TIMEOUT': { ok: false, data: { result: 'TIMEOUT', physical_check_required: true } },
  };
  for (const [name, reply] of Object.entries(failures)) {
    const env = await cancelled(name, (_env, release) => release(), { reply });
    if (name.startsWith('504') || name.startsWith('502')) {
      assert.match(env.text(), /CHECK CHANGER/, `${name} is never treated as nothing sent`);
    }
  }
  const network = await cancelled('network error', (_env, release) => release(), { reply: 'throw' });
  assert.match(network.text(), /CD RESULT UNKNOWN · CHECK CHANGER/);
  await cancelled('source change', async (env, release) => { env.source = 'TUNER'; await release(); });
  await cancelled('connection lost', async (env, release) => { env.connected = false; await release(); });
  await cancelled('changer busy', async (env, release) => { env.cdState = 'HOLDING'; await release(); });
  await cancelled('page hidden', async (env, release) => {
    env.hidden = true;
    env.docListeners.visibilitychange();
    await release();
  });
  await cancelled('page hide event', async (env, release) => {
    env.winListeners.pagehide();
    await release();
  });
  // Eligibility lost by a poll refresh during the cooldown wait.
  await cancelled('lost during cooldown', async (env, release) => {
    await release();
    env.cdState = 'HOLDING';
    env.poll();
    await env.settle();
  });
}

async function secondFailureIsNotRetried() {
  for (const reply of [{ ok: false, data: { result: 'TOO_FAST' } },
    { ok: false, data: { result: 'CD_BUSY' } },
    { ok: false, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } },
    { ok: false, data: { result: 'TIMEOUT', physical_check_required: true } }, 'throw']) {
    const env = setup({ refreshMs: 10 });
    await env.settle();
    env.replies.push(OK, reply);
    const release = env.hold();
    await env.tap();
    await env.tap();
    await release();
    await env.advance(250);
    await env.advance(10000);
    assert.deepEqual(actions(env), ['PREVIOUS', 'PREVIOUS'], 'no retry after a failed second PREV');
  }
}

async function main() {
  await singlePrevQueuesNothing();
  await rapidDoublePrevSendsTwoWithFreshTokenAfterCooldown();
  await burstAfterSecondCompletesAddsNothing();
  await failedSecondAlsoAbsorbsBurst();
  await slowRefreshDispatchesRightAfterToken();
  await laterTapIsASeparateAction();
  await nextIsNeverQueued();
  await cancellations();
  await secondFailureIsNotRetried();
  process.stdout.write('Web CD double-PREV gesture offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
