// @ts-check
/**
 * Eventos en vivo: los eventos que llegan a un stream, filtrados en el
 * servidor con una expresión ZQL (la misma VM que usan las consultas).
 */

import { api } from '../api/client.js';
import { LiveStream } from '../api/live.js';
import { h, replace } from '../core/dom.js';
import { clock, num } from '../core/format.js';
import { store } from '../state/store.js';

/** @typedef {import('../core/router.js').View} View */

const MAX_LINES = 300;

/**
 * Representación compacta y coloreada de un evento JSON.
 * @param {any} value
 * @returns {Node}
 */
function renderJson(value) {
  if (value === null) return h('span', { class: 'v' }, 'null');
  if (typeof value === 'string') return h('span', { class: 's' }, JSON.stringify(value));
  if (typeof value !== 'object') return h('span', { class: 'v' }, String(value));
  const frag = document.createDocumentFragment();
  if (Array.isArray(value)) {
    frag.append('[');
    value.forEach((v, i) => {
      if (i) frag.append(', ');
      frag.append(renderJson(v));
    });
    frag.append(']');
    return frag;
  }
  frag.append('{');
  Object.entries(value).forEach(([k, v], i) => {
    if (i) frag.append(', ');
    frag.append(h('span', { class: 'k' }, k), ': ', renderJson(v));
  });
  frag.append('}');
  return frag;
}

/** @returns {View} */
export function tailView() {
  /** @type {LiveStream | null} */
  let live = null;
  let paused = false;
  let received = 0;
  let windowCount = 0;
  /** @type {ReturnType<typeof setInterval> | undefined} */
  let rateTimer;

  const streamSelect = h('select', { name: 'stream', 'aria-label': 'Stream' });
  const filterInput = h('input', { name: 'where', placeholder: "status >= 500 AND service = 'payments'", autocomplete: 'off', spellcheck: 'false', style: { fontFamily: 'var(--mono)' } });
  const pauseBtn = h('button', { type: 'button' }, 'Pausar');
  const status = h('p', { class: 'muted', role: 'status' });
  const diagnostic = h('p', { class: 'diagnostic' });
  const listEl = h('ul', { class: 'tail-list' });

  let baseStatus = '';

  /** @param {string} text */
  function setStatus(text) {
    baseStatus = text;
    replace(status, text);
  }

  /**
   * Valida el filtro con el compilador (como WHERE de una consulta) antes de
   * abrir el flujo: EventSource no deja leer el cuerpo de un error 400.
   * @param {string} where
   * @returns {Promise<boolean>}
   */
  async function validate(where) {
    if (!where.trim()) return true;
    const prefix = 'SELECT count(*) FROM x WHERE ';
    const r = await api.post('/v1/check', prefix + where, 'text/plain');
    if (r.ok) return true;
    const col = r.error.line === 1 ? Math.max(1, r.error.col - prefix.length) : r.error.col;
    diagnostic.className = 'diagnostic error';
    replace(diagnostic, `Filtro no válido (columna ${col}): ${r.error.error}`);
    return false;
  }

  async function start() {
    live?.close();
    live = null;
    replace(diagnostic);
    diagnostic.className = 'diagnostic';
    const stream = streamSelect.value;
    if (!stream) return;
    const where = filterInput.value;
    if (!(await validate(where))) return;
    replace(listEl);
    received = 0;
    const path = `/v1/streams/${encodeURIComponent(stream)}/tail${where.trim() ? `?where=${encodeURIComponent(where)}` : ''}`;
    live = new LiveStream(
      path,
      {
        events: (events) => {
          received += events.length;
          windowCount += events.length;
          if (paused) return;
          const now = Date.now();
          const frag = document.createDocumentFragment();
          for (let i = events.length - 1; i >= 0; i--) {
            frag.appendChild(h('li', { class: 'fresh' }, h('span', { class: 'faint' }, `${clock(now)}  `), renderJson(events[i])));
          }
          for (const li of listEl.querySelectorAll('li.fresh')) li.classList.remove('fresh');
          listEl.prepend(frag);
          while (listEl.children.length > MAX_LINES) listEl.lastElementChild?.remove();
        },
        dropped: (d) => {
          listEl.prepend(h('li', { class: 'faint' }, `… ${num(d.dropped)} eventos sin mostrar: el tail limita su ritmo para no saturar el navegador`));
        },
      },
      (state) => {
        if (state === 'live') setStatus(where.trim() ? `Escuchando «${stream}» con filtro` : `Escuchando todos los eventos de «${stream}»`);
        else if (state === 'connecting') setStatus('Conectando…');
        else setStatus('El flujo se cerró. Comprueba que el stream exista.');
      },
    );
  }

  return {
    mount(el, route) {
      const wanted = route.args[0] ?? store.streams[0]?.name ?? '';
      replace(
        streamSelect,
        store.streams.map((s) => h('option', { value: s.name, selected: s.name === wanted }, s.name)),
      );
      if (wanted && !store.streams.some((s) => s.name === wanted)) streamSelect.prepend(h('option', { value: wanted, selected: true }, wanted));
      pauseBtn.onclick = () => {
        paused = !paused;
        pauseBtn.textContent = paused ? 'Reanudar' : 'Pausar';
      };
      replace(
        el,
        h(
          'div',
          { class: 'view-head' },
          h('div', null, h('h1', null, 'Eventos en vivo'), h('p', null, 'Mira lo que entra a un stream mientras llega. El filtro es una expresión ZQL y se evalúa en el servidor.')),
        ),
        h(
          'form',
          {
            class: 'form-row',
            onsubmit: (/** @type {Event} */ e) => {
              e.preventDefault();
              start();
            },
          },
          h('label', { style: { flex: '0 1 200px' } }, 'Stream', streamSelect),
          h('label', { style: { flex: '1 1 360px' } }, 'Filtro (opcional)', filterInput),
          h('button', { class: 'primary', type: 'submit' }, 'Escuchar'),
          pauseBtn,
          h('button', { type: 'button', onclick: () => replace(listEl) }, 'Vaciar'),
        ),
        diagnostic,
        h('section', { class: 'sheet', style: { marginTop: '16px' } }, h('header', null, status), listEl),
      );
      streamSelect.onchange = () => start();
      if (store.streams.length === 0) setStatus('No hay streams todavía: envía eventos y vuelve aquí.');
      else start();
      rateTimer = setInterval(() => {
        if (live) replace(status, baseStatus, h('span', { class: 'faint' }, `  ${num(windowCount)} eventos/s, ${num(received)} recibidos`));
        windowCount = 0;
      }, 1000);
    },
    unmount() {
      live?.close();
      if (rateTimer) clearInterval(rateTimer);
    },
  };
}
