const text = (id, value) => { document.getElementById(id).textContent = value; };

async function refreshStatus() {
  try {
    const response = await fetch('/api/v1/status', { cache: 'no-store' });
    const data = await response.json();
    text('gateway', data.gateway === 'online' ? 'Online' : 'Unavailable');
    if (!response.ok || data.xs8_service !== 'connected') {
      text('service', 'Unavailable');
      text('summary-state', 'Service unavailable');
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
  } catch (_) {
    text('gateway', 'Unavailable');
    text('service', 'Unknown');
    text('summary-state', 'Connection lost');
    for (const id of ['state', 'monitor', 'tx', 'waveform']) text(id, '—');
  }
}

refreshStatus();
setInterval(refreshStatus, 5000);
