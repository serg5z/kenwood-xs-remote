const text = (id, value) => { document.getElementById(id).textContent = value; };
const muteButton = document.getElementById('mute-button');
const syncButton = document.getElementById('sync-button');
const sourceButtons = [...document.querySelectorAll('.source-grid button[data-source]')];
const cdButtons = [document.getElementById('cd-previous'), document.getElementById('cd-next')];
const volumeButtons = [document.getElementById('volume-up'), document.getElementById('volume-down')];
const playbackButtons = [document.getElementById('cd-play-pause'), document.getElementById('cd-stop')];
const seekButtons = [document.getElementById('cd-rew'), document.getElementById('cd-ff')];
const monitorButton = document.getElementById('monitor-button');
const powerButtons = [document.getElementById('power-on'), document.getElementById('power-off')];
let csrfToken = null;
let serviceReady = false;
let muteReady = false;
let sourceReady = false;
let queryReady = false;
let cdReady = false;
let cdFault = false;
let volumeReady = false;
let playbackReady = false;
let enabledPlaybackActions = [];
let seekReady = false;
let enabledSeekDirections = [];
// Pacing for held FF/REW from capabilities. The service lease (2 s) and maximum
// (10 s) remain the authority; these only keep the browser inside them.
let seekHeartbeatMs = 500;
let seekMaxHoldMs = 10000;
const SEEK_REQUEST_TIMEOUT_MS = 4000;
// The active hold (CD seek or volume):
// { kind, button, direction, token, holdId, phase, cancelled, reason, started, pointerId, timer, wake }
let seek = null;
let volumeHoldReady = false;
let enabledVolumeHoldDirections = [];
let volumeHoldDelayMs = 400;
let volumeHoldHeartbeatMs = 500;
let volumeHoldMaxMs = 20000;
// { button, timer } between a volume pointerdown and hold_delay_ms.
let volumeGesture = null;
// After a gesture became a hold, its click is suppressed: indefinitely while the
// pointer is still down, then for a short window after the pointer is released.
let suppressVolumeClickUntil = -Infinity;
let volumePointerDown = false;
let powerReady = false;
let enabledPowerActions = [];
let tape2MonitorReady = false;
let enabledVolumeDirections = [];
// Phase 1: single taps only. Spacing guards duplicate taps; the Gateway enforces it too.
let volumeIntervalMs = 250;
let lastVolumeCompleted = -Infinity;
let enabledSources = [];
let enabledCdActions = [];
let controlBusy = false;
let lastSourceAttempt = 0;
// Spacing from one completed CD request to the next, as configured by the
// Gateway; 0 is the experimental no-cooldown setting, not a missing value.
// Without a valid value, keep the conservative Phase A 1 s.
let cdIntervalMs = 1000;
let lastCdCompleted = -Infinity;
// Diagnostics only: the last CD tap's timeline and taps dropped by the guards.
let cdTiming = null;
const cdDropped = { busy: 0, interval: 0, disabled: 0 };
const prevCounts = { queued: 0, extra: 0, cancelled: 0 };
// Double PREV: the first PREV restarts the track; a second one moves back. One
// explicit second tap within this window of the first may wait for the first
// to complete, cool down and refresh its token. A later tap is a separate action.
const PREV_DOUBLE_TAP_WINDOW_MS = 800;
// { firstTap, lastTap, source, phase: 'first' | 'done' | 'second' | 'closing',
//   pending, extra, timer, renderTimer, token, awaitingRefresh }
let prevGesture = null;
let prevNote = null;
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
  const gesture = prevGestureActive();
  for (const button of cdButtons) {
    const receptive = gesture && button.dataset.cdAction === 'PREVIOUS';
    // During a PREV gesture the PREV button stays clickable so a second tap reaches
    // the handler; the handler, not the button state, decides whether to send.
    button.disabled = !receptive && (controlBusy || !csrfToken || !serviceReady || !cdReady ||
      !enabledCdActions.includes(button.dataset.cdAction) || !!gesture);
    if (receptive && gesture.pending) button.dataset.pending = 'true';
    else delete button.dataset.pending;
  }
  for (const button of volumeButtons) {
    const direction = button.dataset.volumeDirection;
    // A held volume button stays enabled so its release events arrive.
    button.disabled = seek ? button !== seek.button
      : controlBusy || !csrfToken || !serviceReady ||
        !((volumeReady && enabledVolumeDirections.includes(direction)) ||
          (volumeHoldReady && enabledVolumeHoldDirections.includes(direction)));
    if (seek && button === seek.button) button.dataset.active = 'true';
    else delete button.dataset.active;
  }
  monitorButton.disabled = controlBusy || !csrfToken || !serviceReady || !tape2MonitorReady;
  for (const button of powerButtons) {
    button.disabled = controlBusy || !csrfToken || !serviceReady || !powerReady ||
      !enabledPowerActions.includes(button.dataset.powerAction);
  }
  for (const button of playbackButtons) {
    button.disabled = controlBusy || !csrfToken || !serviceReady || !playbackReady ||
      !enabledPlaybackActions.includes(button.dataset.cdPlayback) || !!gesture;
  }
  for (const button of seekButtons) {
    // The held button stays enabled so its pointerup and capture events arrive.
    button.disabled = seek ? button !== seek.button
      : controlBusy || !csrfToken || !serviceReady || !seekReady ||
        !enabledSeekDirections.includes(button.dataset.cdSeek) || !!gesture;
    if (seek && button === seek.button) button.dataset.active = 'true';
    else delete button.dataset.active;
  }
  const available = !controlBusy && serviceReady && csrfToken &&
    (muteReady || sourceReady || queryReady || cdReady || volumeReady || playbackReady ||
      seekReady || volumeHoldReady || powerReady || tape2MonitorReady);
  // A latched CD fault needs physical attention; never hide it behind a result.
  text('availability-text', cdFault && !controlBusy
    ? 'CD FAULT LATCHED · STOP AND INSPECT CHANGER'
    : gesture && prevNote && gesture.phase !== 'closing'
    ? `PREV ×2 · ${prevNote}`
    : controlResult
    ? `${controlResult}${prevNote ? ` · ${prevNote}` : ''}${available ? '' : ' · CONTROLS UNAVAILABLE'}`
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
  // The MONITOR indicator shows only a fresh, confirmed receiver report: never a
  // transmission result. Stale, uncertain or unknown is neutral.
  const monitorFresh = connected && !!state && state.integrity === 'HEALTHY' &&
    state.monitor.freshness === 'FRESH' && (monitor === 'ON' || monitor === 'OFF');
  monitorButton.dataset.monitorState = monitorFresh ? monitor : 'UNKNOWN';
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
  cdIntervalMs = cd && Number.isInteger(cd.min_interval_ms) && (cd.min_interval_ms === 0 ||
    (cd.min_interval_ms >= 200 && cd.min_interval_ms <= 1000)) ? cd.min_interval_ms : 1000;
  const cdIdle = !!status && status.cd_state === 'IDLE' && status.cd_release_owed === '0';
  const operations = capabilitiesData && capabilitiesData.operations;
  const playback = operations && operations.cd_playback;
  playbackReady = !!csrfToken && sessionData.cd_playback_available === true && cdIdle &&
    !!playback && playback.enabled === true && Array.isArray(playback.available_actions);
  enabledPlaybackActions = playbackReady ? playback.available_actions : [];
  const seekOperation = operations && operations.cd_seek;
  seekReady = !!csrfToken && sessionData.cd_seek_available === true && cdIdle &&
    !!seekOperation && seekOperation.enabled === true &&
    Array.isArray(seekOperation.available_directions);
  enabledSeekDirections = seekReady ? seekOperation.available_directions : [];
  seekHeartbeatMs = seekOperation && Number.isInteger(seekOperation.heartbeat_ms) &&
    seekOperation.heartbeat_ms >= 200 && seekOperation.heartbeat_ms <= 1000
    ? seekOperation.heartbeat_ms : 500;
  seekMaxHoldMs = seekOperation && Number.isInteger(seekOperation.max_hold_ms) &&
    seekOperation.max_hold_ms >= 1000 && seekOperation.max_hold_ms <= 10000
    ? seekOperation.max_hold_ms : 10000;
  const holdOperation = operations && operations.volume_hold;
  volumeHoldReady = !!csrfToken && sessionData.volume_hold_available === true && cdIdle &&
    !!holdOperation && holdOperation.enabled === true &&
    Array.isArray(holdOperation.available_directions);
  enabledVolumeHoldDirections = volumeHoldReady ? holdOperation.available_directions : [];
  const bounded = (value, low, high, fallback) =>
    (Number.isInteger(value) && value >= low && value <= high ? value : fallback);
  volumeHoldDelayMs = bounded(holdOperation && holdOperation.hold_delay_ms, 200, 1000, 400);
  volumeHoldHeartbeatMs = bounded(holdOperation && holdOperation.heartbeat_ms, 200, 1000, 500);
  volumeHoldMaxMs = bounded(holdOperation && holdOperation.max_hold_ms, 1000, 20000, 20000);
  const powerOperation = operations && operations.power;
  powerReady = !!csrfToken && sessionData.power_available === true && cdIdle &&
    !!powerOperation && powerOperation.enabled === true &&
    Array.isArray(powerOperation.available_actions);
  enabledPowerActions = powerReady ? powerOperation.available_actions : [];
  const monitorOperation = operations && operations.tape2_monitor;
  tape2MonitorReady = !!csrfToken && sessionData.tape2_monitor_available === true && cdIdle &&
    !!monitorOperation && monitorOperation.enabled === true;
  const volume = capabilitiesData && capabilitiesData.operations &&
    capabilitiesData.operations.volume_step;
  volumeReady = !!csrfToken && sessionData.volume_step_available === true &&
    !!status && status.cd_state === 'IDLE' && status.cd_release_owed === '0' &&
    !!volume && volume.enabled === true && Array.isArray(volume.available_directions);
  enabledVolumeDirections = volumeReady ? volume.available_directions : [];
  volumeIntervalMs = volume && Number.isInteger(volume.min_interval_ms) &&
    volume.min_interval_ms >= 250 && volume.min_interval_ms <= 1000 ? volume.min_interval_ms : 250;
  const query = capabilitiesData && capabilitiesData.operations &&
    capabilitiesData.operations.receiver_query;
  queryReady = !!csrfToken && sessionData.receiver_query_available === true &&
    !!query && query.enabled === true;
  renderStatus(statusData);
  if (prevGesture && prevGesture.pending && prevGesture.phase === 'done') {
    const reason = prevBlocked(statusData, true);
    if (reason) cancelPrev(reason);
  }
  updateControls();
}

