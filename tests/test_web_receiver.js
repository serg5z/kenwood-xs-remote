const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const elements = new Map();
const sources = ['CD', 'VIDEO1', 'VIDEO2', 'TUNER', 'PHONO', 'TAPE1', 'TAPE2'].map(
  (source) => ({ dataset: { source }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } }));
const mute = { disabled: true, addEventListener(_event, handler) { this.click = handler; } };
const sync = { disabled: true, addEventListener(_event, handler) { this.click = handler; } };
let poll;
let token = 'token-1';
let serviceAvailable = true;
let receiver = {
  source: { value: 'UNKNOWN', freshness: 'UNKNOWN' },
  monitor: { value: 'UNKNOWN', freshness: 'UNKNOWN' }, integrity: 'HEALTHY',
};
let queryReply;
let queryFailure = null;
const posts = [];
const context = {
  AbortController, Date, setTimeout, clearTimeout,
  setInterval(handler) { poll = handler; },
  document: {
    getElementById(id) {
      if (id === 'mute-button') return mute;
      if (id === 'sync-button') return sync;
      if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
      return elements.get(id);
    },
    querySelectorAll() { return sources; },
  },
  async fetch(url, options) {
    if (url === '/api/v1/status') return {
      ok: serviceAvailable, json: async () => serviceAvailable ? {
        gateway: 'online', xs8_service: 'connected', receiver,
        status: { state: 'IDLE', monitor: 'HEALTHY', stop_owed: '0',
          context_may_be_open: '0', last_byte: 'NONE', last_gpio_tx: 'NONE',
          last_waveform: 'NOT_OBSERVED' },
      } : { gateway: 'online', xs8_service: 'unavailable' },
    };
    if (url === '/api/v1/session') return { ok: true, json: async () => ({
      csrf_token: token, mute_available: true, source_available: true,
      receiver_query_available: true,
    }) };
    if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
      operations: { source: { enabled: true, available_sources: ['CD'] },
        receiver_query: { enabled: true } },
    }) };
    if (options.method === 'POST') {
      posts.push({ url, options });
      if (url === '/api/v1/receiver/query') {
        if (queryReply) await queryReply;
        if (queryFailure) return { ok: false, json: async () => queryFailure };
        token = 'token-2';
        receiver = { source: { value: 'CD', freshness: 'FRESH' },
          monitor: { value: 'ON', freshness: 'FRESH' }, integrity: 'HEALTHY' };
        return { ok: true, json: async () => ({ result: 'OBSERVATION_RECORDED' }) };
      }
      throw new Error('Unexpected control dispatch');
    }
    throw new Error(`Unexpected ${url}`);
  },
};

async function settle() { await new Promise(setImmediate); }

async function main() {
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  await settle();
  assert.equal(posts.length, 0, 'page load and GET polling never query XS8');
  assert.match(elements.get('source-status').textContent, /UNKNOWN/);
  assert.equal(sync.disabled, false);
  assert.equal(sources[0].dataset.observed, undefined);

  let release;
  queryReply = new Promise((resolve) => { release = resolve; });
  const pending = sync.click();
  await settle();
  assert.equal(sync.disabled, true);
  await sync.click();
  await mute.click();
  await sources[0].click();
  assert.equal(posts.length, 1, 'concurrent clicks dispatch only one query');
  assert.equal(posts[0].url, '/api/v1/receiver/query');
  assert.equal(posts[0].options.body, '{}');
  assert.equal(posts[0].options.headers['X-CSRF-Token'], 'token-1');
  release();
  await pending;
  assert.match(elements.get('source-status').textContent, /SOURCE CD/);
  assert.match(elements.get('monitor-status').textContent, /MONITOR ON/);
  assert.equal(sources[0].dataset.observed, 'true');
  assert.equal(posts.length, 1);
  await poll();
  assert.equal(posts.length, 1, 'polling never retries the query');

  receiver = { source: { value: 'VIDEO1', freshness: 'FRESH' },
    monitor: { value: 'OFF', freshness: 'LAST_KNOWN' }, integrity: 'HEALTHY' };
  await poll(); // Simulated front-panel observation, with no Web command.
  assert.equal(sources[1].disabled, true);
  assert.equal(sources[1].dataset.observed, 'true');
  assert.match(elements.get('monitor-status').textContent, /MONITOR OFF/);
  assert.equal(posts.length, 1);

  serviceAvailable = false;
  await poll();
  assert.equal(sync.disabled, true);
  assert.match(elements.get('source-status').textContent, /CONNECTION LOST/);
  serviceAvailable = true;
  await poll();
  assert.equal(sync.disabled, false);
  assert.equal(sources[1].dataset.observed, 'true');

  receiver = { source: { value: 'UNKNOWN', freshness: 'UNKNOWN' },
    monitor: { value: 'UNKNOWN', freshness: 'UNKNOWN' }, integrity: 'HEALTHY' };
  await poll();
  assert.match(elements.get('source-status').textContent, /UNKNOWN/);
  assert.equal(sources[0].dataset.observed, undefined);
  assert.equal(posts.length, 1);

  // Manual SYNC failure names the service's specific reason; nothing is resent.
  await new Promise((resolve) => setTimeout(resolve, 10));
  queryReply = null;
  queryFailure = { result: 'OBSERVATION_FAILED', reason: 'NO_REPLY',
    receiver_execution: 'UNVERIFIED' };
  await sync.click();
  assert.equal(elements.get('availability-text').textContent,
    'SYNC NO_REPLY · LAST OBSERVATION PRESERVED');
  assert.equal(posts.length, 2);
  process.stdout.write('Web receiver readback offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
