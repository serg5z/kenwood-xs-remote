const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('web/index.html', 'utf8');
for (const [id, attribute] of [['cd-rew', 'data-cd-seek="BACKWARD"'], ['cd-ff', 'data-cd-seek="FORWARD"'],
  ['cd-play-pause', 'data-cd-playback="PLAY_PAUSE"'], ['cd-stop', 'data-cd-playback="STOP"'],
  ['cd-time-display', 'data-cd-playback="TIME_DISPLAY"'], ['cd-repeat', 'data-cd-playback="REPEAT"'],
  ['cd-disc-skip', 'data-cd-playback="DISC_SKIP"']]) {
  assert.match(html, new RegExp(`id="${id}" disabled type="button" ${attribute}`));
}
for (const label of ['TIME DISP.', 'REPEAT', 'DISC SKIP']) {
  assert.ok(html.includes(`<small>${label}</small>`), label);
}

// ---- Fake clock: every app timer runs only when the test advances time. ----
let now = 100000;
let nextTimer = 1;
const timers = new Map();
function fakeSetTimeout(handler, ms) {
  const id = nextTimer++;
  timers.set(id, { handler, at: now + (ms || 0) });
  return id;
}
function fakeClearTimeout(id) { timers.delete(id); }
async function settle() { for (let i = 0; i < 12; i += 1) await new Promise(setImmediate); }
async function advance(ms) {
  await settle(); // Pending responses run before time passes, as in a browser.
  const end = now + ms;
  for (;;) {
    let due = null;
    for (const [id, timer] of timers) {
      if (timer.at <= end && (!due || timer.at < due[1].at)) due = [id, timer];
    }
    if (!due) break;
    timers.delete(due[0]);
    now = Math.max(now, due[1].at);
    due[1].handler();
    await settle();
  }
  now = end;
  await settle();
}

// ---- Elements with real listener lists and pointer capture. ----
const elements = new Map();
const datasets = {
  'cd-rew': { cdSeek: 'BACKWARD' }, 'cd-ff': { cdSeek: 'FORWARD' },
  'cd-play-pause': { cdPlayback: 'PLAY_PAUSE' }, 'cd-stop': { cdPlayback: 'STOP' },
  'cd-time-display': { cdPlayback: 'TIME_DISPLAY' }, 'cd-repeat': { cdPlayback: 'REPEAT' },
  'cd-disc-skip': { cdPlayback: 'DISC_SKIP' },
  'cd-previous': { cdAction: 'PREVIOUS' }, 'cd-next': { cdAction: 'NEXT' },
  'volume-up': { volumeDirection: 'UP' }, 'volume-down': { volumeDirection: 'DOWN' },
};
function element(id) {
  if (!elements.has(id)) {
    elements.set(id, {
      id, textContent: '', disabled: true, dataset: { ...(datasets[id] || {}) }, listeners: {},
      captured: null,
      addEventListener(type, handler) { (this.listeners[type] ||= []).push(handler); },
      fire(type, event = {}) {
        const full = { pointerId: 7, button: 0, preventDefault() {}, ...event };
        return Promise.all((this.listeners[type] || []).map((handler) => handler(full)));
      },
      setPointerCapture(id) { this.captured = id; },
      releasePointerCapture() { this.captured = null; },
    });
  }
  return elements.get(id);
}
const ff = element('cd-ff');
const rew = element('cd-rew');
const playPause = element('cd-play-pause');
const stop = element('cd-stop');
const timeDisplay = element('cd-time-display');
const repeat = element('cd-repeat');
const discSkip = element('cd-disc-skip');
const momentary = [[timeDisplay, 'TIME_DISPLAY', 'TIME DISP\\.'], [repeat, 'REPEAT', 'REPEAT'],
  [discSkip, 'DISC_SKIP', 'DISC SKIP']];
const next = element('cd-next');
const volumeUp = element('volume-up');
const windowListeners = {};
const documentListeners = {};
const document = {
  hidden: false,
  getElementById: element,
  querySelectorAll: () => [],
  addEventListener(type, handler) { (documentListeners[type] ||= []).push(handler); },
};
const fireWindow = (type) => (windowListeners[type] || []).forEach((handler) => handler({}));
const fireDocument = (type) => (documentListeners[type] || []).forEach((handler) => handler({}));
const availability = () => element('availability-text').textContent;