async function fetchJson(url, options = {}) {
  const started = Date.now();
  const response = await fetch(url, options);
  const data = await response.json();
  const timing = response.headers && typeof response.headers.get === 'function'
    ? response.headers.get('Server-Timing') : null;
  return { ok: response.ok, status: response.status, data, ms: Date.now() - started, timing };
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
  const prev = prevCounts.queued || prevCounts.extra || prevCounts.cancelled
    ? ` · prev queued ${prevCounts.queued} · extra ${prevCounts.extra}` +
      ` · cancelled ${prevCounts.cancelled}` : '';
  const dropped = `dropped busy ${cdDropped.busy} · interval ${cdDropped.interval}` +
    ` · disabled ${cdDropped.disabled}${prev}`;
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
              session.data.cd_track_available === true ||
              session.data.volume_step_available === true ||
              session.data.cd_playback_available === true ||
              session.data.cd_seek_available === true ||
              session.data.volume_hold_available === true ||
              session.data.power_available === true ||
              session.data.tape2_monitor_available === true) {
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

// options.token: a single-use token returned by the previous CD POST.
// options.onSettled(ok, data): runs after the response, before the display refresh.
async function sendControl(url, body, success, failure, options = {}) {
  const { pendingPrev = false, token: tokenOverride = null, onSettled = null } = options;
  if (!pendingPrev && prevGesture && prevGesture.pending) cancelPrev('OTHER CONTROL');
  let succeeded = false;
  let responseData = null;
  controlBusy = true;
  ++refreshVersion;
  if (refreshController) {
    refreshController.abort();
    refreshController = null;
  }
  const token = tokenOverride || csrfToken;
  csrfToken = null; muteReady = false; sourceReady = false; queryReady = false;
  cdReady = false; volumeReady = false; playbackReady = false; seekReady = false;
  volumeHoldReady = false; powerReady = false; tape2MonitorReady = false;
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
    succeeded = response.ok;
    responseData = response.data;
    controlResult = response.ok ? success : failure(response.data);
  } catch (_) {
    controlResult = url.endsWith('/mute') ? 'MUTE RESULT UNKNOWN · CHECK RECEIVER'
      : url.endsWith('/query') ? 'SYNC RESULT UNKNOWN · CHECK RECEIVER'
      : url.endsWith('/cd/track') || url.endsWith('/cd/playback')
        ? 'CD RESULT UNKNOWN · CHECK CHANGER'
      : url.endsWith('/volume/step') ? 'VOLUME RESULT UNKNOWN · CHECK RECEIVER'
      : url.endsWith('/power') ? 'POWER RESULT UNKNOWN · CHECK RECEIVER'
      : url.endsWith('/receiver/monitor') ? 'MONITOR RESULT UNKNOWN · CHECK RECEIVER'
      : 'SOURCE RESULT UNKNOWN · CHECK RECEIVER';
  } finally {
    clearTimeout(timeout);
    if (url.endsWith('/volume/step')) lastVolumeCompleted = Date.now();
    if (url.endsWith('/cd/playback')) lastCdCompleted = Date.now();
    if (url.endsWith('/cd/track')) {
      lastCdCompleted = Date.now();
      if (cdTiming) { cdTiming.completed = lastCdCompleted; cdTiming.awaitingRefresh = true; }
    }
    controlBusy = false;
    updateControls();
    // A pending second PREV may dispatch here; its request then supersedes this
    // refresh, which only updates the display and the next token otherwise.
    if (onSettled) onSettled(succeeded, responseData);
    await refresh();
  }
  return succeeded;
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

function volumeFailure(label, data) {
  const result = data && data.result || 'FAILED';
  if (result === 'FAULT_LATCHED' || result === 'CD_FAULT_LATCHED') {
    return `${label} ${result} · CHECK RECEIVER · RESTART REQUIRED`;
  }
  if (data && data.physical_check_required === true) return `${label} ${result} · CHECK RECEIVER`;
  return `${label} ${result} · NOTHING SENT`;
}

// One click, one POST. No hold, key repeat or retry: a failure is shown and left.
for (const button of volumeButtons) {
  button.addEventListener('click', async () => {
    // The click that ends a held gesture never also steps.
    if (Date.now() < suppressVolumeClickUntil) {
      suppressVolumeClickUntil = -Infinity;
      return;
    }
    const direction = button.dataset.volumeDirection;
    if (button.disabled || controlBusy || !volumeReady ||
        !enabledVolumeDirections.includes(direction) ||
        Date.now() - lastVolumeCompleted < volumeIntervalMs) return;
    const label = direction === 'UP' ? 'VOL+' : 'VOL−';
    await sendControl('/api/v1/volume/step', JSON.stringify({ direction }),
      `${label} TX VERIFIED · VOLUME CHANGE UNVERIFIED`,
      (data) => volumeFailure(label, data));
  });
}

// One click, one single-frame CD key. PLAY/PAUSE (0xBC) is provisionally mapped.
for (const button of playbackButtons) {
  button.addEventListener('click', async () => {
    if (button.disabled || controlBusy || seek ||
        Date.now() - lastCdCompleted < cdIntervalMs) return;
    const action = button.dataset.cdPlayback;
    const label = action === 'STOP' ? 'STOP' : 'PLAY/PAUSE';
    await sendControl('/api/v1/cd/playback', JSON.stringify({ action }),
      `${label} TX VERIFIED · CHANGER ACTION UNVERIFIED`,
      (data) => cdFailure(label, data));
  });
}

// Held controls: CD FF/REW and Web volume. Both use one connection-owned
// service hold through the Gateway; only one hold of either kind exists at once.
const holdKinds = {
  cd: {
    url: '/api/v1/cd/seek', started: 'SEEK_STARTED',
    label: (direction) => (direction === 'FORWARD' ? 'FF' : 'REW'),
    holding: (label) => `${label} SEEKING · CHANGER ACTION UNVERIFIED`,
    released: (label) => `${label} RELEASE TX VERIFIED · SEEK RESULT UNVERIFIED`,
    unknown: (label) => `${label} RELEASE UNKNOWN · LEASE ENDS SEEK WITHIN 2 s · CHECK CHANGER`,
    fault: 'CD FAULT LATCHED · STOP AND INSPECT CHANGER', check: 'CHANGER',
    failure: (label, data) => cdFailure(label, data),
    heartbeatMs: () => seekHeartbeatMs, maxHoldMs: () => seekMaxHoldMs,
    finished: () => { lastCdCompleted = Date.now(); },
  },
  volume: {
    url: '/api/v1/volume/hold', started: 'HOLD_STARTED',
    label: (direction) => (direction === 'UP' ? 'VOL+' : 'VOL−'),
    holding: (label) => `${label} HOLDING · VOLUME CHANGE UNVERIFIED`,
    released: (label) => `${label} RELEASE TX VERIFIED · VOLUME RESULT UNVERIFIED`,
    unknown: (label) => `${label} RELEASE UNKNOWN · SERVICE STOPS VOLUME WITHIN 2 s · CHECK RECEIVER`,
    fault: 'VOLUME FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED', check: 'RECEIVER',
    failure: (label, data) => volumeFailure(label, data),
    heartbeatMs: () => volumeHoldHeartbeatMs, maxHoldMs: () => volumeHoldMaxMs,
    finished: () => {
      lastVolumeCompleted = Date.now();
      // Pointer already released (its click may still be queued): short window.
      // Still down: stay suppressed until it is released, however long that is.
      if (suppressVolumeClickUntil === Infinity && !volumePointerDown) {
        suppressVolumeClickUntil = Date.now() + 1000;
      }
    },
  },
};

// The single-use token for the next hold request: the successor returned by the
// previous one, or, after a lost or refused response, the session's current token.
async function seekToken(s) {
  if (s.token) {
    const token = s.token;
    s.token = null;
    return token;
  }
  try {
    const session = await fetchJson('/api/v1/session', {
      cache: 'no-store', credentials: 'same-origin',
    });
    return session.ok && typeof session.data.csrf_token === 'string'
      ? session.data.csrf_token : null;
  } catch (_) {
    return null;
  }
}

// One POST, never repeated. status 0 and data null mean no reply was received.
async function seekPost(s, body) {
  const token = await seekToken(s);
  if (!token) return { ok: false, status: 0, data: null };
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), SEEK_REQUEST_TIMEOUT_MS);
  try {
    const response = await fetchJson(s.kind.url, {
      method: 'POST', credentials: 'same-origin', signal: controller.signal,
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': token },
      body: JSON.stringify(body),
    });
    if (response.data && typeof response.data.csrf_token === 'string') {
      s.token = response.data.csrf_token;
    }
    return { ok: response.ok, status: response.status || 0, data: response.data || null };
  } catch (_) {
    return { ok: false, status: 0, data: null };
  } finally {
    clearTimeout(timeout);
  }
}

