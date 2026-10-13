const text = (id, value) => { document.getElementById(id).textContent = value; };
const muteButton = document.getElementById('mute-button');
const syncButton = document.getElementById('sync-button');
const sourceButtons = [...document.querySelectorAll('.source-grid button[data-source]')];
const cdButtons = [document.getElementById('cd-previous'), document.getElementById('cd-next')];
let csrfToken = null;
let serviceReady = false;
let muteReady = false;
let sourceReady = false;
let queryReady = false;
let cdReady = false;
let cdFault = false;
let enabledSources = [];
let enabledCdActions = [];
let controlBusy = false;
let lastSourceAttempt = 0;
// Spacing from one completed CD request to the next, as configured by the
// Gateway. Without a valid value, keep the conservative Phase A 1 s.
let cdIntervalMs = 1000;
let lastCdCompleted = -Infinity;
// Diagnostics only: the last CD tap's timeline and taps dropped by the guards.
let cdTiming = null;
const cdDropped = { busy: 0, interval: 0, disabled: 0 };
let controlResult = null;
let refreshController = null;
let refreshVersion = 0;
let lastReceiver = null;

function updateControls() {
  muteButton.disabled = controlBusy || !csrfToken || !serviceReady || !muteReady;
  syncButton.disabled = controlBusy || !csrfToken || !serviceReady || !queryReady;
  for (const button of sourceButtons) {
    button.disabled = controlBusy || !csrfToken || !serviceReady || !sourceReady ||
      !enabledSources.includes(button.dataset.source);
  }
  for (const button of cdButtons) {
    button.disabled = controlBusy || !csrfToken || !serviceReady || !cdReady ||
      !enabledCdActions.includes(button.dataset.cdAction);
  }
  const available = !controlBusy && serviceReady && csrfToken &&
    (muteReady || sourceReady || queryReady || cdReady);
  // A latched CD fault needs physical attention; never hide it behind a result.
  text('availability-text', cdFault && !controlBusy
    ? 'CD FAULT LATCHED · STOP AND INSPECT CHANGER'
    : controlResult
    ? `${controlResult}${available ? '' : ' · CONTROLS UNAVAILABLE'}`
    : (available ? 'CONTROLS READY · RECEIVER ACTION UNVERIFIED'
      : 'CONTROLS UNAVAILABLE · SERVICE OR AUTHORIZATION NOT READY'));
}

function renderReceiver(receiver, connected) {
  if (receiver && receiver.source && receiver.monitor) lastReceiver = receiver;
  const state = lastReceiver;
  const source = state && state.source.value || 'UNKNOWN';
  const monitor = state && state.monitor.value || 'UNKNOWN';
  const quality = !connected ? 'CONNECTION LOST' :
    state && state.integrity !== 'HEALTHY' ? 'OBSERVATION UNCERTAIN' :
    state && state.source.freshness === 'LAST_KNOWN' ? 'LAST KNOWN' :
    state && state.source.freshness || 'UNKNOWN';
  text('source-status', `OBSERVED SOURCE ${source} · ${quality}`);
  text('monitor-status', `OBSERVED TAPE2 MONITOR ${monitor}${!connected ? ' · CONNECTION LOST' :
    state && state.monitor.freshness && state.monitor.freshness !== 'FRESH'
      ? ` · ${state.monitor.freshness}` : ''}`);
  for (const button of sourceButtons) {
    if (connected && state && state.integrity === 'HEALTHY' &&
        button.dataset.source === source) button.dataset.observed = 'true';
    else delete button.dataset.observed;
  }
}

function renderStatus(data) {
  text('gateway', data && data.gateway === 'online' ? 'Online' : 'Unavailable');
  if (!data || data.xs8_service !== 'connected' || !data.status) {
    renderReceiver(null, false);
    text('service', data ? 'Unavailable' : 'Unknown');
    text('summary-state', data ? 'Service unavailable' : 'Connection lost');
    for (const id of ['state', 'monitor', 'tx', 'waveform', 'cd-state']) text(id, '—');
    return;
  }
  const status = data.status;
  renderReceiver(data.receiver, true);
  text('service', 'Connected');
  text('summary-state', status.state);
  text('state', status.state);
  text('monitor', status.monitor);
  text('tx', `${status.last_byte} · ${status.last_gpio_tx}`);
  text('waveform', status.last_waveform);
  text('cd-state', status.cd_state
    ? `${status.cd_state} · ${status.cd_last_result || '—'}${status.cd_release_owed === '1' ? ' · RELEASE OWED' : ''}`
    : '—');
}

