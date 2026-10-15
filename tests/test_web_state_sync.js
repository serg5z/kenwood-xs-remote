const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Regression tests for two observed state-synchronization reports:
// 1. PHONO -> VIDEO1/VIDEO2 changed the receiver but the source indicator stayed stale
//    until SYNC (the service dropped the receiver's broadcast; see
//    xs8_receiver_state_test). The Web must show the service's new observation
//    after the selection without SYNC, and never infer it from TX evidence.
// 2. After SYNC the MONITOR button was a dark fill, after a reload light: the
//    same receiver report must render identically on page load, after SYNC and
//    on later polls, and a fresh OFF must not use a fill that flips with age.

const css = fs.readFileSync('web/app.css', 'utf8');
const app = fs.readFileSync('web/app.js', 'utf8');

const fresh = (value) => ({ value, freshness: 'FRESH' });

function page(state) {
  const elements = new Map();
  const sources = ['CD', 'TUNER', 'PHONO', 'VIDEO1', 'VIDEO2', 'TAPE1'].map((source) => ({
    dataset: { source }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; },
  }));
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, { id, textContent: '', disabled: true, dataset: {},
        addEventListener(_event, handler) { this.click = handler; } });
    }
    return elements.get(id);
  };
  element('monitor-button').dataset.monitorState = 'UNKNOWN';
  const ui = { poll: null, posts: [], sources, element };
  const context = {
    AbortController, Date, setTimeout, clearTimeout,
    setInterval(handler) { ui.poll = handler; },
    document: {
      getElementById: element,
      querySelectorAll: (selector) => (selector.includes('data-source') ? sources : []),
    },
    async fetch(url, options = {}) {
      if (url === '/api/v1/status') return { ok: true, json: async () => ({
        gateway: 'online', xs8_service: 'connected', receiver: state.receiver,
        status: { state: 'IDLE', monitor: 'HEALTHY', stop_owed: '0', cd_state: 'IDLE',
          cd_release_owed: '0', context_may_be_open: '0', last_byte: '0x07', last_gpio_tx: 'TX_OK',
          last_waveform: 'MATCH' } }) };
      if (url === '/api/v1/session') return { ok: true, json: async () => ({
        csrf_token: `token-${state.tokens += 1}`, source_available: true,
        receiver_query_available: true, tape2_monitor_available: true }) };
      if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
        operations: { source: { enabled: true,
          available_sources: ['CD', 'TUNER', 'PHONO', 'VIDEO1', 'VIDEO2', 'TAPE1'] },
        receiver_query: { enabled: true }, tape2_monitor: { enabled: true } } }) };
      if (options.method === 'POST') {
        ui.posts.push(url);
        const reply = state.onPost(url, JSON.parse(options.body));
        return { ok: true, json: async () => reply };
      }
      throw new Error(`unexpected ${url}`);
    },
  };
  vm.runInNewContext(app, context);
  return ui;
}

async function settle() { for (let i = 0; i < 8; i += 1) await new Promise(setImmediate); }
const sourceText = (ui) => ui.element('source-status').textContent;
const observed = (ui) => ui.sources.filter((b) => b.dataset.observed === 'true')
  .map((b) => b.dataset.source);