function seekWait(s, ms) {
  return new Promise((resolve) => {
    s.wake = resolve;
    s.timer = setTimeout(resolve, ms);
  });
}

function startSeek(button, event) {
  if (seek || button.disabled || controlBusy ||
      Date.now() - lastCdCompleted < cdIntervalMs) return;
  startHold(holdKinds.cd, button, button.dataset.cdSeek, event);
}

function startHold(kind, button, direction, event) {
  if (prevGesture && prevGesture.pending) cancelPrev('OTHER CONTROL');
  const s = { kind, button, direction, token: csrfToken, holdId: null,
    phase: 'starting', cancelled: false, reason: null, started: Date.now(),
    pointerId: event && event.pointerId, timer: null, wake: null };
  seek = s;
  controlBusy = true;
  ++refreshVersion;
  if (refreshController) {
    refreshController.abort();
    refreshController = null;
  }
  csrfToken = null; muteReady = false; sourceReady = false; queryReady = false;
  cdReady = false; volumeReady = false; playbackReady = false; seekReady = false;
  volumeHoldReady = false; powerReady = false; tape2MonitorReady = false;
  controlResult = `${kind.label(direction)} STARTING`;
  try {
    if (event && typeof button.setPointerCapture === 'function') {
      button.setPointerCapture(event.pointerId);
    }
  } catch (_) {
    // Window-level release handlers still end the hold.
  }
  updateControls();
  runSeek(s);
}

