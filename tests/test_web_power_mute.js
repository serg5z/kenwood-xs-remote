const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// POWER: one round red button opposite the heading that only opens an ON / OFF
// chooser, gated by the existing power capability. MUTE: neutral in the Volume
// column; never painted muted from a TX result (there is no mute readback).

const html = fs.readFileSync('web/index.html', 'utf8');
const css = fs.readFileSync('web/app.css', 'utf8');
const app = fs.readFileSync('web/app.js', 'utf8');

// ---- Markup ----
const between = (start, end) => {
  const from = html.indexOf(start);
  return html.slice(from, html.indexOf(end, from));
};
const heading = between('<div class="remote-heading">', '<div class="control-layout">');
const volume = between('<section class="volume"', '</section>');
assert.match(heading, /<h1>Receiver control<\/h1>/);
assert.match(heading, /<button id="power-button" class="power-button" disabled type="button" aria-label="Receiver power" aria-haspopup="true" aria-expanded="false" aria-controls="power-menu">\s*<svg viewBox="0 0 24 24" aria-hidden="true"/);
assert.match(heading, /<div id="power-menu" class="power-menu" role="group"[^>]*hidden>/);
assert.match(heading, /id="power-on" disabled type="button" data-power-action="ON">ON</);
assert.match(heading, /id="power-off" disabled type="button" data-power-action="OFF">OFF</);
assert.equal((html.match(/data-power-action=/g) || []).length, 2);
assert.match(volume, /<button class="mute" id="mute-button" disabled type="button">MUTE<\/button>/);
assert.doesNotMatch(volume, /power/i, 'POWER is no longer in the Volume column');