async function sourceAfterSelection() {
  const state = { tokens: 0, receiver: { source: fresh('PHONO'), monitor: fresh('OFF'),
    integrity: 'HEALTHY' } };
  const tx = { result: 'SERVICE_TX_OK', gpio_tx: 'TX_OK', waveform: 'MATCH',
    receiver_execution: 'UNVERIFIED' };
  const ui = page(state);
  await settle();
  assert.match(sourceText(ui), /^OBSERVED SOURCE PHONO · FRESH$/);
  const tv1 = ui.sources.find((b) => b.dataset.source === 'VIDEO1');
  const tv2 = ui.sources.find((b) => b.dataset.source === 'VIDEO2');

  // The service observed the receiver's VIDEO1 broadcast during the selection: the
  // refresh after the POST shows it, without SYNC.
  state.onPost = (url, body) => {
    assert.deepEqual([url, body], ['/api/v1/source', { source: 'VIDEO1' }]);
    state.receiver = { ...state.receiver, source: { ...fresh('VIDEO1'), evidence: 'AFTER_LOCAL_TX' } };
    return tx;
  };
  await tv1.click();
  await settle();
  assert.match(sourceText(ui), /^OBSERVED SOURCE VIDEO1 · FRESH$/);
  assert.deepEqual(observed(ui), ['VIDEO1']);
  assert.deepEqual(ui.posts, ['/api/v1/source'], 'no automatic SYNC or other POST');

  // No observation (the old behaviour): TX_OK/MATCH never moves the indicator.
  await new Promise((resolve) => setTimeout(resolve, 1050)); // Browser source spacing.
  state.onPost = () => tx;
  await tv2.click();
  await settle();
  assert.match(sourceText(ui), /^OBSERVED SOURCE VIDEO1 · FRESH$/);
  assert.deepEqual(observed(ui), ['VIDEO1']);
  assert.match(ui.element('availability-text').textContent, /VIDEO2 TX REPORTED · SELECTION UNVERIFIED/);
  // A later passive observation is picked up by the ordinary poll.
  state.receiver = { ...state.receiver, source: fresh('VIDEO2') };
  await ui.poll();
  await settle();
  assert.deepEqual(observed(ui), ['VIDEO2']);
  assert.deepEqual(ui.posts, ['/api/v1/source', '/api/v1/source']);
}