function applyRefresh(statusData, sessionData, capabilitiesData) {
  const status = statusData && statusData.xs8_service === 'connected' && statusData.status;
  serviceReady = !!status && status.state === 'IDLE' && status.monitor === 'HEALTHY' &&
    status.stop_owed === '0' && status.context_may_be_open === '0';
  csrfToken = serviceReady && sessionData && typeof sessionData.csrf_token === 'string'
    ? sessionData.csrf_token : null;
  muteReady = !!csrfToken && sessionData.mute_available === true;
  const source = capabilitiesData && capabilitiesData.operations && capabilitiesData.operations.source;
  sourceReady = !!csrfToken && sessionData.source_available === true &&
    !!source && source.enabled === true && Array.isArray(source.available_sources);
  enabledSources = sourceReady ? source.available_sources : [];
  const cd = capabilitiesData && capabilitiesData.operations &&
    capabilitiesData.operations.cd_transport;
  cdFault = !!status && (status.cd_state === 'FAULT_LATCHED' ||
    (status.cd_state === 'IDLE' && status.cd_release_owed === '1'));
  cdReady = !!csrfToken && sessionData.cd_track_available === true &&
    !!status && status.cd_state === 'IDLE' && status.cd_release_owed === '0' &&
    !!cd && cd.enabled === true && Array.isArray(cd.available_actions);
  enabledCdActions = cdReady ? cd.available_actions : [];
  cdIntervalMs = cd && Number.isInteger(cd.min_interval_ms) &&
    cd.min_interval_ms >= 200 && cd.min_interval_ms <= 1000 ? cd.min_interval_ms : 1000;
  const query = capabilitiesData && capabilitiesData.operations &&
    capabilitiesData.operations.receiver_query;
  queryReady = !!csrfToken && sessionData.receiver_query_available === true &&
    !!query && query.enabled === true;
  renderStatus(statusData);
  updateControls();
}

async function fetchJson(url, options = {}) {
  const started = Date.now();
  const response = await fetch(url, options);
  const data = await response.json();
  const timing = response.headers && typeof response.headers.get === 'function'
    ? response.headers.get('Server-Timing') : null;
  return { ok: response.ok, data, ms: Date.now() - started, timing };
}

function serverTiming(header) {
  const result = {};
  for (const item of (header || '').split(',')) {
    const match = /^\s*(\w+);dur=([\d.]+)\s*$/.exec(item);
    if (match) result[match[1]] = Math.round(Number(match[2]));
  }
  return result;
}

function renderCdTiming() {
  const t = cdTiming;
  const dropped = `dropped busy ${cdDropped.busy} · interval ${cdDropped.interval}` +
    ` · disabled ${cdDropped.disabled}`;
  if (!t) { text('cd-timing', dropped); return; }
  const parts = [`POST ${t.postMs ?? '…'} (xs8 ${t.server.xs8cd ?? '—'} · status ${t.server.xs8status ?? '—'})`];
  if (t.refresh) {
    parts.push(`token ${t.refresh.total} (${t.refresh.status ?? '—'}/${t.refresh.session ?? '—'}/${t.refresh.capabilities ?? '—'})`);
    parts.push(t.enabledAt === null ? 'not re-enabled'
      : `wait ${t.intervalWait} · ready +${t.readyAt - t.click} ms`);
    if (t.renderMs !== null) parts.push(`render ${t.renderMs}`);
  }
  text('cd-timing', `${parts.join(' · ')} · ${dropped}`);
}

// One bounded refresh at a time. Apply its results together, so a delayed GET
// cannot restore an old one-use token or overwrite newer availability.
async function refresh() {
  if (controlBusy || refreshController) return;
  const controller = new AbortController();
  const version = ++refreshVersion;
  refreshController = controller;
  const timeout = setTimeout(() => controller.abort(), 4500);
  let statusData = null;
  let sessionData = null;
  let capabilitiesData = null;
  const started = Date.now();
  const steps = {};
  try {
    const status = await fetchJson('/api/v1/status', {
      cache: 'no-store', credentials: 'same-origin', signal: controller.signal,
    });
    statusData = status.data;
    steps.status = status.ms;
    if (status.ok && statusData.xs8_service === 'connected') {
      const s = statusData.status;
      if (s && s.state === 'IDLE' && s.monitor === 'HEALTHY' &&
          s.stop_owed === '0' && s.context_may_be_open === '0') {
        const session = await fetchJson('/api/v1/session', {
          cache: 'no-store', credentials: 'same-origin', signal: controller.signal,
        });
        steps.session = session.ms;
        if (session.ok && typeof session.data.csrf_token === 'string') {
          sessionData = session.data;
          if (session.data.source_available === true ||
              session.data.receiver_query_available === true ||
              session.data.cd_track_available === true) {
            try {
              const capabilities = await fetchJson('/api/v1/capabilities', {
                cache: 'no-store', credentials: 'same-origin', signal: controller.signal,
              });
              steps.capabilities = capabilities.ms;
              if (capabilities.ok) capabilitiesData = capabilities.data;
            } catch (_) {
              // MUTE eligibility remains independent of capabilities.
            }
          }
        }
      }
    }
  } catch (_) {
    // A timeout or transient failure disables controls until the next poll.
  } finally {
    clearTimeout(timeout);
    if (refreshController === controller) refreshController = null;
  }
  if (version === refreshVersion && !controlBusy) {
    applyRefresh(controller.signal.aborted ? null : statusData,
      controller.signal.aborted ? null : sessionData, capabilitiesData);
    if (cdTiming && cdTiming.awaitingRefresh) recordCdRefresh(steps, Date.now() - started);
  }
}