// ---- Styling ----
const rule = (selector) => {
  const at = css.indexOf(`${selector} {`);
  assert.notEqual(at, -1, selector);
  return css.slice(at, css.indexOf('}', at));
};
assert.match(rule('.power-button'), /border-radius:50%/);
assert.match(rule('.power-button'), /background:#a64532/);
assert.match(rule('.power-button'), /width:44px; height:44px/);
const disabled = rule('.remote .power-button:disabled');
assert.doesNotMatch(disabled, /#a64532/, 'disabled POWER is visibly different');
assert.match(disabled, /cursor:not-allowed/);
assert.match(rule('.power-control'), /position:absolute; top:0; right:0/);
assert.match(rule('.remote-heading'), /padding-right:60px/, 'the heading never runs under POWER');
assert.match(rule('.power-menu[hidden]'), /display:none/);
// MUTE: neutral when enabled; nothing styles a muted state.
assert.doesNotMatch(rule('.mute:not(:disabled)'), /#a64532/);
assert.doesNotMatch(css, /\.mute\[|\[data-mute|\.muted\b/);
// The transport label sizing is unchanged.
assert.match(css, /\.transport-grid small \{ font-size: 14px; font-weight: 600; letter-spacing: \.04em; line-height: 1; \}/);
assert.match(css, /@media \(max-width: 359px\) \{ \.transport-grid small \{ font-size: 10px; letter-spacing: \.02em; \} \}/);

// ---- Behaviour ----
const elements = new Map();
function element(id) {
  if (!elements.has(id)) {
    elements.set(id, {
      id, textContent: '', disabled: true, dataset: {}, listeners: {}, attributes: {},
      addEventListener(type, handler) { (this.listeners[type] ||= []).push(handler); },
      setAttribute(name, value) { this.attributes[name] = value; },
      contains(other) { return other === this || (this.children || []).includes(other); },
      fire(type) { return Promise.all((this.listeners[type] || []).map((h) => h({ target: this }))); },
    });
  }
  return elements.get(id);
}
const trigger = element('power-button');
const menu = element('power-menu');
const on = element('power-on');
const off = element('power-off');
const mute = element('mute-button');
const outside = element('volume-up');
on.dataset.powerAction = 'ON';
off.dataset.powerAction = 'OFF';
menu.hidden = true;
menu.children = [on, off];
const documentListeners = {};
const fireDocument = (type, event) =>
  (documentListeners[type] || []).forEach((handler) => handler(event));

let power = { available: false, actions: ['ON', 'OFF'] };
const posts = [];
const context = {
  AbortController, Date, setTimeout, clearTimeout, setInterval() {},
  document: {
    getElementById: element, querySelectorAll: () => [],
    addEventListener(type, handler) { (documentListeners[type] ||= []).push(handler); },
  },
  async fetch(url, options = {}) {
    if (url === '/api/v1/status') return { ok: true, json: async () => ({
      gateway: 'online', xs8_service: 'connected', status: { state: 'IDLE', monitor: 'HEALTHY',
        stop_owed: '0', context_may_be_open: '0', cd_state: 'IDLE', cd_release_owed: '0',
        last_byte: '0x07', last_gpio_tx: 'TX_OK', last_waveform: 'MATCH' } }) };
    if (url === '/api/v1/session') return { ok: true, json: async () => ({
      csrf_token: `token-${posts.length}`, mute_available: true,
      power_available: power.available }) };
    if (url === '/api/v1/capabilities') return { ok: true, json: async () => ({
      operations: { power: { enabled: power.available, available_actions: power.actions } } }) };
    if (options.method === 'POST') {
      posts.push([url, JSON.parse(options.body)]);
      return { ok: true, json: async () => ({ result: 'SERVICE_TX_OK', gpio_tx: 'TX_OK',
        waveform: 'MATCH', receiver_execution: 'UNVERIFIED' }) };
    }
    throw new Error(`unexpected ${url}`);
  },
};
async function settle() { for (let i = 0; i < 8; i += 1) await new Promise(setImmediate); }
async function refresh() { await context.refreshNow(); await settle(); }

async function main() {
  vm.runInNewContext(`${app}\nglobalThis.refreshNow = refresh;`, context);
  await settle();

  // Capability off (no --enable-power): visibly disabled, sends nothing.
  assert.equal(trigger.disabled, true);
  await trigger.fire('click');
  assert.equal(menu.hidden, true, 'a disabled POWER never opens the chooser');
  await on.fire('click');
  await off.fire('click');
  assert.deepEqual(posts, []);

  // Capability on: the first click only opens the chooser.
  power = { available: true, actions: ['ON', 'OFF'] };
  await refresh();
  assert.equal(trigger.disabled, false);
  assert.equal(on.disabled, false);
  await on.fire('click');
  assert.deepEqual(posts, [], 'a choice in a closed chooser sends nothing');
  await trigger.fire('click');
  assert.equal(menu.hidden, false);
  assert.equal(trigger.attributes['aria-expanded'], 'true');
  assert.deepEqual(posts, [], 'opening the chooser sends nothing');
  // Escape, a second click and an outside press close it; a press inside does not.
  fireDocument('keydown', { key: 'Escape' });
  assert.equal(menu.hidden, true);
  assert.equal(trigger.attributes['aria-expanded'], 'false');
  await trigger.fire('click');
  await trigger.fire('click');
  assert.equal(menu.hidden, true, 'the button toggles the chooser');
  await trigger.fire('click');
  fireDocument('pointerdown', { target: on });
  assert.equal(menu.hidden, false);
  fireDocument('pointerdown', { target: outside });
  assert.equal(menu.hidden, true);
  assert.deepEqual(posts, []);

  // An explicit choice sends exactly one command and closes the chooser.
  await trigger.fire('click');
  await on.fire('click');
  await settle();
  assert.equal(menu.hidden, true);
  assert.deepEqual(posts, [['/api/v1/power', { action: 'ON' }]]);
  assert.match(element('availability-text').textContent,
    /^POWER ON TX VERIFIED · RECEIVER POWER UNVERIFIED/);
  await trigger.fire('click');
  await off.fire('click');
  await settle();
  assert.deepEqual(posts.at(-1), ['/api/v1/power', { action: 'OFF' }]);
  assert.equal(posts.length, 2);

  // Only allowlisted actions are offered.
  power = { available: true, actions: ['ON'] };
  await refresh();
  assert.equal(on.disabled, false);
  assert.equal(off.disabled, true);
  // Losing the capability while the chooser is open closes and disables it.
  await trigger.fire('click');
  assert.equal(menu.hidden, false);
  power = { available: false, actions: ['ON', 'OFF'] };
  await refresh();
  assert.equal(trigger.disabled, true);
  assert.equal(menu.hidden, true);
  await on.fire('click');
  assert.equal(posts.length, 2);

  // MUTE: a verified TX never marks the button muted.
  assert.equal(mute.disabled, false);
  const before = JSON.stringify([mute.dataset, mute.attributes]);
  await mute.fire('click');
  await settle();
  assert.deepEqual(posts.at(-1), ['/api/v1/mute', {}]);
  assert.equal(JSON.stringify([mute.dataset, mute.attributes]), before);
  process.stdout.write('Web POWER and MUTE offline tests passed\n');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