// Pointer release, cancellation, blur or hiding. Idempotent; a release that
// arrives while START is in flight is honoured as soon as START answers.
function cancelSeek(reason) {
  const s = seek;
  if (!s || s.cancelled) return;
  s.cancelled = true;
  s.reason = reason;
  clearTimeout(s.timer);
  if (s.wake) s.wake();
}

async function runSeek(s) {
  const kind = s.kind;
  const label = kind.label(s.direction);
  const start = await seekPost(s, { action: 'START', direction: s.direction });
  if (!start.ok || !start.data || start.data.result !== kind.started ||
      typeof start.data.hold_id !== 'string') {
    if (start.data && start.status < 500) {
      // A refusal before or without transmission: nothing to release.
      finishSeek(s, kind.failure(label, start.data));
      return;
    }
    // No reply or an uncertain one: a hold may exist that this browser cannot
    // name. Release this session's hold once (stop-only, never a new START).
    s.phase = 'releasing';
    const release = await seekPost(s, { action: 'RELEASE' });
    finishSeek(s, release.data && release.data.result === 'RELEASED'
      ? `${label} START UNCERTAIN · RELEASE TX VERIFIED · CHECK ${kind.check}`
      : `${label} ${start.data && start.data.result || 'RESULT UNKNOWN'} · CHECK ${kind.check}`);
    return;
  }
  s.holdId = start.data.hold_id;
  s.phase = 'holding';
  if (!s.cancelled) {
    controlResult = kind.holding(label);
    updateControls();
  }
  let ended = null;
  while (!s.cancelled) {
    await seekWait(s, kind.heartbeatMs());
    if (s.cancelled) break;
    if (Date.now() - s.started >= kind.maxHoldMs()) {
      s.reason = 'MAX_HOLD';
      break;
    }
    const beat = await seekPost(s, { action: 'HEARTBEAT', hold_id: s.holdId });
    if (beat.ok) continue;
    if (beat.data && beat.data.result === 'NOT_ACTIVE') {
      ended = beat.data.cd_last_result || beat.data.reason || 'NOT ACTIVE';
      break;
    }
    if (beat.data && beat.status !== 403 && beat.status < 500) break;
    // Lost response, consumed token or a Gateway/service timeout: keep holding.
    // The next heartbeat recovers a token; the service inactivity limit ends
    // the hold if renewals keep failing.
  }
  // Always one RELEASE, also after an expiry the Gateway already reported:
  // it is stop-only and returns NOT_ACTIVE without transmitting when nothing is held.
  s.phase = 'releasing';
  const release = await seekPost(s, { action: 'RELEASE', hold_id: s.holdId });
  const result = release.data && release.data.result;
  finishSeek(s, result === 'RELEASED' ? kind.released(label)
    : result === 'NOT_ACTIVE'
      ? `${label} ENDED · ${ended || release.data.cd_last_result || 'NOT ACTIVE'}`
    : result === 'SERVICE_UNCERTAIN' ? kind.fault
    : release.data ? kind.failure(`${label} RELEASE`, release.data)
    : kind.unknown(label));
}

