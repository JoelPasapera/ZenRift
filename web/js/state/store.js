// @ts-check
/**
 * Estado compartido del panel: catálogo (streams y consultas), alertas y
 * métricas globales. Se alimenta del flujo /v1/live y de peticiones REST, y
 * avisa a las vistas con eventos.
 */

import { api } from '../api/client.js';
import { LiveStream } from '../api/live.js';
import { Emitter } from '../core/emitter.js';

/**
 * @typedef {{ t: number, events_per_sec: number, bytes_per_sec: number, late_per_sec: number,
 *   windows_per_sec: number, events_total: number, events_stored: number, streams: number, queries: number,
 *   alerts_active: number, stream_events: Record<string, number> }} Metrics
 *   (events_total cuenta lo recibido desde el arranque; events_stored, lo que guarda el log)
 * @typedef {{ name: string, partitions: number, ts_field: string, created_ms: number, events: number,
 *   stored: number, bytes_in: number, rejected: number, disk_bytes: number, last_event_ms: number,
 *   partition_stats: { partition: number, shard: number, start_offset: number, end_offset: number,
 *     segments: number, disk_bytes: number }[], queries: string[] }} Stream
 * @typedef {{ events: number, matched: number, late: number, dropped: number, lag: number,
 *   partitions: number, partitions_live: number, windows: number, partials: number,
 *   last_window_end_ms: number, alerts_firing: number }} QueryStats
 * @typedef {{ name: string, stream: string, zql: string, from: string, start_ms: number, created_ms: number,
 *   columns: string[], keys: string[], window: { type: string, size_ms: number, advance_ms: number,
 *   lateness_ms: number }, emit_every_ms: number, alert: { condition: string, for: number } | null,
 *   stats: QueryStats, explain?: string, latest?: Result | null, latest_partial?: Result | null }} Query
 * @typedef {{ query: string, partial: boolean, start: string, end: string, start_ms: number, end_ms: number,
 *   events: number, groups: number, truncated: boolean, columns: string[], rows: any[][] }} Result
 * @typedef {{ id: string, query: string, state: 'firing' | 'resolved', condition: string,
 *   group: Record<string, any>, values: Record<string, any>, at: string, at_ms: number,
 *   since: string, since_ms: number }} Alert
 * @typedef {'connecting' | 'live' | 'error'} LiveState
 *
 * @typedef {{
 *   metrics: Metrics,
 *   catalog: void,
 *   alerts: void,
 *   alert: Alert,
 *   state: LiveState,
 * }} StoreEvents
 */

export const store = {
  /** @type {Metrics[]} */
  metrics: [],
  /** @type {Stream[]} */
  streams: [],
  /** @type {Query[]} */
  queries: [],
  /** @type {{ active: Alert[], history: Alert[] }} */
  alerts: { active: [], history: [] },
  version: '',
  /** @type {LiveState} */
  state: /** @type {LiveState} */ ('connecting'),
  /** @type {Emitter<StoreEvents>} */
  bus: new Emitter(),
};

const MAX_METRICS = 600;

export async function refreshCatalog() {
  const [streams, queries] = await Promise.all([api.get('/v1/streams'), api.get('/v1/queries')]);
  store.streams = streams;
  store.queries = queries;
  store.bus.emit('catalog', undefined);
}

export async function refreshAlerts() {
  store.alerts = await api.get('/v1/alerts');
  store.bus.emit('alerts', undefined);
}

/** @type {LiveStream | null} */
let live = null;

/** Carga inicial y conexión al flujo de métricas, alertas y cambios de catálogo. */
export async function startStore() {
  const [stats, history] = await Promise.all([api.get('/v1/stats'), api.get('/v1/metrics')]);
  store.version = stats?.engine?.version ?? '';
  store.metrics = Array.isArray(history) ? history : [];
  await Promise.all([refreshCatalog(), refreshAlerts()]);
  live = new LiveStream(
    '/v1/live',
    {
      metrics: (m) => {
        store.metrics.push(m);
        if (store.metrics.length > MAX_METRICS) store.metrics.splice(0, store.metrics.length - MAX_METRICS);
        store.bus.emit('metrics', m);
      },
      alert: (a) => {
        const list = store.alerts.active.filter((x) => x.id !== a.id);
        if (a.state === 'firing') list.unshift(a);
        store.alerts = { active: list, history: [a, ...store.alerts.history].slice(0, 200) };
        store.bus.emit('alert', a);
        store.bus.emit('alerts', undefined);
      },
      catalog: () => {
        refreshCatalog().catch(() => {});
      },
    },
    (state) => {
      store.state = state;
      store.bus.emit('state', state);
    },
  );
}

export function stopStore() {
  live?.close();
}