// ---- Fake Gateway: single-use CSRF, one server-wide hold, scripted faults. ----
let gatewayToken = 't0';
let rotations = 0;
let sessionGets = 0;
let network = 'up';
let hold = null;
let holdCount = 0;
let cdState = 'IDLE';
let seekAvailable = true;
let playbackAvailable = true;
let playbackActions = ['PLAY_PAUSE', 'STOP', 'TIME_DISPLAY', 'REPEAT', 'DISC_SKIP'];
const playbackPlans = [];  // per playback POST: { status, data } overrides success
const plans = [];          // per seek POST: 'defer' | 'drop' | { status, data }
const deferred = [];
const requests = [];       // every seek POST that reached the network
const forbidden = [];
const playbackPosts = [];

function gatewaySeek(record, override) {
  if (record.token !== gatewayToken) {
    forbidden.push(record);
    return { status: 403, data: { error: 'Invalid CSRF token' } };
  }
  gatewayToken = `t${++rotations}`;
  const reply = (status, data) => ({ status, data: { ...data, receiver_execution: 'UNVERIFIED',
    csrf_token: gatewayToken } });
  if (override) return reply(override.status, override.data);
  const { action, hold_id: holdId } = record.body;
  if (action === 'START') {
    if (hold) return reply(409, { result: 'CD_BUSY' });
    hold = { id: `h${++holdCount}`, direction: record.body.direction };
    return reply(200, { result: 'SEEK_STARTED', direction: hold.direction, hold_id: hold.id });
  }
  if (action === 'HEARTBEAT') {
    return hold && hold.id === holdId ? reply(200, { result: 'LEASE_RENEWED' })
      : reply(409, { result: 'NOT_ACTIVE', cd_last_result: 'LEASE_EXPIRED' });
  }
  if (!hold) return reply(200, { result: 'NOT_ACTIVE', cd_last_result: 'LEASE_EXPIRED' });
  if (holdId !== undefined && holdId !== hold.id) return reply(409, { result: 'NOT_OWNER' });
  hold = null;
  return reply(200, { result: 'RELEASED', gpio_tx: 'TX_OK', waveform: 'MATCH' });
}
const respond = (r) => ({ ok: r.status < 300, status: r.status, json: async () => r.data });

const context = {
  AbortController, document, Date: { now: () => now },
  setTimeout: fakeSetTimeout, clearTimeout: fakeClearTimeout,
  // The app's 5 s refresh poll, on the fake clock.
  setInterval(handler, ms) {
    const tick = () => { handler(); fakeSetTimeout(tick, ms); };
    fakeSetTimeout(tick, ms);
  },
  addEventListener(type, handler) { (windowListeners[type] ||= []).push(handler); },
  async fetch(url, options = {}) {
    if (network === 'down') throw new Error('offline');
    if (url === '/api/v1/status') {
      return respond({ status: 200, data: { gateway: 'online', xs8_service: 'connected', status: {
        state: 'IDLE', monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
        last_byte: '0x72', last_gpio_tx: 'TX_OK', last_waveform: 'MATCH', cd_state: cdState,
        cd_action: 'NONE', cd_release_owed: '0', cd_lease_expired: '0', cd_last_result: 'NONE' } } });
    }
    if (url === '/api/v1/session') {
      sessionGets += 1;
      return respond({ status: 200, data: { csrf_token: gatewayToken, mute_available: false,
        source_available: false, receiver_query_available: false, cd_track_available: true,
        volume_step_available: true, cd_playback_available: playbackAvailable,
        cd_seek_available: seekAvailable } });
    }
    if (url === '/api/v1/capabilities') {
      return respond({ status: 200, data: { operations: {
        cd_transport: { enabled: true, available_actions: ['NEXT', 'PREVIOUS'], min_interval_ms: 250 },
        volume_step: { enabled: true, available_directions: ['UP', 'DOWN'], min_interval_ms: 250 },
        cd_playback: { enabled: playbackAvailable, available_actions: playbackActions },
        cd_seek: { enabled: seekAvailable, available_directions: ['FORWARD', 'BACKWARD'],
          heartbeat_ms: 500, lease_ms: 2000, max_hold_ms: 10000 } } } });
    }
    if (url === '/api/v1/cd/playback') {
      const record = { body: JSON.parse(options.body), token: options.headers['X-CSRF-Token'] };
      playbackPosts.push(record);
      if (record.token !== gatewayToken) {
        forbidden.push(record);
        return respond({ status: 403, data: { error: 'Invalid CSRF token' } });
      }
      gatewayToken = `t${++rotations}`;
      const plan = playbackPlans.shift();
      if (plan) return respond({ status: plan.status, data: { ...plan.data, action: record.body.action,
        receiver_execution: 'UNVERIFIED' } });
      return respond({ status: 200, data: { result: 'SERVICE_TX_OK', action: record.body.action,
        receiver_execution: 'UNVERIFIED' } });
    }
    if (url === '/api/v1/cd/seek') {
      const plan = plans.shift();
      const record = { body: JSON.parse(options.body), token: options.headers['X-CSRF-Token'] };
      requests.push(record);
      if (plan === 'drop') {
        gatewaySeek(record);
        throw new Error('response lost');
      }
      if (plan === 'defer') {
        return new Promise((resolve, reject) => {
          options.signal.addEventListener('abort', () => reject(new Error('aborted')));
          deferred.push(() => resolve(respond(gatewaySeek(record))));
        });
      }
      return respond(gatewaySeek(record, plan));
    }
    throw new Error(`unexpected request ${url}`);
  },
};