// The first refresh after a CD request supplies the next token and re-enables PREV/NEXT.
function recordCdRefresh(steps, total) {
  const t = cdTiming;
  t.awaitingRefresh = false;
  t.refresh = { total, ...steps };
  const enabled = cdButtons.some((button) => !button.disabled);
  t.enabledAt = enabled ? Date.now() : null;
  t.intervalWait = enabled ? Math.max(0, lastCdCompleted + cdIntervalMs - t.enabledAt) : null;
  t.readyAt = enabled ? t.enabledAt + t.intervalWait : null;
  t.renderMs = null;
  renderCdTiming();
  if (enabled && typeof requestAnimationFrame === 'function') {
    requestAnimationFrame(() => { t.renderMs = Date.now() - t.enabledAt; renderCdTiming(); });
  }
}

async function sendControl(url, body, success, failure) {
  controlBusy = true;
  ++refreshVersion;
  if (refreshController) {
    refreshController.abort();
    refreshController = null;
  }
  const token = csrfToken;
  csrfToken = null; muteReady = false; sourceReady = false; queryReady = false;
  cdReady = false;
  updateControls();
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 6000);
  try {
    const response = await fetchJson(url, {
      method: 'POST', credentials: 'same-origin', signal: controller.signal,
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': token },
      body,
    });
    if (url.endsWith('/cd/track') && cdTiming) {
      cdTiming.postMs = response.ms;
      cdTiming.server = serverTiming(response.timing);
    }
    controlResult = response.ok ? success : failure(response.data);
  } catch (_) {
    controlResult = url.endsWith('/mute') ? 'MUTE RESULT UNKNOWN · CHECK RECEIVER'
      : url.endsWith('/query') ? 'SYNC RESULT UNKNOWN · CHECK RECEIVER'
      : url.endsWith('/cd/track') ? 'CD RESULT UNKNOWN · CHECK CHANGER'
      : 'SOURCE RESULT UNKNOWN · CHECK RECEIVER';
  } finally {
    clearTimeout(timeout);
    if (url.endsWith('/cd/track')) {
      lastCdCompleted = Date.now();
      if (cdTiming) { cdTiming.completed = lastCdCompleted; cdTiming.awaitingRefresh = true; }
    }
    controlBusy = false;
    updateControls();
    await refresh();
  }
}

muteButton.addEventListener('click', async () => {
  if (muteButton.disabled || controlBusy) return;
  await sendControl('/api/v1/mute', '{}',
    'MUTE TX REPORTED · RECEIVER ACTION UNVERIFIED',
    (data) => `MUTE ${data.result || 'FAILED'} · RECEIVER ACTION UNVERIFIED`);
});

syncButton.addEventListener('click', async () => {
  if (syncButton.disabled || controlBusy) return;
  await sendControl('/api/v1/receiver/query', '{}',
    'SYNC OBSERVATIONS RECORDED · CHECK DISPLAYED STATE',
    (data) => `SYNC ${data.result || 'FAILED'} · LAST OBSERVATION PRESERVED`);
});

for (const button of sourceButtons) {
  button.addEventListener('click', async () => {
    if (button.disabled || controlBusy || Date.now() - lastSourceAttempt < 1000) return;
    lastSourceAttempt = Date.now();
    await sendControl('/api/v1/source', JSON.stringify({ source: button.dataset.source }),
      `${button.dataset.source} TX REPORTED · SELECTION UNVERIFIED`,
      (data) => `SOURCE ${data.result || 'FAILED'} · SELECTION UNVERIFIED`);
  });
}

function cdFailure(label, data) {
  const result = data && data.result || 'FAILED';
  if (result === 'CD_FAULT_LATCHED') return 'CD FAULT LATCHED · STOP AND INSPECT CHANGER';
  if (data && data.physical_check_required === true) {
    return `${label} ${result} · CHECK CHANGER`;
  }
  return `${label} ${result} · NOTHING SENT`;
}

for (const button of cdButtons) {
  button.addEventListener('click', async () => {
    // Taps while a request or its token refresh is pending are dropped, never queued.
    const dropped = button.disabled ? 'disabled' : controlBusy ? 'busy'
      : Date.now() - lastCdCompleted < cdIntervalMs ? 'interval' : null;
    if (dropped) {
      cdDropped[dropped] += 1;
      renderCdTiming();
      return;
    }
    cdTiming = { click: Date.now(), postMs: null, server: {}, refresh: null,
      awaitingRefresh: false, enabledAt: null, readyAt: null, renderMs: null };
    renderCdTiming();
    const label = button.dataset.cdAction === 'NEXT' ? 'NEXT' : 'PREV';
    await sendControl('/api/v1/cd/track', JSON.stringify({ action: button.dataset.cdAction }),
      `${label} TX VERIFIED · TRACK CHANGE UNVERIFIED`,
      (data) => cdFailure(label, data));
  });
}

refresh();
setInterval(() => refresh(), 5000);