async function finishSeek(s, message) {
  if (seek !== s) return;
  seek = null;
  clearTimeout(s.timer);
  try {
    if (s.pointerId !== undefined && typeof s.button.releasePointerCapture === 'function') {
      s.button.releasePointerCapture(s.pointerId);
    }
  } catch (_) {
    // Capture may already be gone.
  }
  s.kind.finished();
  controlResult = message;
  controlBusy = false;
  updateControls();
  await refresh();
}

for (const button of seekButtons) {
  button.addEventListener('pointerdown', (event) => {
    if (event && event.button !== undefined && event.button !== 0) return;
    if (event && typeof event.preventDefault === 'function') event.preventDefault();
    startSeek(button, event);
  });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    button.addEventListener(type, () => cancelSeek(type.toUpperCase()));
  }
  button.addEventListener('contextmenu', (event) => event.preventDefault());
}

// Volume gesture: a press shorter than hold_delay_ms stays the existing click
// (one VOLUME_STEP); a longer one becomes a hold and its click is suppressed.
// One gesture never produces both.
function cancelVolumeGesture() {
  if (volumeGesture) {
    clearTimeout(volumeGesture.timer);
    volumeGesture = null;
  }
}

for (const button of volumeButtons) {
  button.addEventListener('pointerdown', (event) => {
    if (event && event.button !== undefined && event.button !== 0) return;
    suppressVolumeClickUntil = -Infinity; // A new gesture: any earlier click is gone.
    volumePointerDown = true;
    const direction = button.dataset.volumeDirection;
    if (button.disabled || controlBusy || seek || volumeGesture || !volumeHoldReady ||
        !enabledVolumeHoldDirections.includes(direction)) return;
    const gesture = { button, timer: null };
    volumeGesture = gesture;
    try {
      if (event && typeof button.setPointerCapture === 'function') {
        button.setPointerCapture(event.pointerId);
      }
    } catch (_) {
      // The click and window handlers still apply.
    }
    gesture.timer = setTimeout(() => {
      if (volumeGesture !== gesture) return;
      volumeGesture = null;
      if (controlBusy || seek || !volumeHoldReady ||
          Date.now() - lastVolumeCompleted < volumeIntervalMs) return;
      suppressVolumeClickUntil = Infinity; // This gesture is a hold, never also a step.
      startHold(holdKinds.volume, button, direction, event);
    }, volumeHoldDelayMs);
  });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    button.addEventListener(type, () => {
      volumePointerDown = false;
      // A held gesture's click arrives right after its pointerup.
      if (suppressVolumeClickUntil === Infinity) suppressVolumeClickUntil = Date.now() + 1000;
      cancelVolumeGesture();
      if (seek && seek.button === button) cancelSeek(type.toUpperCase());
    });
  }
  button.addEventListener('contextmenu', (event) => event.preventDefault());
}