const actions = () => requests.map((r) => r.body.action);
const holdIds = () => requests.map((r) => r.body.hold_id);
function reset() {
  requests.length = 0;
  plans.length = 0;
  hold = null;
  network = 'up';
}
async function idle() {
  // Let the finishing refresh (or, after an outage, the next poll) run and the
  // CD spacing pass.
  await advance(1000);
  if (ff.disabled) await advance(5000);
  assert.equal(ff.disabled, false, `ready again: ${availability()}`);
}

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(ff.disabled, false);
  assert.equal(rew.disabled, false);
  assert.equal(playPause.disabled, false);
  assert.equal(stop.disabled, false);
  assert.equal(requests.length, 0, 'loading never seeks');

  // 1. Hold: START, heartbeats with successor tokens, RELEASE on pointerup.
  await ff.fire('pointerdown');
  await settle();
  assert.equal(ff.captured, 7, 'pointer captured');
  assert.equal(ff.disabled, false, 'the held button stays receptive');
  for (const other of [rew, playPause, stop, next, volumeUp]) {
    assert.equal(other.disabled, true, `${other.id} disabled while seeking`);
  }
  assert.match(availability(), /^FF SEEKING/);
  await advance(1600);
  assert.deepEqual(actions(), ['START', 'HEARTBEAT', 'HEARTBEAT', 'HEARTBEAT']);
  assert.deepEqual(holdIds().slice(1), ['h1', 'h1', 'h1']);
  // Clicking other CD controls during the hold sends nothing.
  await next.fire('click');
  await playPause.fire('click');
  await ff.fire('pointerup');
  await settle();
  assert.deepEqual(actions(), ['START', 'HEARTBEAT', 'HEARTBEAT', 'HEARTBEAT', 'RELEASE']);
  assert.equal(requests[4].body.hold_id, 'h1');
  assert.equal(hold, null);
  assert.equal(ff.captured, null, 'capture released');
  assert.match(availability(), /^FF RELEASE TX VERIFIED · SEEK RESULT UNVERIFIED/);
  await advance(5000);
  assert.equal(requests.length, 5, 'no heartbeat after release');
  assert.equal(playbackPosts.length, 0);
  assert.deepEqual(forbidden, []);
  await idle();

  // 2. Pointer released before the START response: the delayed START must not
  //    leave a seek running. RELEASE follows it at once; no heartbeat is sent.
  reset();
  plans.push('defer');
  await rew.fire('pointerdown');
  await rew.fire('pointerup');
  await settle();
  assert.deepEqual(actions(), ['START']);
  deferred.shift()();
  await settle();
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  assert.equal(requests[1].body.hold_id, 'h2');
  assert.equal(hold, null);
  await advance(3000);
  assert.equal(requests.length, 2);
  await idle();

  // 3. START response lost although the Gateway started the hold: the browser
  //    cannot name it, so it recovers a token and releases this session's hold.
  reset();
  plans.push('drop');
  const gets = sessionGets;
  await ff.fire('pointerdown');
  await settle();
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  assert.equal(requests[1].body.hold_id, undefined, 'session-scoped release');
  assert.equal(sessionGets, gets + 2, 'token recovered by GET (plus the final refresh)');
  assert.equal(hold, null);
  assert.match(availability(), /^FF START UNCERTAIN · RELEASE TX VERIFIED · CHECK CHANGER/);
  await ff.fire('pointerup');
  await advance(3000);
  assert.deepEqual(actions(), ['START', 'RELEASE'], 'START is never retried');
  await idle();

  // 4. Lost heartbeat response: the next heartbeat recovers the rotated token.
  reset();
  await ff.fire('pointerdown');
  await settle();
  plans.push('drop');
  await advance(500);
  await advance(500);
  assert.deepEqual(actions(), ['START', 'HEARTBEAT', 'HEARTBEAT']);
  assert.deepEqual(forbidden, [], 'the recovered token was valid');
  assert.notEqual(hold, null);
  await ff.fire('pointerup');
  await settle();
  assert.equal(actions().at(-1), 'RELEASE');
  assert.equal(hold, null);
  await idle();

  // 5. Network disconnect during a seek: no START retry, heartbeats stop being
  //    deliverable, and pointerup makes exactly one RELEASE attempt.
  reset();
  await ff.fire('pointerdown');
  await settle();
  network = 'down';
  await advance(2000);
  assert.deepEqual(actions(), ['START'], 'nothing reaches the Gateway while offline');
  await ff.fire('pointerup');
  await settle();
  assert.match(availability(), /^FF RELEASE UNKNOWN · LEASE ENDS SEEK WITHIN 2 s · CHECK CHANGER/);
  network = 'up';
  hold = null; // The service lease ended the orphaned hold.
  await advance(5000);
  assert.deepEqual(actions(), ['START'], 'no automatic retry of any request');
  await idle();

  // 6. Repeated and stray pointer events: one START, one RELEASE.
  reset();
  await ff.fire('pointerdown');
  await ff.fire('pointerdown');
  await rew.fire('pointerdown');
  await settle();
  assert.deepEqual(actions(), ['START']);
  await ff.fire('pointerup');
  await ff.fire('lostpointercapture');
  fireWindow('pointerup');
  await ff.fire('pointerup');
  await settle();
  // A pointerdown inside the CD spacing interval after a seek is dropped.
  await ff.fire('pointerdown');
  await settle();
  await advance(2000);
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  await idle();

  // 7. Every cancellation source ends the hold with one RELEASE.
  const cancellations = [
    ['pointercancel', () => ff.fire('pointercancel')],
    ['lostpointercapture', () => ff.fire('lostpointercapture')],
    ['window blur', () => fireWindow('blur')],
    ['window pointerup', () => fireWindow('pointerup')],
    ['visibility hidden', () => { document.hidden = true; fireDocument('visibilitychange'); }],
    ['pagehide', () => fireWindow('pagehide')],
  ];
  for (const [name, cancel] of cancellations) {
    reset();
    document.hidden = false;
    await ff.fire('pointerdown');
    await advance(600);
    await cancel();
    await settle();
    assert.deepEqual(actions(), ['START', 'HEARTBEAT', 'RELEASE'], name);
    await advance(3000);
    assert.equal(requests.length, 3, `${name}: no heartbeat after release`);
    document.hidden = false;
    await idle();
  }
  // Visible again does not cancel anything or start anything.
  fireDocument('visibilitychange');
  await settle();

  // 8. Lease expired server-side: the heartbeat reports NOT_ACTIVE; heartbeats
  //    stop and the stop-only RELEASE is answered without transmission.
  reset();
  await ff.fire('pointerdown');
  await settle();
  hold = null;
  await advance(500);
  assert.deepEqual(actions(), ['START', 'HEARTBEAT', 'RELEASE']);
  assert.match(availability(), /^FF ENDED · LEASE_EXPIRED/);
  await advance(3000);
  assert.equal(requests.length, 3);
  await ff.fire('pointerup');
  await idle();

  // 9. Client maximum: the browser releases at 10 s even if the pointer stays down.
  reset();
  await ff.fire('pointerdown');
  await advance(10500);
  const sent = actions();
  assert.equal(sent[0], 'START');
  assert.equal(sent.at(-1), 'RELEASE');
  assert.ok(sent.filter((a) => a === 'HEARTBEAT').length <= 20);
  assert.equal(hold, null);
  await advance(3000);
  assert.equal(requests.length, sent.length);
  await ff.fire('pointerup');
  await idle();

  // 10. START refused before transmission: nothing to release.
  reset();
  hold = { id: 'other' };
  await rew.fire('pointerdown');
  await settle();
  assert.deepEqual(actions(), ['START']);
  assert.match(availability(), /^REW CD_BUSY · NOTHING SENT/);
  await rew.fire('pointerup');
  await advance(2000);
  assert.deepEqual(actions(), ['START']);
  hold = null;
  await idle();

  // 11. START uncertain (the service may have pressed): one stop-only RELEASE,
  //     never another START.
  reset();
  plans.push({ status: 502, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } });
  await rew.fire('pointerdown');
  await settle();
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  assert.equal(requests[1].body.hold_id, undefined);
  assert.match(availability(), /^REW SERVICE_UNCERTAIN · CHECK CHANGER/);
  await advance(3000);
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  await rew.fire('pointerup');
  await idle();

  // 12. Uncertain RELEASE: fault shown, never resent.
  reset();
  await ff.fire('pointerdown');
  await settle();
  plans.push({ status: 502, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } });
  await ff.fire('pointerup');
  await settle();
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  assert.match(availability(), /^CD FAULT LATCHED · STOP AND INSPECT CHANGER/);
  await advance(3000);
  assert.deepEqual(actions(), ['START', 'RELEASE']);
  hold = null;
  await idle();

  // 13. PLAY/PAUSE and STOP: one POST per click, then the normal refresh.
  await playPause.fire('click');
  await settle();
  assert.deepEqual(playbackPosts.map((p) => p.body), [{ action: 'PLAY_PAUSE' }]);
  assert.match(availability(), /^PLAY\/PAUSE TX VERIFIED · CHANGER ACTION UNVERIFIED/);
  await playPause.fire('click');
  await settle();
  assert.equal(playbackPosts.length, 1, 'inside the CD spacing interval');
  await advance(1000);
  await stop.fire('click');
  await settle();
  assert.deepEqual(playbackPosts.map((p) => p.body.action), ['PLAY_PAUSE', 'STOP']);
  assert.deepEqual(forbidden, []);

  // 13b. TIME DISP., REPEAT and DISC SKIP: momentary, one exact POST per click.
  for (const [button, action, label] of momentary) {
    await advance(1000);
    assert.equal(button.disabled, false, `${action} enabled`);
    const before = playbackPosts.length;
    await button.fire('click');
    await settle();
    assert.deepEqual(playbackPosts.slice(before).map((p) => p.body), [{ action }]);
    assert.match(availability(), new RegExp(`^${label} TX VERIFIED · CHANGER ACTION UNVERIFIED`));
    // Nothing is inferred: no state attribute or pressed/active marking appears.
    assert.equal(button.dataset.active, undefined);
    assert.equal(button.dataset.state, undefined);
    assert.equal(button.ariaPressed, undefined);
    // A second click inside the shared CD spacing sends nothing.
    await button.fire('click');
    await settle();
    assert.equal(playbackPosts.length, before + 1, `${action} spacing`);
  }
  // Two REPEAT presses are two independent requests; nothing tracks ON/OFF.
  await advance(1000);
  await repeat.fire('click');
  await advance(1000);
  await repeat.fire('click');
  await settle();
  assert.deepEqual(playbackPosts.slice(-2).map((p) => p.body.action), ['REPEAT', 'REPEAT']);
  assert.match(availability(), /^REPEAT TX VERIFIED · CHANGER ACTION UNVERIFIED/);

  // 13c. Failures are reported, never retried; a refusal says nothing was sent.
  for (const [plan, pattern] of [
    [{ status: 502, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } },
      /^DISC SKIP SERVICE_UNCERTAIN · CHECK CHANGER/],
    [{ status: 409, data: { result: 'CD_BUSY' } }, /^DISC SKIP CD_BUSY · NOTHING SENT/],
    [{ status: 409, data: { result: 'CD_FAULT_LATCHED', physical_check_required: true } },
      /^CD FAULT LATCHED · STOP AND INSPECT CHANGER/]]) {
    await advance(1000);
    const before = playbackPosts.length;
    playbackPlans.push(plan);
    await discSkip.fire('click');
    await settle();
    assert.match(availability(), pattern);
    await advance(6000);
    assert.equal(playbackPosts.length, before + 1, 'never retried');
  }
  assert.deepEqual(forbidden, []);

  // 13d. A key the Gateway does not list stays disabled and sends nothing.
  playbackActions = ['PLAY_PAUSE', 'STOP'];
  await advance(6000);
  for (const [button] of momentary) {
    assert.equal(button.disabled, true, `${button.id} not offered`);
    const before = playbackPosts.length;
    await button.fire('click');
    await settle();
    assert.equal(playbackPosts.length, before);
  }
  assert.equal(stop.disabled, false, 'listed keys remain usable');
  playbackActions = ['PLAY_PAUSE', 'STOP', 'TIME_DISPLAY', 'REPEAT', 'DISC_SKIP'];
  await advance(6000);

  // 14. Unavailable capability or a CD hold keeps the controls disabled.
  for (const change of [() => { seekAvailable = false; playbackAvailable = false; },
    () => { cdState = 'HOLDING'; }]) {
    seekAvailable = true; playbackAvailable = true; cdState = 'IDLE';
    change();
    await advance(6000);
    for (const button of [ff, rew, playPause, stop, timeDisplay, repeat, discSkip]) {
      assert.equal(button.disabled, true, button.id);
    }
    const before = requests.length + playbackPosts.length;
    await ff.fire('pointerdown');
    await playPause.fire('click');
    await settle();
    assert.equal(requests.length + playbackPosts.length, before);
  }
  process.stdout.write('Web CD seek and playback offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
