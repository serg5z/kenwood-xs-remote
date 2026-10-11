const text = (id, value) => { document.getElementById(id).textContent = value; };
const muteButton = document.getElementById('mute-button');
let csrfToken = null;
let serviceReady = false;
let sessionReady = false;
let muteBusy = false;
let muteResult = null;

function updateMute() {
  muteButton.disabled = !csrfToken || !sessionReady || !serviceReady || muteBusy;
  text('availability-text', csrfToken
    ? (sessionReady && serviceReady ? (muteResult || 'MUTE READY · RECEIVER ACTION UNVERIFIED') : 'MUTE UNAVAILABLE · SERVICE NOT READY')
    : 'CONTROL DISABLED · READ-ONLY PREVIEW');
}

async function refreshSession() {
  try {
    const response = await fetch('/api/v1/session', { cache: 'no-store', credentials: 'same-origin' });
    const data = await response.json();
    csrfToken = response.ok && typeof data.csrf_token === 'string' ? data.csrf_token : null;
    sessionReady = response.ok && data.mute_available === true;
  } catch (_) {
    csrfToken = null;
    sessionReady = false;
  }
  updateMute();
}

async function refreshStatus() {
  try {
    const response = await fetch('/api/v1/status', { cache: 'no-store' });
    const data = await response.json();
    text('gateway', data.gateway === 'online' ? 'Online' : 'Unavailable');
    if (!response.ok || data.xs8_service !== 'connected') {
      serviceReady = false;
      updateMute();
      text('service', 'Unavailable');
      text('summary-state', 'Service unavailable');
      for (const id of ['state', 'monitor', 'tx', 'waveform']) text(id, '—');
      return;
    }
    const status = data.status;
    serviceReady = status.state === 'IDLE' && status.monitor === 'HEALTHY' &&
      status.stop_owed === '0' && status.context_may_be_open === '0';
    updateMute();
    text('service', 'Connected');
    text('summary-state', status.state);
    text('state', status.state);
    text('monitor', status.monitor);
    text('tx', `${status.last_byte} · ${status.last_gpio_tx}`);
    text('waveform', status.last_waveform);
  } catch (_) {
    serviceReady = false;
    updateMute();
    text('gateway', 'Unavailable');
    text('service', 'Unknown');
    text('summary-state', 'Connection lost');
    for (const id of ['state', 'monitor', 'tx', 'waveform']) text(id, '—');
  }
}

muteButton.addEventListener('click', async () => {
  if (muteButton.disabled) return;
  muteBusy = true;
  updateMute();
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

refreshStatus();
refreshSession();
setInterval(() => { refreshStatus(); refreshSession(); }, 5000);
