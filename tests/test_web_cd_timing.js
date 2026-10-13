const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// CD tap timeline diagnostics: each simulated request advances a fake clock.
function load({ postMs, statusMs, sessionMs, capabilitiesMs, intervalMs }) {
  const elements = new Map();
  const button = (cdAction) => ({ dataset: { cdAction }, disabled: true,
    addEventListener(_event, handler) { this.click = handler; } });
  const cd = { 'cd-previous': button('PREVIOUS'), 'cd-next': button('NEXT') };
  const env = { cd, elements, posts: 0, now: 10000, gate: null };
  const respond = (data, timing) => ({ ok: true, json: async () => data,
    headers: { get: (name) => (name === 'Server-Timing' ? timing : null) } });
  const context = {
    AbortController, setTimeout, clearTimeout,
    Date: { now: () => env.now },
    setInterval() {},
    document: {
      getElementById(id) {
        if (cd[id]) return cd[id];
        if (!elements.has(id)) elements.set(id, { textContent: '', dataset: {}, addEventListener() {} });
        return elements.get(id);
      },
      querySelectorAll: () => [],
    },
    async fetch(url, options) {
      if (url === '/api/v1/status') {
        env.now += statusMs;
        return respond({ gateway: 'online', xs8_service: 'connected', status: { state: 'IDLE',
          monitor: 'HEALTHY', stop_owed: '0', context_may_be_open: '0', cd_state: 'IDLE',
          cd_release_owed: '0', cd_last_result: 'TX_VERIFIED' } }, 'xs8status;dur=5.0, total;dur=8.0');
      }
      if (url === '/api/v1/session') {
        env.now += sessionMs;
        return respond({ csrf_token: `token-${env.posts + 1}`, cd_track_available: true });
      }
      if (url === '/api/v1/capabilities') {
        env.now += capabilitiesMs;
        return respond({ operations: { cd_transport: { enabled: true,
          available_actions: ['NEXT', 'PREVIOUS'], min_interval_ms: intervalMs } } });
      }
      if (url === '/api/v1/cd/track') {
        env.posts += 1;
        if (env.gate) await env.gate;
        env.now += postMs;
        return respond({ result: 'SERVICE_TX_OK', cd_result: 'TX_VERIFIED',
          receiver_execution: 'UNVERIFIED' }, 'sincecd;dur=900.0, xs8status;dur=6.4, xs8cd;dur=331.2, total;dur=345.0');
      }
      throw new Error(`unexpected request ${url}`);
    },
  };
  vm.runInNewContext(fs.readFileSync('web/app.js', 'utf8'), context);
  return env;
}

async function settle() { for (let i = 0; i < 8; i += 1) await new Promise(setImmediate); }
const timing = (env) => env.elements.get('cd-timing').textContent;

async function slowRefreshDominates() {
  // Pi-like: the three sequential token-refresh GETs take longer than the interval.
  const env = load({ postMs: 400, statusMs: 90, sessionMs: 95, capabilitiesMs: 100, intervalMs: 250 });
  await settle();
  const click = env.now;
  await env.cd['cd-previous'].click();
  assert.equal(timing(env), 'POST 400 (xs8 331 · status 6) · token 285 (90/95/100) · ' +
    'wait 0 · ready +685 ms · dropped busy 0 · interval 0 · disabled 0');
  assert.equal(env.now - click, 685, 'ready = POST + token refresh when refresh > interval');
  assert.equal(env.cd['cd-previous'].disabled, false);
}

async function fastRefreshWaitsForInterval() {
  const env = load({ postMs: 400, statusMs: 30, sessionMs: 30, capabilitiesMs: 30, intervalMs: 250 });
  await settle();
  await env.cd['cd-next'].click();
  // Enabled 90 ms after completion; the 250 ms interval still blocks 160 ms more.
  assert.match(timing(env), /token 90 \(30\/30\/30\) · wait 160 · ready \+650 ms/);
  await env.cd['cd-next'].click();
  assert.equal(env.posts, 1);
  assert.match(timing(env), /dropped busy 0 · interval 1 · disabled 0$/,
    'an enabled-looking button silently drops taps inside the interval');
  env.now += 160;
  await env.cd['cd-next'].click();
  assert.equal(env.posts, 2);
}

async function inFlightTapsAreCounted() {
  const env = load({ postMs: 400, statusMs: 90, sessionMs: 95, capabilitiesMs: 100, intervalMs: 250 });
  await settle();
  let release;
  env.gate = new Promise((done) => { release = done; });
  const first = env.cd['cd-next'].click();
  await settle();
  assert.match(timing(env), /^POST … \(xs8 — · status —\)/);
  env.cd['cd-next'].disabled = false; // A click the browser still delivers.
  await env.cd['cd-next'].click();
  env.cd['cd-next'].disabled = true;
  await env.cd['cd-next'].click();
  release();
  await first;
  assert.equal(env.posts, 1);
  assert.match(timing(env), /dropped busy 1 · interval 0 · disabled 1$/);
}

async function main() {
  await slowRefreshDominates();
  await fastRefreshWaitsForInterval();
  await inFlightTapsAreCounted();
  process.stdout.write('Web CD timing diagnostics offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
