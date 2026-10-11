const text = (id, value) => { document.getElementById(id).textContent = value; };
const muteButton = document.getElementById('mute-button');
const sourceButtons = [...document.querySelectorAll('.source-grid button[data-source]')];
let csrfToken = null;
let serviceReady = false;
let sessionReady = false;
let sourceReady = false;
let enabledSources = [];
let sourceBusy = false;
let lastSourceAttempt = 0;
let sourceResult = null;
let muteBusy = false;
let muteResult = null;

function updateControls() {
  muteButton.disabled = !csrfToken || !sessionReady || !serviceReady || muteBusy;
  for (const button of sourceButtons) {
    button.disabled = !csrfToken || !sourceReady || !serviceReady || sourceBusy ||
      !enabledSources.includes(button.dataset.source);
  }
  text('availability-text', csrfToken
    ? (serviceReady ? (sourceResult || muteResult || 'CONTROLS READY · RECEIVER ACTION UNVERIFIED')
      : 'CONTROLS UNAVAILABLE · SERVICE NOT READY')
    : 'CONTROL DISABLED · READ-ONLY PREVIEW');
}

async function refreshCapabilities() {
  try {
    const response = await fetch('/api/v1/capabilities', { cache: 'no-store' });
    const data = await response.json();
    const source = data.operations && data.operations.source;
    enabledSources = response.ok && source && source.enabled === true &&
      Array.isArray(source.available_sources) ? source.available_sources : [];
  } catch (_) {
    enabledSources = [];
  }
  updateControls();
}

async function refreshSession() {
  try {
    const response = await fetch('/api/v1/session', { cache: 'no-store', credentials: 'same-origin' });
    const data = await response.json();
    csrfToken = response.ok && typeof data.csrf_token === 'string' ? data.csrf_token : null;
    sessionReady = response.ok && data.mute_available === true;
    sourceReady = response.ok && data.source_available === true;
  } catch (_) {
    csrfToken = null;
    sessionReady = false;
    sourceReady = false;
  }
  updateControls();
}

async function refreshStatus() {
  try {
    const response = await fetch('/api/v1/status', { cache: 'no-store' });
    const data = await response.json();
    text('gateway', data.gateway === 'online' ? 'Online' : 'Unavailable');
    if (!response.ok || data.xs8_service !== 'connected') {
      serviceReady = false;
      updateControls();
      text('service', 'Unavailable');
      text('summary-state', 'Service unavailable');
      for (const id of ['state', 'monitor', 'tx', 'waveform']) text(id, '—');
      return;
    }
    const status = data.status;
    serviceReady = status.state === 'IDLE' && status.monitor === 'HEALTHY' &&
      status.stop_owed === '0' && status.context_may_be_open === '0';
    updateControls();
    text('service', 'Connected');
    text('summary-state', status.state);
    text('state', status.state);
    text('monitor', status.monitor);
    text('tx', `${status.last_byte} · ${status.last_gpio_tx}`);
    text('waveform', status.last_waveform);
  } catch (_) {
    serviceReady = false;
    updateControls();
    text('gateway', 'Unavailable');
    text('service', 'Unknown');
    text('summary-state', 'Connection lost');
    for (const id of ['state', 'monitor', 'tx', 'waveform']) text(id, '—');
  }
}

muteButton.addEventListener('click', async () => {
  if (muteButton.disabled) return;
  muteBusy = true;
  updateControls();
  try {
    const response = await fetch('/api/v1/mute', {
      method: 'POST', credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken },
      body: '{}',
    });
    const data = await response.json();
    muteResult = response.ok
      ? 'MUTE TX REPORTED · RECEIVER ACTION UNVERIFIED'
      : `MUTE ${data.result || 'FAILED'} · RECEIVER ACTION UNVERIFIED`;
  } catch (_) {
    muteResult = 'MUTE RESULT UNKNOWN · CHECK RECEIVER';
  } finally {
    csrfToken = null;
    sessionReady = false;
    muteBusy = false;
    await refreshSession();
    await refreshStatus();
  }
});

for (const button of sourceButtons) {
  button.addEventListener('click', async () => {
    if (button.disabled || sourceBusy || Date.now() - lastSourceAttempt < 1000) return;
    lastSourceAttempt = Date.now();
    sourceBusy = true;
    updateControls();
    try {
      const response = await fetch('/api/v1/source', {
        method: 'POST', credentials: 'same-origin',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken },
        body: JSON.stringify({ source: button.dataset.source }),
      });
      const data = await response.json();
      sourceResult = response.ok
        ? `${button.dataset.source} TX REPORTED · SELECTION UNVERIFIED`
        : `SOURCE ${data.result || 'FAILED'} · SELECTION UNVERIFIED`;
    } catch (_) {
      sourceResult = 'SOURCE RESULT UNKNOWN · CHECK RECEIVER';
    } finally {
      csrfToken = null;
      sourceReady = false;
      sourceBusy = false;
      await refreshSession();
      await refreshCapabilities();
      await refreshStatus();
    }
  });
}

refreshStatus();
refreshSession();
refreshCapabilities();
setInterval(() => { refreshStatus(); refreshSession(); refreshCapabilities(); }, 5000);