async function monitorConsistency() {
  const state = { tokens: 0, receiver: { source: fresh('CD'),
    monitor: { value: 'OFF', freshness: 'LAST_KNOWN' }, integrity: 'HEALTHY' } };
  state.onPost = (url) => {
    assert.equal(url, '/api/v1/receiver/query');
    state.receiver = { source: fresh('CD'), monitor: fresh('OFF'), integrity: 'HEALTHY' };
    return { result: 'OBSERVATION_RECORDED' };
  };
  const synced = page(state);
  await settle();
  const button = (ui) => ui.element('monitor-button').dataset.monitorState;
  assert.equal(button(synced), 'UNKNOWN', 'a stale report is neutral');
  await synced.element('sync-button').click();
  await settle();
  assert.equal(button(synced), 'OFF', 'SYNC shows the fresh OFF report');
  // A page reload with the same report renders the same state.
  const reloaded = page(state);
  await settle();
  assert.equal(button(reloaded), 'OFF', 'initial load matches SYNC');
  assert.equal(reloaded.element('monitor-status').textContent,
    synced.element('monitor-status').textContent);
  // Once the report ages, the poll and a reload both turn neutral together.
  state.receiver = { ...state.receiver, monitor: { value: 'OFF', freshness: 'LAST_KNOWN' } };
  await synced.poll();
  await settle();
  const later = page(state);
  await settle();
  assert.equal(button(synced), 'UNKNOWN');
  assert.equal(button(later), 'UNKNOWN');
  // Uncertain integrity is neutral even when fresh.
  state.receiver = { ...state.receiver, monitor: fresh('ON'), integrity: 'UNCERTAIN' };
  const uncertain = page(state);
  await settle();
  assert.equal(button(uncertain), 'UNKNOWN');

  // Rendering: OFF is unlit (no fill, the neutral button colours) and is told
  // apart only by its label, so ageing out never flips dark to light. ON is lit.
  const rule = (state) => (css.match(new RegExp(
    `#monitor-button\\[data-monitor-state="${state}"\\] \\{([^}]*)\\}`)) || [])[1] || '';
  assert.match(rule('ON'), /background:#2f7d4f/);
  assert.doesNotMatch(rule('OFF'), /background|border-color|color/);
  assert.doesNotMatch(css, /#9a9d97/);
  assert.match(css, /#monitor-button\[data-monitor-state="OFF"\] small::after \{ content:" OFF"; \}/);
  assert.match(css, /#monitor-button\[data-monitor-state="ON"\] small::after \{ content:" ON"; \}/);
}

// The Gateway ends a source or MONITOR control with SYNC. The indicators still
// follow only the service's recorded observation, read by the refresh after the
// POST: a recorded SYNC whose observation is not fresh and healthy stays neutral,
// and a failed SYNC leaves the last observation in place. Exactly one POST each.
async function automaticSync() {
  const state = { tokens: 0, receiver: { source: fresh('CD'), monitor: fresh('OFF'),
    integrity: 'HEALTHY' } };
  const tx = { result: 'SERVICE_TX_OK', gpio_tx: 'TX_OK', waveform: 'MATCH',
    receiver_execution: 'UNVERIFIED' };
  const ui = page(state);
  await settle();
  const button = () => ui.element('monitor-button').dataset.monitorState;
  const status = () => ui.element('availability-text').textContent;
  assert.equal(button(), 'OFF');

  // MONITOR -> SYNC recorded a fresh ON: shown from the observation.
  state.onPost = (url) => {
    assert.equal(url, '/api/v1/receiver/monitor');
    state.receiver = { ...state.receiver, monitor: { ...fresh('ON'), evidence: 'QUERY' } };
    return { ...tx, mapping: 'PROVISIONAL', sync: { result: 'OBSERVATION_RECORDED' } };
  };
  await ui.element('monitor-button').click();
  await settle();
  assert.equal(button(), 'ON');
  assert.match(status(), /^MONITOR TOGGLE TX VERIFIED · SYNC RECORDED · SEE OBSERVED MONITOR/);

  // SYNC failed: the TX result never moves the indicator; nothing is resent.
  await new Promise((resolve) => setTimeout(resolve, 1050));
  state.onPost = () => ({ ...tx, mapping: 'PROVISIONAL', sync: { result: 'SERVICE_UNCERTAIN' } });
  await ui.element('monitor-button').click();
  await settle();
  assert.equal(button(), 'ON');
  assert.match(status(), /SYNC SERVICE_UNCERTAIN · STATE SHOWN ONLY WHEN OBSERVED/);

  // SYNC recorded but the observation is uncertain: neutral, never inferred.
  state.onPost = () => {
    state.receiver = { ...state.receiver, monitor: fresh('OFF'), integrity: 'UNCERTAIN' };
    return { ...tx, mapping: 'PROVISIONAL', sync: { result: 'OBSERVATION_RECORDED' } };
  };
  await ui.element('monitor-button').click();
  await settle();
  assert.equal(button(), 'UNKNOWN');

  // Source: the recorded VIDEO1 observation is shown; a stale one is not fresh.
  state.receiver = { source: fresh('CD'), monitor: fresh('OFF'), integrity: 'HEALTHY' };
  await ui.poll();
  await settle();
  const video1 = ui.sources.find((b) => b.dataset.source === 'VIDEO1');
  state.onPost = (url, body) => {
    assert.deepEqual([url, body], ['/api/v1/source', { source: 'VIDEO1' }]);
    state.receiver = { ...state.receiver, source: { ...fresh('VIDEO1'), evidence: 'QUERY' } };
    return { ...tx, source: 'VIDEO1', cd_stop: tx, sync: { result: 'OBSERVATION_RECORDED' } };
  };
  await video1.click();
  await settle();
  assert.deepEqual(observed(ui), ['VIDEO1']);
  assert.match(sourceText(ui), /^OBSERVED SOURCE VIDEO1 · FRESH$/);
  await new Promise((resolve) => setTimeout(resolve, 1050));
  state.onPost = () => {
    state.receiver = { ...state.receiver, source: { value: 'VIDEO1', freshness: 'LAST_KNOWN' } };
    return { ...tx, sync: { result: 'OBSERVATION_FAILED' } };
  };
  await ui.sources.find((b) => b.dataset.source === 'TUNER').click();
  await settle();
  assert.doesNotMatch(sourceText(ui), /TUNER/);
  assert.doesNotMatch(sourceText(ui), /FRESH/);
  assert.deepEqual(ui.posts, ['/api/v1/receiver/monitor', '/api/v1/receiver/monitor',
    '/api/v1/receiver/monitor', '/api/v1/source', '/api/v1/source'], 'one POST per press');
}

// SYNC readback: the message names what the receiver reported (and a mismatch),
// a failure names the service's reason, and an observation made before the
// latest command is never highlighted, even if the service still reports it.
async function syncReadback() {
  const seen = (value, ns) => ({ value, freshness: 'FRESH', observed_mono_ns: ns });
  const state = { tokens: 0, receiver: { source: seen('CD', 100), monitor: seen('OFF', 100),
    integrity: 'HEALTHY' } };
  const tx = { result: 'SERVICE_TX_OK', gpio_tx: 'TX_OK', waveform: 'MATCH',
    receiver_execution: 'UNVERIFIED' };
  const ui = page(state);
  await settle();
  const status = () => ui.element('availability-text').textContent;
  const click = async (name) => {
    await ui.sources.find((b) => b.dataset.source === name).click();
    await settle();
  };
  const pause = () => new Promise((resolve) => setTimeout(resolve, 1050));
  assert.deepEqual(observed(ui), ['CD']);

  state.onPost = () => {
    state.receiver = { ...state.receiver, source: seen('TUNER', 600) };
    return { ...tx, sync: { result: 'OBSERVATION_RECORDED', observed_source: 'TUNER',
      observed_monitor: 'OFF', requested: 'TUNER', match: true, since_mono_ns: 500 } };
  };
  await click('TUNER');
  assert.equal(status(), 'TUNER TX REPORTED · RECEIVER REPORTS TUNER');
  assert.deepEqual(observed(ui), ['TUNER']);

  // Mismatch: the receiver's report is what is highlighted, with a warning.
  await pause();
  state.onPost = () => {
    state.receiver = { ...state.receiver, source: seen('CD', 1100) };
    return { ...tx, sync: { result: 'OBSERVATION_RECORDED', observed_source: 'CD',
      observed_monitor: 'OFF', requested: 'VIDEO1', match: false, since_mono_ns: 1000 } };
  };
  await click('VIDEO1');
  assert.equal(status(), 'VIDEO1 TX REPORTED · RECEIVER REPORTS CD · CHECK RECEIVER');
  assert.deepEqual(observed(ui), ['CD']);

  // Failed SYNC: the pre-command CD is still reported but is not current.
  await pause();
  state.onPost = () => ({ ...tx, sync: { result: 'OBSERVATION_FAILED', reason: 'NO_REPLY',
    requested: 'PHONO', match: null, since_mono_ns: 2000 } });
  await click('PHONO');
  assert.equal(status(), 'PHONO TX REPORTED · SYNC NO_REPLY · SELECTION UNVERIFIED');
  assert.deepEqual(observed(ui), [], 'nothing highlighted from before the command');
  assert.match(sourceText(ui), /^OBSERVED SOURCE UNKNOWN · AWAITING OBSERVATION$/);
  // A later observation (passive broadcast) is shown by the ordinary poll.
  state.receiver = { ...state.receiver, source: seen('PHONO', 2500) };
  await ui.poll();
  await settle();
  assert.deepEqual(observed(ui), ['PHONO']);
  assert.match(sourceText(ui), /^OBSERVED SOURCE PHONO · FRESH$/);

  // MONITOR: the same guard applies to the indicator.
  const monitor = () => ui.element('monitor-button').dataset.monitorState;
  assert.equal(monitor(), 'OFF');
  state.onPost = () => ({ ...tx, mapping: 'PROVISIONAL', sync: { result: 'OBSERVATION_FAILED',
    reason: 'BUS_ACTIVE', expected_monitor: 'ON', match: null, since_mono_ns: 3000 } });
  await ui.element('monitor-button').click();
  await settle();
  assert.equal(monitor(), 'UNKNOWN');
  assert.match(ui.element('monitor-status').textContent, /AWAITING OBSERVATION/);
  assert.equal(status(), 'MONITOR TOGGLE TX VERIFIED · SYNC BUS_ACTIVE · STATE SHOWN ONLY WHEN OBSERVED');
  state.onPost = () => {
    state.receiver = { ...state.receiver, monitor: seen('ON', 4100) };
    return { ...tx, mapping: 'PROVISIONAL', sync: { result: 'OBSERVATION_RECORDED',
      observed_source: 'PHONO', observed_monitor: 'ON', expected_monitor: null, match: null,
      since_mono_ns: 4000 } };
  };
  await ui.element('monitor-button').click();
  await settle();
  assert.equal(monitor(), 'ON');
  assert.equal(status(), 'MONITOR TOGGLE TX VERIFIED · RECEIVER REPORTS MONITOR ON');
  assert.equal(ui.posts.length, 5, 'one POST per press, never repeated');
}

async function main() {
  await sourceAfterSelection();
  await monitorConsistency();
  await automaticSync();
  await syncReadback();
  process.stdout.write('Web state synchronization offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
