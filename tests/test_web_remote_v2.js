const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('web/index.html', 'utf8');
const css = fs.readFileSync('web/app.css', 'utf8');
assert.match(html, /id="power-on" disabled type="button" data-power-action="ON">ON</);
assert.match(html, /id="power-off" disabled type="button" data-power-action="OFF">OFF</);
assert.match(css, /#monitor-button\[data-monitor-state="ON"\] \{ background:#2f7d4f/);
assert.match(css, /#monitor-button\[data-monitor-state="OFF"\] small::after \{ content:" OFF"; \}/);

// ---- Fake clock ----
let now = 100000;
let nextTimer = 1;
const timers = new Map();
const fakeSetTimeout = (handler, ms) => {
  const id = nextTimer++;
  timers.set(id, { handler, at: now + (ms || 0) });
  return id;
};
const fakeClearTimeout = (id) => timers.delete(id);
async function settle() { for (let i = 0; i < 12; i += 1) await new Promise(setImmediate); }
async function advance(ms) {
  await settle();
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

// ---- Elements ----
const elements = new Map();
const datasets = {
  'volume-up': { volumeDirection: 'UP' }, 'volume-down': { volumeDirection: 'DOWN' },
  'power-on': { powerAction: 'ON' }, 'power-off': { powerAction: 'OFF' },
  'monitor-button': { monitorState: 'UNKNOWN' },
};
function element(id) {
  if (!elements.has(id)) {
    elements.set(id, {
      id, textContent: '', disabled: true, dataset: { ...(datasets[id] || {}) }, listeners: {},
      captured: null,
      addEventListener(type, handler) { (this.listeners[type] ||= []).push(handler); },
      fire(type, event = {}) {
        const full = { pointerId: 3, button: 0, preventDefault() {}, ...event };
        return Promise.all((this.listeners[type] || []).map((handler) => handler(full)));
      },
      setPointerCapture(pointer) { this.captured = pointer; },
      releasePointerCapture() { this.captured = null; },
    });
  }
  return elements.get(id);
}
const sourceButtons = ['CD', 'TUNER', 'PHONO', 'VIDEO1', 'VIDEO2', 'TAPE1'].map((source) => {
  const button = element(`source-${source}`);
  button.dataset.source = source;
  return button;
});
const up = element('volume-up');
const down = element('volume-down');
const monitor = element('monitor-button');
const powerOn = element('power-on');
const powerOff = element('power-off');
const powerButton = element('power-button');
element('power-menu').hidden = true; // As in the markup.
const windowListeners = {};
const documentListeners = {};
const document = {
  hidden: false, getElementById: element, querySelectorAll: () => sourceButtons,
  addEventListener(type, handler) { (documentListeners[type] ||= []).push(handler); },
};
const fireDocument = (type) => (documentListeners[type] || []).forEach((handler) => handler({}));
const availability = () => element('availability-text').textContent;

// ---- Fake Gateway ----
let gatewayToken = 't0';
let rotations = 0;
let monitorReport = { value: 'UNKNOWN', freshness: 'UNKNOWN' };
let integrity = 'HEALTHY';
let connected = true;
let flags = { hold: true, power: true, monitor: true, step: true };
let holdMaxMs = 20000;
let hold = null;
let holds = 0;
let network = 'up';
const plans = [];
const deferred = [];
const requests = [];
const forbidden = [];
function gateway(url, record, plan) {
  if (record.token !== gatewayToken) {
    forbidden.push(record);
    return { status: 403, data: { error: 'Invalid CSRF token' } };
  }
  gatewayToken = `t${++rotations}`;
  const token = { csrf_token: gatewayToken, receiver_execution: 'UNVERIFIED' };
  if (plan && plan.status) return { status: plan.status, data: { ...plan.data, ...token } };
  if (url !== '/api/v1/volume/hold') {
    return { status: 200, data: { result: 'SERVICE_TX_OK', receiver_execution: 'UNVERIFIED' } };
  }
  const { action, hold_id: holdId } = record.body;
  if (action === 'START') {
    if (hold) return { status: 409, data: { result: 'VOLUME_BUSY', ...token } };
    hold = { id: `v${++holds}` };
    return { status: 200, data: { result: 'HOLD_STARTED', hold_id: hold.id, ...token } };
  }
  if (action === 'HEARTBEAT') {
    return hold && hold.id === holdId ? { status: 200, data: { result: 'LEASE_RENEWED', ...token } }
      : { status: 409, data: { result: 'NOT_ACTIVE', ...token } };
  }
  if (!hold) return { status: 200, data: { result: 'NOT_ACTIVE', ...token } };
  hold = null;
  return { status: 200, data: { result: 'RELEASED', ...token } };
}
const respond = (r) => ({ ok: r.status < 300, status: r.status, json: async () => r.data });
const context = {
  AbortController, document, Date: { now: () => now },
  setTimeout: fakeSetTimeout, clearTimeout: fakeClearTimeout,
  setInterval(handler, ms) {
    const tick = () => { handler(); fakeSetTimeout(tick, ms); };
    fakeSetTimeout(tick, ms);
  },
  addEventListener(type, handler) { (windowListeners[type] ||= []).push(handler); },
  async fetch(url, options = {}) {
    if (network === 'down') throw new Error('offline');
    if (url === '/api/v1/status') {
      if (!connected) return respond({ status: 503, data: { gateway: 'online', xs8_service: 'unavailable' } });
      return respond({ status: 200, data: { gateway: 'online', xs8_service: 'connected', status: {
        state: 'IDLE', monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
        last_byte: '0x07', last_gpio_tx: 'TX_OK', last_waveform: 'MATCH', cd_state: 'IDLE',
        cd_action: 'NONE', cd_release_owed: '0', cd_lease_expired: '0', cd_last_result: 'NONE' },
        receiver: { source: { value: 'VIDEO1', freshness: 'FRESH' }, monitor: monitorReport,
          integrity, receiver_execution: 'UNVERIFIED' } } });
    }
    if (url === '/api/v1/session') {
      return respond({ status: 200, data: { csrf_token: gatewayToken, mute_available: false,
        source_available: true, receiver_query_available: false, cd_track_available: false,
        volume_step_available: flags.step, volume_hold_available: flags.hold,
        power_available: flags.power, tape2_monitor_available: flags.monitor } });
    }
    if (url === '/api/v1/capabilities') {
      return respond({ status: 200, data: { operations: {
        source: { enabled: true, available_sources: ['CD', 'TUNER', 'PHONO', 'VIDEO1', 'VIDEO2', 'TAPE1'] },
        volume_step: { enabled: flags.step, available_directions: ['UP', 'DOWN'], min_interval_ms: 250 },
        volume_hold: { enabled: flags.hold, available_directions: ['UP', 'DOWN'], hold_delay_ms: 400,
          heartbeat_ms: 500, lease_ms: 1200, max_hold_ms: holdMaxMs },
        power: { enabled: flags.power, available_actions: ['ON', 'OFF'] },
        tape2_monitor: { enabled: flags.monitor, provisional: true } } } });
    }
    const record = { url, body: JSON.parse(options.body), token: options.headers['X-CSRF-Token'], at: now };
    requests.push(record);
    const plan = plans.shift();
    if (plan === 'drop') {
      gateway(url, record);
      throw new Error('response lost');
    }
    if (plan === 'defer') {
      return new Promise((resolve, reject) => {
        options.signal.addEventListener('abort', () => reject(new Error('aborted')));
        deferred.push(() => resolve(respond(gateway(url, record))));
      });
    }
    return respond(gateway(url, record, plan));
  },
};

const sent = () => requests.map((r) => `${r.url.replace('/api/v1/', '')}${r.body.action ? ` ${r.body.action}` : ''}`);
function reset() { requests.length = 0; plans.length = 0; hold = null; network = 'up'; }
async function idle() { await advance(1500); }

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(requests.length, 0, 'loading sends nothing');

  // 1. VIDEO1/VIDEO2 are ordinary source buttons now; one POST per click.
  const tv1 = sourceButtons[3];
  assert.equal(tv1.disabled, false);
  assert.equal(sourceButtons[4].disabled, false);
  await tv1.fire('click');
  await settle();
  assert.deepEqual(requests.map((r) => [r.url, r.body]), [['/api/v1/source', { source: 'VIDEO1' }]]);
  await idle();

  // 2. MONITOR indicator: only fresh, confirmed reports colour it.
  const shown = async (report, extra = {}) => {
    monitorReport = report;
    integrity = extra.integrity || 'HEALTHY';
    connected = extra.connected !== false;
    await advance(5000);
    return monitor.dataset.monitorState;
  };
  assert.equal(await shown({ value: 'ON', freshness: 'FRESH' }), 'ON');
  assert.equal(await shown({ value: 'OFF', freshness: 'FRESH' }), 'OFF');
  assert.equal(await shown({ value: 'ON', freshness: 'LAST_KNOWN' }), 'UNKNOWN');
  assert.equal(await shown({ value: 'UNKNOWN', freshness: 'UNKNOWN' }), 'UNKNOWN');
  assert.equal(await shown({ value: 'ON', freshness: 'FRESH' }, { integrity: 'UNCERTAIN' }), 'UNKNOWN');
  assert.equal(await shown({ value: 'ON', freshness: 'FRESH' }, { connected: false }), 'UNKNOWN');
  // A verified toggle TX never changes the indicator.
  assert.equal(await shown({ value: 'OFF', freshness: 'FRESH' }), 'OFF');
  reset();
  assert.equal(monitor.disabled, false);
  await monitor.fire('click');
  await settle();
  assert.deepEqual(requests.map((r) => [r.url, r.body]), [['/api/v1/receiver/monitor', {}]]);
  assert.match(availability(), /^MONITOR TOGGLE TX VERIFIED · STATE SHOWN ONLY WHEN OBSERVED/);
  assert.equal(monitor.dataset.monitorState, 'OFF', 'still the last observed report');
  await idle();

  // 3. POWER: the round button opens the chooser; one POST per explicit choice,
  //    never a state claim.
  reset();
  await powerButton.fire('click');
  await powerOn.fire('click');
  await settle();
  assert.match(availability(), /^POWER ON TX VERIFIED · RECEIVER POWER UNVERIFIED/);
  await idle();
  await powerButton.fire('click');
  await powerOff.fire('click');
  await settle();
  assert.deepEqual(requests.map((r) => [r.url, r.body]),
    [['/api/v1/power', { action: 'ON' }], ['/api/v1/power', { action: 'OFF' }]]);
  await idle();

  // 4. Volume tap (< 400 ms): exactly one VOLUME_STEP, no hold.
  reset();
  await up.fire('pointerdown');
  await advance(150);
  await up.fire('pointerup');
  await up.fire('click');
  await advance(2000);
  assert.deepEqual(sent(), ['volume/step']);
  assert.equal(requests[0].body.direction, 'UP');
  await idle();

  // 5. Volume hold: START after 400 ms, heartbeats, RELEASE on pointerup; the
  //    gesture's click is suppressed, so no step is added.
  reset();
  await down.fire('pointerdown');
  await advance(399);
  assert.deepEqual(sent(), []);
  await advance(1);
  assert.deepEqual(sent(), ['volume/hold START']);
  assert.equal(requests[0].body.direction, 'DOWN');
  assert.equal(down.disabled, false, 'the held button stays receptive');
  assert.equal(up.disabled, true);
  assert.equal(powerOff.disabled, true, 'nothing else during a hold');
  await advance(1100);
  await down.fire('pointerup');
  await down.fire('click');
  await settle();
  assert.deepEqual(sent(), ['volume/hold START', 'volume/hold HEARTBEAT', 'volume/hold HEARTBEAT',
    'volume/hold RELEASE']);
  assert.match(availability(), /^VOL− RELEASE TX VERIFIED · VOLUME RESULT UNVERIFIED/);
  await advance(3000);
  assert.equal(requests.length, 4, 'no step, no heartbeat after release');
  await idle();

  // 5b. The hold ends on its own at the 20 s maximum while the finger stays down
  //     for 22 s: heartbeats every 500 ms keep it alive to 19,999 ms, RELEASE
  //     comes at 20,000 ms, and the late release and its click add no step.
  const holdUntilMaximum = async (button, label) => {
    reset();
    await button.fire('pointerdown');
    await advance(400);
    const started = now;
    await advance(19999);
    const beats = requests.filter((r) => r.body.action === 'HEARTBEAT');
    assert.deepEqual(sent(), ['volume/hold START', ...beats.map(() => 'volume/hold HEARTBEAT')],
      `${label}: no RELEASE at 19,999 ms`);
    assert.equal(beats.length, 39);
    beats.forEach((beat, index) => assert.equal(beat.at - started, 500 * (index + 1),
      'one heartbeat every 500 ms, inside the 1200 ms service inactivity limit'));
    assert.notEqual(hold, null);
    await advance(1);
    assert.equal(sent().at(-1), 'volume/hold RELEASE', `${label}: released at 20,000 ms`);
    assert.equal(requests.at(-1).at - started, 20000);
    assert.equal(hold, null);
    const count = requests.length;
    await advance(2000);
    await button.fire('pointerup');
    await button.fire('click');
    await advance(3000);
    assert.equal(requests.length, count, 'no step and no restart after the maximum');
    await idle();
  };
  await holdUntilMaximum(up, 'max 20000');
  // A capability above 20 s is not trusted: the browser keeps its 20 s bound.
  holdMaxMs = 20001;
  await advance(6000);
  await holdUntilMaximum(down, 'max 20001');
  holdMaxMs = 20000;
  await advance(6000);

  // 6. Release before the START response: RELEASE as soon as START answers.
  reset();
  plans.push('defer');
  await up.fire('pointerdown');
  await advance(400);
  await up.fire('pointerup');
  await up.fire('click');
  deferred.shift()();
  await settle();
  assert.deepEqual(sent(), ['volume/hold START', 'volume/hold RELEASE']);
  assert.equal(hold, null);
  await advance(3000);
  assert.equal(requests.length, 2);
  await idle();

  // 7. Lost START response: one session-scoped RELEASE, never another START.
  reset();
  plans.push('drop');
  await up.fire('pointerdown');
  await advance(400);
  await settle();
  assert.deepEqual(sent(), ['volume/hold START', 'volume/hold RELEASE']);
  assert.equal(requests[1].body.hold_id, undefined);
  assert.equal(hold, null);
  await up.fire('pointerup');
  await up.fire('click');
  await advance(3000);
  assert.equal(requests.length, 2);
  await idle();

  // 8. Cancelled before the hold threshold: nothing at all is sent.
  for (const cancel of [() => up.fire('pointercancel'), () => up.fire('lostpointercapture'),
    () => { document.hidden = true; fireDocument('visibilitychange'); },
    () => (windowListeners.blur || []).forEach((handler) => handler({}))]) {
    reset();
    document.hidden = false;
    await up.fire('pointerdown');
    await advance(200);
    await cancel();
    await advance(1000);
    assert.deepEqual(sent(), []);
    document.hidden = false;
    await idle();
  }

  // 9. Uncertain START (5xx): one release, no retry, no step.
  reset();
  plans.push({ status: 502, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true } });
  await up.fire('pointerdown');
  await advance(400);
  await settle();
  await up.fire('pointerup');
  await up.fire('click');
  await advance(3000);
  assert.deepEqual(sent(), ['volume/hold START', 'volume/hold RELEASE']);
  assert.match(availability(), /^VOL\+ SERVICE_UNCERTAIN · CHECK RECEIVER/);
  assert.deepEqual(forbidden, []);
  await idle();

  // 10. Capabilities off: controls disabled; a long press only steps on click.
  flags = { hold: false, power: false, monitor: false, step: true };
  await advance(6000);
  assert.equal(powerOn.disabled, true);
  assert.equal(powerOff.disabled, true);
  assert.equal(powerButton.disabled, true);
  assert.equal(monitor.disabled, true);
  reset();
  await up.fire('pointerdown');
  await advance(800);
  await up.fire('pointerup');
  await up.fire('click');
  await settle();
  assert.deepEqual(sent(), ['volume/step'], 'without hold, a long press is still one tap');
  process.stdout.write('Web remote v2 offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
