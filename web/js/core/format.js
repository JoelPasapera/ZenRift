// @ts-check
/**
 * Formato de números, tamaños y tiempos para el panel (es-PE).
 */

const LOCALE = 'es-PE';
const nf0 = new Intl.NumberFormat(LOCALE, { maximumFractionDigits: 0 });
const nf1 = new Intl.NumberFormat(LOCALE, { maximumFractionDigits: 1 });
const nf2 = new Intl.NumberFormat(LOCALE, { maximumFractionDigits: 2 });
const nf3 = new Intl.NumberFormat(LOCALE, { maximumSignificantDigits: 3 });
const compact = new Intl.NumberFormat(LOCALE, { notation: 'compact', maximumFractionDigits: 1 });
const timeFmt = new Intl.DateTimeFormat(LOCALE, { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
const shortTimeFmt = new Intl.DateTimeFormat(LOCALE, { hour: '2-digit', minute: '2-digit', hour12: false });
const dateTimeFmt = new Intl.DateTimeFormat(LOCALE, {
  day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
});

/**
 * Número con decimales según su magnitud: 12 345 | 123,4 | 1,23 | 0,0123.
 * @param {number | null | undefined} v
 * @returns {string}
 */
export function num(v) {
  if (v === null || v === undefined || !Number.isFinite(v)) return '—';
  const a = Math.abs(v);
  if (Number.isInteger(v) || a >= 1000) return nf0.format(v);
  if (a >= 100) return nf1.format(v);
  if (a >= 1) return nf2.format(v);
  return nf3.format(v);
}

/**
 * @param {number} v
 * @returns {string}
 */
export function compactNum(v) {
  return Number.isFinite(v) ? compact.format(v) : '—';
}

/**
 * @param {number} n
 * @returns {string}
 */
export function bytes(n) {
  if (!Number.isFinite(n)) return '—';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let u = 0;
  while (n >= 1024 && u < units.length - 1) {
    n /= 1024;
    u++;
  }
  return `${u === 0 ? nf0.format(n) : nf1.format(n)} ${units[u]}`;
}

/** @param {number} ms */
export const clock = (ms) => timeFmt.format(ms);
/** @param {number} ms */
export const shortClock = (ms) => shortTimeFmt.format(ms);
/** @param {number} ms */
export const dateTime = (ms) => dateTimeFmt.format(ms);

/**
 * "10 s", "1 min", "1 h 30 min", "250 ms".
 * @param {number} ms
 * @returns {string}
 */
export function duration(ms) {
  if (!Number.isFinite(ms)) return '—';
  if (ms < 1000) return `${nf0.format(ms)} ms`;
  const s = ms / 1000;
  if (s < 60) return `${nf1.format(s)} s`;
  const m = Math.floor(s / 60);
  const rs = Math.round(s % 60);
  if (m < 60) return rs ? `${m} min ${rs} s` : `${m} min`;
  const hh = Math.floor(m / 60);
  const rm = m % 60;
  if (hh < 48) return rm ? `${hh} h ${rm} min` : `${hh} h`;
  return `${nf1.format(hh / 24)} d`;
}

/**
 * "hace 12 s", "hace 3 min".
 * @param {number} ms
 * @param {number} [now]
 * @returns {string}
 */
export function ago(ms, now = Date.now()) {
  const d = Math.max(0, now - ms);
  if (d < 1500) return 'ahora';
  return `hace ${duration(Math.round(d / 1000) * 1000)}`;
}
