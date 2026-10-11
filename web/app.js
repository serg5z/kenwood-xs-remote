const text = (id, value) => { document.getElementById(id).textContent = value; };
const muteButton = document.getElementById('mute-button');
const sourceButtons = [...document.querySelectorAll('.source-grid button[data-source]')];
let csrfToken = null;
let serviceReady = false;
let muteReady = false;
let sourceReady = false;
let enabledSources = [];
let controlBusy = false;
let lastSourceAttempt = 0;
let controlResult = null;
let refreshController = null;
let refreshVersion = 0;

function updateControls() {
  muteButton.disabled = controlBusy || !csrfToken || !serviceReady || !muteReady;
  for (const button of sourceButtons) {
    button.disabled = controlBusy || !csrfToken || !serviceReady || !sourceReady ||
      !enabledSources.includes(button.dataset.source);
  }
  const available = !controlBusy && serviceReady && csrfToken && (muteReady || sourceReady);
  text('availability-text', controlResult
    ? `${controlResult}${available ? '' : ' · CONTROLS UNAVAILABLE'}`
    : (available ? 'CONTROLS READY · RECEIVER ACTION UNVERIFIED'
      : 'CONTROLS UNAVAILABLE · SERVICE OR AUTHORIZATION NOT READY'));
}

function renderStatus(data) {
  text('gateway', data && data.gateway === 'online' ? 'Online' : 'Unavailable');
  if (!data || data.xs8_service !== 'connected' || !data.status) {
    text('service', data ? 'Unavailable' : 'Unknown');
    text('summary-state', data ? 'Service unavailable' : 'Connection lost');
    for (const id of ['state', 'monitor', 'tx', 'waveform']) text(id, '—');
    return;
  }
  const status = data.status;
  text('service', 'Connected');
  text('summary-state', status.state);
  text('state', status.state);
  text('monitor', status.monitor);
  text('tx', `${status.last_byte} · ${status.last_gpio_tx}`);
  text('waveform', status.last_waveform);
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
  renderStatus(statusData);
  updateControls();
}

async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  return { ok: response.ok, data: await response.json() };
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
  try {
    const status = await fetchJson('/api/v1/status', {
      cache: 'no-store', credentials: 'same-origin', signal: controller.signal,
    });
    statusData = status.data;
    if (status.ok && statusData.xs8_service === 'connected') {
      const s = statusData.status;
      if (s && s.state === 'IDLE' && s.monitor === 'HEALTHY' &&
          s.stop_owed === '0' && s.context_may_be_open === '0') {
        const session = await fetchJson('/api/v1/session', {
          cache: 'no-store', credentials: 'same-origin', signal: controller.signal,
        });
        if (session.ok && typeof session.data.csrf_token === 'string') {
          sessionData = session.data;
          if (session.data.source_available === true) {
            try {
              const capabilities = await fetchJson('/api/v1/capabilities', {
                cache: 'no-store', credentials: 'same-origin', signal: controller.signal,
              });
              if (capabilities.ok) capabilitiesData = capabilities.data;
            } catch (_) {
              // MUTE eligibility remains independent of source capabilities.
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
  csrfToken = null; muteReady = false; sourceReady = false;
  updateControls();
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 6000);
  try {
    const response = await fetchJson(url, {
      method: 'POST', credentials: 'same-origin', signal: controller.signal,
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': token },
      body,
    });
    controlResult = response.ok ? success : failure(response.data);
  } catch (_) {
    controlResult = url.endsWith('/mute')
      ? 'MUTE RESULT UNKNOWN · CHECK RECEIVER'
      : 'SOURCE RESULT UNKNOWN · CHECK RECEIVER';
  } finally {
    clearTimeout(timeout);
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

for (const button of sourceButtons) {
  button.addEventListener('click', async () => {
    if (button.disabled || controlBusy || Date.now() - lastSourceAttempt < 1000) return;
    lastSourceAttempt = Date.now();
    await sendControl('/api/v1/source', JSON.stringify({ source: button.dataset.source }),
      `${button.dataset.source} TX REPORTED · SELECTION UNVERIFIED`,
      (data) => `SOURCE ${data.result || 'FAILED'} · SELECTION UNVERIFIED`);
  });
}

refresh();
setInterval(() => refresh(), 5000);