// TAPE2 MONITOR toggle (provisional). The indicator never changes from a TX
// result, only from a later observed receiver report.
monitorButton.addEventListener('click', async () => {
  if (monitorButton.disabled || controlBusy || seek) return;
  await sendControl('/api/v1/receiver/monitor', '{}',
    'MONITOR TOGGLE TX VERIFIED · STATE SHOWN ONLY WHEN OBSERVED',
    (data) => volumeFailure('MONITOR', data));
});

// Explicit POWER ON and POWER OFF; no power state is read back or implied.
for (const button of powerButtons) {
  button.addEventListener('click', async () => {
    if (button.disabled || controlBusy || seek) return;
    const action = button.dataset.powerAction;
    await sendControl('/api/v1/power', JSON.stringify({ action }),
      `POWER ${action} TX VERIFIED · RECEIVER POWER UNVERIFIED`,
      (data) => volumeFailure(`POWER ${action}`, data));
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

function observedSource() {
  return lastReceiver && lastReceiver.source && lastReceiver.source.value || 'UNKNOWN';
}

// Why the pending PREV must not be sent now, or null if it may be. A refresh
// always checks eligibility. Before dispatch with the token returned by the
// verified first PREV, the refresh need not have arrived: the Gateway reads
// STATUS again before transmitting, and any refresh that does arrive first and
// shows a problem has already cancelled the tap.
function prevBlocked(statusData, refreshed = false) {
  if (statusData !== undefined &&
      !(statusData && statusData.xs8_service === 'connected')) return 'CONNECTION LOST';
  if (cdFault) return 'CD FAULT';
  if ((refreshed || !prevGesture.token) && (!csrfToken || !serviceReady || !cdReady ||
      !enabledCdActions.includes('PREVIOUS'))) {
    return 'NOT ELIGIBLE';
  }
  if (observedSource() !== prevGesture.source) return 'SOURCE CHANGED';
  return null;
}

// The gesture lasts while its first PREV runs and while a second tap is pending
// or sent. Without a second tap it ends when the recognition window closes. After
// the second PREV it ends only after a full window with no further tap, so a
// continuing burst can never start a third command.
function prevGestureActive() {
  const gesture = prevGesture;
  const now = Date.now();
  if (gesture && ((gesture.phase === 'done' && !gesture.pending &&
      now - gesture.firstTap > PREV_DOUBLE_TAP_WINDOW_MS) ||
      (gesture.phase === 'closing' && now - gesture.lastTap > PREV_DOUBLE_TAP_WINDOW_MS))) {
    endPrevGesture();
    return null;
  }
  return prevGesture;
}

// Re-render when the gesture may end; expiry itself is checked against the clock.
function renderPrevGestureAt(gesture, at) {
  clearTimeout(gesture.renderTimer);
  gesture.renderTimer = setTimeout(() => {
    if (prevGesture === gesture) updateControls();
  }, Math.max(0, at - Date.now()) + 1);
}

function endPrevGesture() {
  if (prevGesture) {
    clearTimeout(prevGesture.timer);
    clearTimeout(prevGesture.renderTimer);
  }
  prevGesture = null;
  updateControls();
}

function cancelPrev(reason) {
  if (!prevGesture) return;
  if (prevGesture.pending) {
    prevCounts.cancelled += 1;
    prevNote = `SECOND PREV CANCELLED (${reason})`;
  }
  endPrevGesture();
  renderCdTiming();
}

// Sends the queued second PREV once, only when every condition holds; otherwise
// waits for the cooldown or drops it. It is never retried.
function tryPendingPrev() {
  const gesture = prevGesture;
  if (!gesture || !gesture.pending || gesture.phase !== 'done' || controlBusy ||
      gesture.awaitingRefresh) return;
  const reason = prevBlocked();
  if (reason) { cancelPrev(reason); return; }
  const wait = lastCdCompleted + cdIntervalMs - Date.now();
  if (wait > 0) {
    clearTimeout(gesture.timer);
    gesture.timer = setTimeout(tryPendingPrev, wait);
    return;
  }
  clearTimeout(gesture.timer);
  gesture.phase = 'second';
  gesture.pending = false;
  prevNote = null;
  const token = gesture.token;
  gesture.token = null; // Single use, even client-side.
  dispatchCd('PREVIOUS', { pendingPrev: true, token }).finally(() => {
    if (prevGesture !== gesture) return;
    // Success or failure, never a retry: absorb the rest of the burst.
    gesture.phase = 'closing';
    prevNote = gesture.extra ? 'EXTRA TAP IGNORED' : null;
    renderPrevGestureAt(gesture, gesture.lastTap + PREV_DOUBLE_TAP_WINDOW_MS);
    updateControls();
  });
}

function onPrevGestureTap() {
  const gesture = prevGesture;
  if (gesture.pending || gesture.phase === 'second' || gesture.phase === 'closing') {
    // Third and later taps never add commands; say so instead of dropping silently.
    prevCounts.extra += 1;
    gesture.extra = true;
    gesture.lastTap = Date.now();
    prevNote = gesture.pending ? 'SECOND PREV PENDING · EXTRA TAP IGNORED'
      : gesture.phase === 'second' ? 'SECOND PREV SENT · EXTRA TAP IGNORED'
      : 'EXTRA TAP IGNORED';
    if (gesture.phase === 'closing') {
      renderPrevGestureAt(gesture, gesture.lastTap + PREV_DOUBLE_TAP_WINDOW_MS);
    }
  } else if (Date.now() - gesture.firstTap > PREV_DOUBLE_TAP_WINDOW_MS) {
    cdDropped.busy += 1;
  } else {
    prevCounts.queued += 1;
    gesture.lastTap = Date.now();
    gesture.pending = true;
    prevNote = 'SECOND PREV PENDING';
    tryPendingPrev();
  }
  updateControls();
  renderCdTiming();
}

async function dispatchCd(action, options = {}) {
  cdTiming = { click: Date.now(), postMs: null, server: {}, refresh: null,
    awaitingRefresh: false, enabledAt: null, readyAt: null, renderMs: null };
  renderCdTiming();
  const label = action === 'NEXT' ? 'NEXT' : 'PREV';
  return sendControl('/api/v1/cd/track', JSON.stringify({ action }),
    `${label} TX VERIFIED · TRACK CHANGE UNVERIFIED`,
    (data) => cdFailure(label, data), options);
}

async function firstPrev() {
  const now = Date.now();
  const gesture = { firstTap: now, lastTap: now, source: observedSource(), phase: 'first',
    pending: false, extra: false, timer: null, renderTimer: null };
  prevGesture = gesture;
  prevNote = null;
  renderPrevGestureAt(gesture, now + PREV_DOUBLE_TAP_WINDOW_MS);
  await dispatchCd('PREVIOUS', { onSettled: (ok, data) => {
    if (prevGesture !== gesture) return;
    if (!ok || !data || data.result !== 'SERVICE_TX_OK' || data.cd_result !== 'TX_VERIFIED') {
      // Failure, refusal, timeout or uncertainty: never follow it with another PREV.
      cancelPrev('FIRST PREV NOT CONFIRMED');
      return;
    }
    gesture.phase = 'done';
    gesture.token = typeof data.csrf_token === 'string' && data.csrf_token
      ? data.csrf_token : null;
    // Without a returned token (older Gateway), wait for the refresh as before.
    gesture.awaitingRefresh = !gesture.token;
    if (gesture.pending) tryPendingPrev();
  } });
  if (prevGesture !== gesture || gesture.phase !== 'done') return;
  gesture.awaitingRefresh = false;
  if (gesture.pending) tryPendingPrev();
  else updateControls();
}

// Leaving the page drops a pending tap; nothing is stored, so nothing replays.
function cancelOnHide() {
  if (typeof document.hidden !== 'boolean' || document.hidden) {
    cancelPrev('PAGE HIDDEN');
    cancelVolumeGesture();
    cancelSeek('PAGE HIDDEN');
  }
}
if (typeof document.addEventListener === 'function') {
  document.addEventListener('visibilitychange', cancelOnHide);
}
if (typeof globalThis.addEventListener === 'function') {
  globalThis.addEventListener('pagehide', () => {
    cancelPrev('PAGE HIDDEN');
    cancelVolumeGesture();
    cancelSeek('PAGE HIDDEN');
  });
  // A held seek also ends when focus leaves the page or a pointer is released
  // anywhere, in case the button's own events are lost.
  globalThis.addEventListener('blur', () => {
    cancelVolumeGesture();
    cancelSeek('BLUR');
  });
  globalThis.addEventListener('pointerup', () => cancelSeek('POINTERUP'));
  globalThis.addEventListener('pointercancel', () => cancelSeek('POINTERCANCEL'));
}

for (const button of cdButtons) {
  button.addEventListener('click', async () => {
    if (prevGestureActive() && button.dataset.cdAction === 'PREVIOUS') {
      onPrevGestureTap();
      return;
    }
    // Other taps while a request or its token refresh is pending are dropped.
    const dropped = button.disabled ? 'disabled' : controlBusy ? 'busy'
      : Date.now() - lastCdCompleted < cdIntervalMs ? 'interval' : null;
    if (dropped) {
      cdDropped[dropped] += 1;
      renderCdTiming();
      return;
    }
    prevNote = null;
    if (button.dataset.cdAction === 'PREVIOUS') await firstPrev();
    else await dispatchCd(button.dataset.cdAction);
  });
}

refresh();
setInterval(() => refresh(), 5000);
