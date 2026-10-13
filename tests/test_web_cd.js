const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const html = fs.readFileSync('web/index.html', 'utf8');
assert.match(html, /id="cd-previous" disabled type="button" data-cd-action="PREVIOUS"/);
assert.match(html, /id="cd-next" disabled type="button" data-cd-action="NEXT"/);
// PLAY/PAUSE, STOP and FF/REW have their own data attributes and endpoints;
// only PREV/NEXT use the CD track path.
assert.match(html, /id="cd-play-pause" disabled type="button" data-cd-playback="PLAY_PAUSE"/);
assert.match(html, /id="cd-stop" disabled type="button" data-cd-playback="STOP"/);
assert.equal((html.match(/data-cd-action=/g) || []).length, 2);

const posts = [];
const elements = new Map();
const stub = () => ({ disabled: true, addEventListener() {} });
const cd = {
  'cd-previous': { dataset: { cdAction: 'PREVIOUS' }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } },
  'cd-next': { dataset: { cdAction: 'NEXT' }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } },
};
const mute = stub();
const sync = stub();
let cdState = 'IDLE';
let releaseOwed = '0';
let cdAvailable = true;
let cdStatusFields = true;
let postReply = { ok: true, data: { result: 'SERVICE_TX_OK', action: 'NEXT',
  cd_result: 'TX_VERIFIED', receiver_execution: 'UNVERIFIED' } };
let postThrows = false;
let now = 10000;
let poll;

const context = {
  AbortController, setTimeout, clearTimeout,
  Date: { now: () => now },
  setInterval(handler) { poll = handler; },
  document: {
    getElementById(id) {
      if (id === 'mute-button') return mute;
      if (id === 'sync-button') return sync;
      if (cd[id]) return cd[id];
      if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
      return elements.get(id);
    },
    querySelectorAll: () => [],
  },
  async fetch(url, options) {
    if (url === '/api/v1/status') {
      const status = { state: cdState === 'FAULT_LATCHED' ? 'FAULT_LATCHED' : 'IDLE',
        monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0',
        last_byte: '0x72', last_gpio_tx: 'TX_OK', last_waveform: 'MATCH' };
      if (cdStatusFields) Object.assign(status, { cd_state: cdState, cd_action: 'NONE',
        cd_release_owed: releaseOwed, cd_lease_expired: '0', cd_last_result: 'NONE' });
      return { ok: true, json: async () => ({ gateway: 'online', xs8_service: 'connected', status }) };
    }
    if (url === '/api/v1/session') return { ok: true, json: async () => ({
      csrf_token: 'token', mute_available: false, source_available: false,
      receiver_query_available: false, cd_track_available: cdAvailable }) };
    if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
      operations: { cd_transport: { enabled: cdAvailable,
        available_actions: ['NEXT', 'PREVIOUS'] } } }) };
    if (url === '/api/v1/cd/track') {
      posts.push(options);
      if (postThrows) throw new Error('network');
      return { ok: postReply.ok, json: async () => postReply.data };
    }
    throw new Error(`unexpected request ${url}`);
  },
};

async function settle() { for (let i = 0; i < 5; i += 1) await new Promise(setImmediate); }
const availability = () => elements.get('availability-text').textContent;

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(posts.length, 0, 'loading the page must not dispatch');
  assert.equal(cd['cd-next'].disabled, false);
  assert.equal(cd['cd-previous'].disabled, false);
  assert.match(elements.get('cd-state').textContent, /^IDLE/);

  await Promise.all([cd['cd-next'].click(), cd['cd-next'].click()]);
  assert.equal(posts.length, 1, 'double click must dispatch once');
  assert.deepEqual(JSON.parse(posts[0].body), { action: 'NEXT' });
  assert.equal(posts[0].headers['X-CSRF-Token'], 'token');
  assert.equal(availability(), 'NEXT TX VERIFIED · TRACK CHANGE UNVERIFIED');
  await cd['cd-previous'].click();
  assert.equal(posts.length, 1, 'CD actions share a client-side 1 s interval');

  now += 1001;
  postReply = { ok: false, data: { result: 'SERVICE_UNCERTAIN', physical_check_required: true,
    receiver_execution: 'UNVERIFIED' } };
  await cd['cd-previous'].click();
  assert.deepEqual(JSON.parse(posts[1].body), { action: 'PREVIOUS' });
  assert.equal(availability(), 'PREV SERVICE_UNCERTAIN · CHECK CHANGER');

  now += 1001;
  postReply = { ok: false, data: { result: 'NOT_ELIGIBLE', receiver_execution: 'UNVERIFIED' } };
  await cd['cd-next'].click();
  assert.equal(availability(), 'NEXT NOT_ELIGIBLE · NOTHING SENT');

  now += 1001;
  postThrows = true;
  await cd['cd-next'].click();
  assert.equal(availability(), 'CD RESULT UNKNOWN · CHECK CHANGER');
  postThrows = false;

  for (const [state, owed] of [['FAULT_LATCHED', '1'], ['IDLE', '1']]) {
    cdState = state; releaseOwed = owed;
    poll();
    await settle();
    assert.equal(cd['cd-next'].disabled, true);
    assert.equal(availability(), 'CD FAULT LATCHED · STOP AND INSPECT CHANGER');
  }

  cdState = 'HOLDING'; releaseOwed = '1';
  poll();
  await settle();
  assert.equal(cd['cd-next'].disabled, true);
  assert.doesNotMatch(availability(), /FAULT/, 'an active hold is not a fault');

  cdState = 'IDLE'; releaseOwed = '0'; cdStatusFields = false;
  poll();
  await settle();
  assert.equal(cd['cd-next'].disabled, true, 'missing CD status fields keep CD disabled');

  cdStatusFields = true; cdAvailable = false;
  poll();
  await settle();
  assert.equal(cd['cd-next'].disabled, true);
  assert.equal(cd['cd-previous'].disabled, true);
  assert.equal(posts.length, 4);
  process.stdout.write('Web CD track offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
