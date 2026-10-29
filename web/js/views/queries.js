// @ts-check
/**
 * Consultas: lista a la izquierda y, a la derecha, la consulta elegida en vivo
 * (gráfica por grupo, ventanas recientes, tabla del último resultado y plan).
 */

import { api, ApiError } from '../api/client.js';
import { LiveStream } from '../api/live.js';
import { LineChart } from '../charts/lines.js';
import { h, replace } from '../core/dom.js';
import { clock, duration, num } from '../core/format.js';
import { toast } from '../core/toast.js';
import { readPalette } from '../charts/surface.js';
import { refreshCatalog, store } from '../state/store.js';
import { cell, groupLabel } from './values.js';

/**
 * @typedef {import('../core/router.js').View} View
 * @typedef {import('../state/store.js').Query} Query
 * @typedef {import('../state/store.js').Result} Result
 */

const HISTORY = 120;

/** @returns {View} */
export function queriesView() {
  /** @type {string | null} */
  let selected = null;
  /** @type {Query | null} */
  let query = null;
  /** @type {Result[]} */
  let finals = [];
  /** @type {Result | null} */
  let partial = null;
  /** @type {LiveStream | null} */
  let live = null;
  /** @type {LineChart | null} */
  let chart = null;
  let column = -1;
  /** @type {(() => void)[]} */
  const offs = [];

  const list = h('ul', { class: 'query-list' });
  const detail = h('div', { class: 'stack' });
  const facts = h('p', { class: 'facts' });
  const columnSelect = h('select', { 'aria-label': 'Columna a graficar' });
  const chartBox = h('div', { class: 'chart chart-lines' });
  const legend = h('div', { class: 'legend' });
  const windowsStrip = h('div', { class: 'windows', 'aria-hidden': 'true' });
  const tableHead = h('p', { class: 'muted' });
  const table = h('table');
  const planBox = h('pre', { class: 'plan' });
  const zqlBox = h('pre', { class: 'plan' });

  function renderList() {
    if (store.queries.length === 0) {
      replace(list, h('li', null, h('p', { class: 'empty' }, 'No hay consultas. ', h('a', { href: '#/editor' }, 'Crea una en el editor'), '.')));
      return;
    }
    replace(
      list,
      store.queries.map((q) =>
        h(
          'li',
          null,
          h(
            'a',
            { href: `#/consultas/${encodeURIComponent(q.name)}`, 'aria-current': q.name === selected ? 'true' : null },
            q.name,
            q.stats.alerts_firing ? h('span', { class: 'pill firing', style: { marginLeft: '6px' } }, 'alerta') : null,
            h('small', null, `${q.stream}, ${q.window.type} ${duration(q.window.size_ms)}`),
          ),
        ),
      ),
    );
  }

  /** Índices de columnas numéricas del último resultado. */
  function numericColumns() {
    const sample = latest();
    if (!sample) return [];
    /** @type {number[]} */
    const out = [];
    sample.columns.forEach((_, i) => {
      if (sample.rows.some((r) => typeof r[i] === 'number')) out.push(i);
    });
    return out;
  }

  /** @returns {Result | null} */
  function latest() {
    const last = finals[finals.length - 1] ?? null;
    if (partial && (!last || partial.end_ms > last.end_ms)) return partial;
    return last;
  }

  function renderFacts() {
    if (!query) return;
    const s = query.stats;
    const live = s.partitions_live >= s.partitions;
    replace(
      facts,
      h('span', null, 'Stream ', h('strong', null, query.stream)),
      h('span', null, `${query.window.type === 'tumbling' ? 'Ventana tumbling' : 'Ventana hopping'} de `, h('strong', null, duration(query.window.size_ms)),
        query.window.type === 'hopping' ? ` cada ${duration(query.window.advance_ms)}` : '', `, lateness ${duration(query.window.lateness_ms)}`),
      query.emit_every_ms ? h('span', null, 'Parciales cada ', h('strong', null, duration(query.emit_every_ms))) : null,
      h('span', null, h('strong', null, num(s.events)), ' eventos vistos'),
      h('span', null, h('strong', null, num(s.matched)), ' pasan el filtro'),
      h('span', { class: s.late ? 'danger' : null }, h('strong', null, num(s.late)), ' tardíos'),
      live ? h('span', { class: 'ok' }, 'En vivo') : h('span', { class: 'danger' }, 'Leyendo el log: faltan ', h('strong', null, num(s.lag)), ' eventos'),
      query.alert ? h('span', null, 'Alerta si ', h('strong', null, query.alert.condition), query.alert.for > 1 ? ` durante ${query.alert.for} ventanas` : '') : null,
    );
  }

  function renderColumnSelect() {
    const r = latest();
    const cols = numericColumns();
    if (!r || cols.length === 0) {
      replace(columnSelect);
      return;
    }
    const hasGroups = r.rows.some((row) => groupLabel(row) !== '');
    if (!cols.includes(column) && column !== -2) column = cols[0];
    replace(
      columnSelect,
      cols.map((i) => h('option', { value: String(i), selected: i === column }, r.columns[i])),
      hasGroups ? null : h('option', { value: '-2', selected: column === -2 }, 'todas las columnas'),
    );
  }

  function renderChart() {
    if (!chart) return;
    const r = latest();
    const palette = readPalette().series;
    if (!r) {
      chart.set([]);
      replace(legend);
      return;
    }
    const hasGroups = r.rows.some((row) => groupLabel(row) !== '');
    /** @type {import('../charts/lines.js').Series[]} */
    let series = [];
    if (hasGroups && column >= 0) {
      // Los 8 grupos con mayor valor en la ventana más reciente.
      const top = [...r.rows]
        .filter((row) => typeof row[column] === 'number')
        .sort((a, b) => b[column] - a[column])
        .slice(0, 8)
        .map(groupLabel);
      series = top.map((label, i) => ({
        name: label,
        color: palette[i % palette.length],
        points: finals.flatMap((res) => {
          const row = res.rows.find((x) => groupLabel(x) === label);
          return row && typeof row[column] === 'number' ? [{ t: res.end_ms, v: row[column] }] : [];
        }),
        partial: (() => {
          if (!partial || (finals.length && partial.end_ms <= finals[finals.length - 1].end_ms)) return null;
          const row = partial.rows.find((x) => groupLabel(x) === label);
          return row && typeof row[column] === 'number' ? { t: partial.end_ms, v: row[column] } : null;
        })(),
      }));
    } else {
      const cols = column === -2 ? numericColumns() : [column];
      series = cols.map((c, i) => ({
        name: r.columns[c],
        color: palette[i % palette.length],
        points: finals.flatMap((res) => (typeof res.rows[0]?.[c] === 'number' ? [{ t: res.end_ms, v: res.rows[0][c] }] : [])),
        partial:
          partial && typeof partial.rows[0]?.[c] === 'number' && (!finals.length || partial.end_ms > finals[finals.length - 1].end_ms)
            ? { t: partial.end_ms, v: partial.rows[0][c] }
            : null,
      }));
    }
    chart.set(series);
    replace(legend, series.map((s) => h('span', { style: `--c:${s.color}` }, s.name || '(vacío)')));
  }

  function renderWindows() {
    const recent = finals.slice(-40);
    const p = partial && (!recent.length || partial.end_ms > recent[recent.length - 1].end_ms) ? partial : null;
    const values = recent.map((r) => r.events);
    if (p) values.push(p.events);
    const max = Math.max(1, ...values);
    replace(
      windowsStrip,
      recent.map((r) => h('span', { style: { height: `${Math.max(4, (r.events / max) * 100)}%` }, title: `${clock(r.start_ms)}–${clock(r.end_ms)}: ${num(r.events)} eventos` })),
      p ? h('span', { class: 'partial', style: { height: `${Math.max(4, (p.events / max) * 100)}%` }, title: `Ventana en curso: ${num(p.events)} eventos` }) : null,
    );
  }

  function renderTable() {
    const r = latest();
    if (!r) {
      replace(tableHead, 'Todavía no se ha cerrado ninguna ventana. La primera llega cuando la marca de agua supere su final.');
      replace(table);
      return;
    }
    replace(
      tableHead,
      `Ventana de ${clock(r.start_ms)} a ${clock(r.end_ms)}`,
      r.partial ? h('span', { class: 'pill catching', style: { marginLeft: '8px' } }, 'parcial, puede cambiar') : h('span', { class: 'pill', style: { marginLeft: '8px' } }, 'cerrada'),
      `  ${num(r.events)} eventos en ${num(r.groups)} grupo${r.groups === 1 ? '' : 's'}`,
      r.truncated ? h('span', { class: 'faint' }, ' (se muestran las primeras filas)') : null,
    );
    replace(
      table,
      h('thead', null, h('tr', null, r.columns.map((c, i) => h('th', { class: r.rows.some((row) => typeof row[i] === 'number') ? 'n' : null }, c)))),
      h(
        'tbody',
        null,
        r.rows.length === 0
          ? h('tr', null, h('td', { colspan: r.columns.length, class: 'empty' }, 'Ningún grupo cumple el HAVING en esta ventana.'))
          : r.rows.map((row) => h('tr', null, row.map((v) => h('td', { class: typeof v === 'number' ? 'n' : null }, cell(v))))),
      ),
    );
  }

  function renderAll() {
    renderFacts();
    renderColumnSelect();
    renderChart();
    renderWindows();
    renderTable();
  }

  /** @param {string} name */
  async function load(name) {
    live?.close();
    live = null;
    finals = [];
    partial = null;
    query = null;
    column = -1;
    selected = name;
    renderList();
    try {
      const [q, res] = await Promise.all([
        api.get(`/v1/queries/${encodeURIComponent(name)}`),
        api.get(`/v1/queries/${encodeURIComponent(name)}/results?last=${HISTORY}`),
      ]);
      if (selected !== name) return;
      query = q;
      finals = res.results ?? [];
      partial = res.partial ?? null;
    } catch (err) {
      replace(detail, h('p', { class: 'empty' }, err instanceof ApiError && err.status === 404 ? `La consulta «${name}» ya no existe.` : String(err)));
      return;
    }
    buildDetail();
    renderAll();
    live = new LiveStream(`/v1/queries/${encodeURIComponent(name)}/stream`, {
      result: (r) => {
        finals.push(r);
        if (finals.length > HISTORY) finals.shift();
        renderAll();
      },
      partial: (r) => {
        partial = r;
        renderColumnSelect();
        renderChart();
        renderWindows();
        renderTable();
      },
    });
  }

  function buildDetail() {
    if (!query) return;
    const q = query;
    chart?.destroy();
    replace(chartBox);
    chart = new LineChart(chartBox);
    replace(planBox, q.explain ?? '');
    replace(zqlBox, q.zql);
    replace(
      detail,
      h(
        'div',
        { class: 'view-head' },
        h('h1', null, q.name),
        h('a', { href: `#/editor?desde=${encodeURIComponent(q.name)}` }, 'Copiar al editor'),
        h('button', { class: 'danger-btn', onclick: () => remove(q.name) }, 'Borrar consulta'),
      ),
      facts,
      h(
        'section',
        { class: 'sheet' },
        h('header', null, h('h2', null, 'Evolución por ventana'), h('label', { class: 'aside', style: { flexDirection: 'row', alignItems: 'center' } }, 'Columna ', columnSelect)),
        chartBox,
        legend,
        h('div', { class: 'faint', style: { marginTop: '12px', fontSize: 'var(--step--1)' } }, 'Eventos por ventana (la rayada es la ventana en curso)'),
        windowsStrip,
      ),
      h('section', { class: 'sheet' }, h('header', null, h('h2', null, 'Último resultado')), tableHead, h('div', { class: 'table-wrap', style: { marginTop: '8px' } }, table)),
      h('details', { class: 'sheet' }, h('summary', null, 'Plan de ejecución'), planBox),
      h('details', { class: 'sheet' }, h('summary', null, 'Texto ZQL'), zqlBox),
    );
    columnSelect.onchange = () => {
      column = Number(columnSelect.value);
      renderChart();
    };
  }

  /** @param {string} name */
  async function remove(name) {
    if (!confirm(`¿Borrar la consulta «${name}»? Su historial de resultados se pierde; los eventos del stream no.`)) return;
    try {
      await api.del(`/v1/queries/${encodeURIComponent(name)}`);
      toast(`Consulta «${name}» borrada`);
      await refreshCatalog();
      location.hash = '#/consultas';
    } catch (err) {
      toast(`No se pudo borrar: ${err instanceof Error ? err.message : err}`, { alarm: true });
    }
  }

  return {
    mount(el, route) {
      replace(el, h('div', { class: 'query-layout' }, h('nav', { 'aria-label': 'Consultas' }, list), detail));
      offs.push(
        store.bus.on('catalog', () => {
          renderList();
          const fresh = store.queries.find((q) => q.name === selected);
          if (fresh && query) {
            query = { ...query, stats: fresh.stats };
            renderFacts();
          }
        }),
      );
      const name = route.args[0] ?? store.queries[0]?.name;
      if (name) load(name);
      else {
        renderList();
        replace(detail, h('p', { class: 'empty' }, 'Cuando crees una consulta, aquí verás sus resultados ventana a ventana.'));
      }
      const poll = setInterval(() => refreshCatalog().catch(() => {}), 3000);
      offs.push(() => clearInterval(poll));
    },
    unmount() {
      offs.forEach((off) => off());
      live?.close();
      chart?.destroy();
    },
  };
}

