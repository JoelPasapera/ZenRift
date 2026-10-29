// @ts-check
/**
 * Pulso: el sistema de un vistazo. Arriba, el registrador con los eventos por
 * segundo de todo el motor; debajo, streams, alertas activas y consultas.
 */

import { h, replace } from '../core/dom.js';
import { ago, bytes, duration, num } from '../core/format.js';
import { Trace } from '../charts/trace.js';
import { refreshCatalog, store } from '../state/store.js';

/** @typedef {import('../core/router.js').View} View */

/** @returns {View} */
export function pulseView() {
  /** @type {Trace | null} */
  let trace = null;
  /** @type {(() => void)[]} */
  const offs = [];
  /** @type {ReturnType<typeof setInterval> | undefined} */
  let poll;

  const value = h('div', { class: 'pulse-value' }, '—');
  const side = h('div', { class: 'pulse-side' });
  const streamsBody = h('tbody');
  const alertsBox = h('div');
  const queriesBody = h('tbody');

  /** Ritmo por stream a partir de las dos últimas muestras de métricas. */
  function streamRates() {
    /** @type {Record<string, number>} */
    const rates = {};
    const m = store.metrics;
    if (m.length < 2) return rates;
    const a = m[m.length - 2];
    const b = m[m.length - 1];
    const dt = (b.t - a.t) / 1000 || 1;
    for (const [name, total] of Object.entries(b.stream_events ?? {})) {
      rates[name] = Math.max(0, (total - (a.stream_events?.[name] ?? total)) / dt);
    }
    return rates;
  }

  function renderReadout() {
    const last = store.metrics[store.metrics.length - 1];
    value.textContent = last ? num(Math.round(last.events_per_sec)) : '—';
    const firing = store.alerts.active.length;
    replace(
      side,
      h('div', null, h('strong', null, num(last?.events_stored ?? 0)), ' eventos guardados'),
      h('div', null, h('strong', null, num(store.streams.length)), ' streams, ', h('strong', null, num(store.queries.length)), ' consultas'),
      h('div', { class: firing ? 'danger' : null }, h('strong', null, num(firing)), firing === 1 ? ' alerta activa' : ' alertas activas'),
    );
  }

  function renderStreams() {
    const rates = streamRates();
    if (store.streams.length === 0) {
      replace(
        streamsBody,
        h('tr', null, h('td', { colspan: 6, class: 'empty' }, 'Aún no hay streams. Envía eventos con ', h('code', null, 'POST /v1/streams/<nombre>/events'), ' y aparecerán aquí.')),
      );
      return;
    }
    replace(
      streamsBody,
      store.streams.map((s) =>
        h(
          'tr',
          { class: 'link', onclick: () => (location.hash = `#/tail/${encodeURIComponent(s.name)}`), title: 'Ver sus eventos en vivo' },
          h('td', null, h('strong', null, s.name)),
          h('td', { class: 'n' }, num(rates[s.name] ?? 0)),
          h('td', { class: 'n', title: `${num(s.events)} recibidos desde el arranque` }, num(s.stored)),
          h('td', { class: 'n wide' }, String(s.partitions)),
          h('td', { class: 'n' }, bytes(s.disk_bytes)),
          h('td', { class: 'muted wide' }, s.last_event_ms ? ago(s.last_event_ms) : 'sin eventos'),
        ),
      ),
    );
  }

  function renderAlerts() {
    const active = store.alerts.active;
    if (active.length === 0) {
      replace(alertsBox, h('p', { class: 'empty' }, 'Ninguna alerta disparada. Las condiciones ALERT IF de las consultas se vigilan en cada ventana.'));
      return;
    }
    replace(
      alertsBox,
      h(
        'ul',
        { class: 'alert-list' },
        active.slice(0, 6).map((a) =>
          h(
            'li',
            { class: 'firing' },
            h('span', { class: 'alert-mark' }),
            h('a', { class: 'alert-title', href: `#/consultas/${encodeURIComponent(a.query)}` }, a.query),
            h('span', { class: 'alert-time' }, ago(a.since_ms)),
            h('span', { class: 'alert-meta' }, Object.entries(a.group).map(([k, v]) => `${k} = ${v}`).join(', ') || 'global', ': ', a.condition),
          ),
        ),
      ),
    );
  }

  function renderQueries() {
    if (store.queries.length === 0) {
      replace(
        queriesBody,
        h('tr', null, h('td', { colspan: 5, class: 'empty' }, 'Sin consultas todavía. ', h('a', { href: '#/editor' }, 'Escribe la primera en el editor ZQL'), '.')),
      );
      return;
    }
    replace(
      queriesBody,
      store.queries.map((q) => {
        const live = q.stats.partitions_live >= q.stats.partitions;
        const state = q.stats.alerts_firing
          ? h('span', { class: 'pill firing' }, 'alerta')
          : live
            ? h('span', { class: 'pill' }, 'en vivo')
            : h('span', { class: 'pill catching' }, `al día en ${num(q.stats.lag)}`);
        return h(
          'tr',
          { class: 'link', onclick: () => (location.hash = `#/consultas/${encodeURIComponent(q.name)}`) },
          h('td', null, h('strong', null, q.name), h('div', { class: 'faint' }, q.stream)),
          h('td', { class: 'wide' }, `${q.window.type === 'tumbling' ? 'tumbling' : 'hopping'} ${duration(q.window.size_ms)}`),
          h('td', { class: 'n' }, num(q.stats.matched)),
          h('td', { class: 'n wide' }, q.stats.late ? h('span', { class: 'danger' }, num(q.stats.late)) : '0'),
          h('td', { class: 'nowrap' }, state, ' ', h('span', { class: 'faint wide' }, q.stats.last_window_end_ms ? ago(q.stats.last_window_end_ms) : '')),
        );
      }),
    );
  }

  return {
    mount(el) {
      const chart = h('div', { class: 'chart chart-pulse' });
      replace(
        el,
        h(
          'div',
          { class: 'stack' },
          h(
            'section',
            { class: 'sheet pulse', 'aria-label': 'Eventos por segundo' },
            h('div', { class: 'pulse-readout' }, value, h('div', { class: 'pulse-label' }, 'eventos por segundo entrando al motor')),
            side,
            chart,
          ),
          h(
            'div',
            { class: 'columns' },
            h(
              'section',
              { class: 'sheet' },
              h('header', null, h('h2', null, 'Streams'), h('span', { class: 'aside' }, 'pulsa uno para ver sus eventos')),
              h(
                'div',
                { class: 'table-wrap' },
                h(
                  'table',
                  null,
                  h('thead', null, h('tr', null, h('th', null, 'Stream'), h('th', { class: 'n' }, 'ev/s'), h('th', { class: 'n' }, 'Guardados'), h('th', { class: 'n wide' }, 'Particiones'), h('th', { class: 'n' }, 'En disco'), h('th', { class: 'wide' }, 'Último'))),
                  streamsBody,
                ),
              ),
            ),
            h('section', { class: 'sheet' }, h('header', null, h('h2', null, 'Alertas activas'), h('a', { class: 'aside', href: '#/alertas' }, 'historial')), alertsBox),
          ),
          h(
            'section',
            { class: 'sheet' },
            h('header', null, h('h2', null, 'Consultas continuas'), h('a', { class: 'aside', href: '#/editor' }, 'nueva consulta')),
            h(
              'div',
              { class: 'table-wrap' },
              h(
                'table',
                null,
                h('thead', null, h('tr', null, h('th', null, 'Consulta'), h('th', { class: 'wide' }, 'Ventana'), h('th', { class: 'n' }, 'Eventos'), h('th', { class: 'n wide' }, 'Tardíos'), h('th', null, 'Estado'))),
                queriesBody,
              ),
            ),
          ),
        ),
      );
      trace = new Trace(chart);
      trace.set(store.metrics.map((m) => ({ t: m.t, v: m.events_per_sec })));
      renderReadout();
      renderStreams();
      renderAlerts();
      renderQueries();
      offs.push(
        store.bus.on('metrics', (m) => {
          trace?.push({ t: m.t, v: m.events_per_sec });
          renderReadout();
          renderStreams();
        }),
        store.bus.on('catalog', () => {
          renderStreams();
          renderQueries();
          renderReadout();
        }),
        store.bus.on('alerts', () => {
          renderAlerts();
          renderReadout();
        }),
      );
      poll = setInterval(() => refreshCatalog().catch(() => {}), 4000);
    },
    unmount() {
      offs.forEach((off) => off());
      if (poll) clearInterval(poll);
      trace?.destroy();
    },
  };
}
