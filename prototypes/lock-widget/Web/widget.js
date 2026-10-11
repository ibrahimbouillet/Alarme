// Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
'use strict';
const card = document.querySelector('.glass');
const phases = new Set(['closed', 'idle', 'scheduled', 'ringing', 'silent', 'error']);
let hostActive = true;
let refreshTimer = 0;
function schedule() {
  clearTimeout(refreshTimer);
  refreshTimer = 0;
  if (!document.hidden && hostActive) refreshTimer = setTimeout(refresh, 30000);
}
function visibility() {
  document.documentElement.dataset.hidden = String(document.hidden || !hostActive);
  schedule();
}
function present(data) {
  if (!data || typeof data !== 'object') return;
  if (typeof data.hostActive === 'boolean') { hostActive = data.hostActive; visibility(); return; }
  for (const key of ['time', 'status', 'next', 'sound']) {
    if (typeof data[key] === 'string') document.getElementById(key).textContent = data[key].slice(0, 200);
  }
  card.dataset.phase = phases.has(data.phase) ? data.phase : 'closed';
}
async function refresh() {
  if (document.hidden || !hostActive) return;
  try {
    // This pseudo-origin is served entirely by IWidgetResourceProvider, without a web server.
    const response = await fetch('state.json', {cache:'no-store', credentials:'omit'});
    if (!response.ok) throw new Error('State unavailable');
    present(await response.json());
  } catch {
    present({time:'—', status:'ALARM state unavailable', next:'Open ALARM to check the engine', sound:'', phase:'closed'});
  } finally { schedule(); }
}
if (window.chrome?.webview) {
  window.chrome.webview.addEventListener('message', event => {
    try { present(typeof event.data === 'string' ? JSON.parse(event.data) : event.data); } catch {}
  });
  window.chrome.webview.postMessage('ready');
}
card.addEventListener('pointermove', event => {
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  const box = card.getBoundingClientRect();
  const x = Math.max(0, Math.min(1, (event.clientX-box.left)/box.width));
  const y = Math.max(0, Math.min(1, (event.clientY-box.top)/box.height));
  card.style.setProperty('--lx', `${x*100}%`);
  card.style.setProperty('--ly', `${y*100}%`);
  card.style.setProperty('--dx', `${(x-.5)*5}px`);
  card.style.setProperty('--dy', `${(y-.5)*5}px`);
});
card.addEventListener('pointerleave', () => {
  for (const key of ['--lx','--ly','--dx','--dy']) card.style.removeProperty(key);
});
document.addEventListener('visibilitychange', () => {
  visibility();
  if (!document.hidden && hostActive) refresh();
});
visibility();
refresh();
