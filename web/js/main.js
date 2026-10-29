// @ts-check
/**
 * Arranque del panel: estado global, barra superior, avisos de alertas y
 * enrutado entre vistas.
 */

import { onTokenNeeded } from './api/client.js';
import { $ } from './core/dom.js';
import { num } from './core/format.js';
import { Router } from './core/router.js';
import { toast } from './core/toast.js';
import { startStore, store } from './state/store.js';
import { alertsView } from './views/alerts.js';
import { editorView } from './views/editor.js';
import { pulseView } from './views/pulse.js';
import { queriesView } from './views/queries.js';
import { tailView } from './views/tail.js';

/** Pide el token con el diálogo nativo; resuelve null si se cancela. */
function askToken() {
  /** @type {HTMLDialogElement} */
  const dialog = $('#token-dialog');
  const form = /** @type {HTMLFormElement} */ (dialog.querySelector('form'));
  const input = /** @type {HTMLInputElement} */ (form.elements.namedItem('token'));
  return new Promise((resolve) => {
    dialog.addEventListener(
      'close',
      () => resolve(dialog.returnValue === 'ok' && input.value ? input.value : null),
      { once: true },
    );
    dialog.showModal();
  });
}

function renderBar() {
  const last = store.metrics[store.metrics.length - 1];
  const firing = store.alerts.active.length;
  const figures = $('#bar-figures');
  figures.replaceChildren();
  /**
   * @param {string} value
   * @param {string} label
   * @param {string} [cls]
   */
  const add = (value, label, cls) => {
    const span = document.createElement('span');
    if (cls) span.className = cls;
    const strong = document.createElement('strong');
    strong.textContent = value;
    span.append(strong, label);
    figures.appendChild(span);
  };
  add(last ? num(Math.round(last.events_per_sec)) : '—', 'ev/s');
  add(num(last?.events_stored ?? 0), 'guardados');
  add(num(firing), firing === 1 ? 'alerta' : 'alertas', firing ? 'alerting' : undefined);
  /** @type {HTMLElement} */
  const badge = $('#alert-badge');
  badge.hidden = firing === 0;
  badge.textContent = String(firing);
}

function renderState() {
  const dot = /** @type {HTMLElement} */ ($('#bar-status .dot'));
  dot.dataset.state = store.state;
  $('#bar-status-text').textContent =
    store.state === 'live' ? `En vivo${store.version ? `, v${store.version}` : ''}` : store.state === 'connecting' ? 'Reconectando…' : 'Sin conexión';
}

async function main() {
  onTokenNeeded(askToken);
  store.bus.on('metrics', renderBar);
  store.bus.on('alerts', renderBar);
  store.bus.on('state', renderState);
  store.bus.on('alert', (a) => {
    const group = Object.entries(a.group).map(([k, v]) => `${k} = ${v}`).join(', ');
    toast(
      a.state === 'firing' ? `Alerta en ${a.query}${group ? ` (${group})` : ''}: ${a.condition}` : `Resuelta: ${a.query}${group ? ` (${group})` : ''}`,
      { alarm: a.state === 'firing' },
    );
  });
  try {
    await startStore();
  } catch (err) {
    toast(`No se pudo cargar el estado del servidor: ${err instanceof Error ? err.message : err}`, { alarm: true });
  }
  renderBar();
  renderState();
  const router = new Router(
    $('#view'),
    { pulso: pulseView, consultas: queriesView, editor: editorView, tail: tailView, alertas: alertsView },
    'pulso',
  );
  router.start();
}

main();
