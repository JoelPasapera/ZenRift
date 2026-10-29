// @ts-check
/**
 * Escalas y marcas de eje "bonitas" (pasos 1-2-5) para valores y tiempos.
 */

/**
 * Marcas para el rango [min, max] con unas `count` divisiones aproximadas.
 * @param {number} min
 * @param {number} max
 * @param {number} [count]
 * @returns {{ min: number, max: number, step: number, ticks: number[] }}
 */
export function niceTicks(min, max, count = 4) {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return { min: 0, max: 1, step: 0.25, ticks: [0, 0.25, 0.5, 0.75, 1] };
  if (max === min) {
    const pad = Math.abs(max) > 0 ? Math.abs(max) * 0.5 : 1;
    min -= pad;
    max += pad;
  }
  const raw = (max - min) / Math.max(1, count);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
  const lo = Math.floor(min / step) * step;
  const hi = Math.ceil(max / step) * step;
  /** @type {number[]} */
  const ticks = [];
  for (let v = lo; v <= hi + step * 0.5; v += step) ticks.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  return { min: lo, max: hi, step, ticks };
}

const TIME_STEPS = [
  1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000, 900000, 1800000, 3600000, 7200000,
  10800000, 21600000, 43200000, 86400000,
];

/**
 * Paso temporal para que las etiquetas queden al menos a `minPx` píxeles.
 * @param {number} spanMs duración visible
 * @param {number} widthPx ancho disponible
 * @param {number} [minPx]
 * @returns {number}
 */
export function timeStep(spanMs, widthPx, minPx = 90) {
  const per = (spanMs / Math.max(1, widthPx)) * minPx;
  for (const s of TIME_STEPS) if (s >= per) return s;
  return TIME_STEPS[TIME_STEPS.length - 1];
}

/**
 * Marcas temporales alineadas al reloj local (múltiplos del paso).
 * @param {number} t0
 * @param {number} t1
 * @param {number} step
 * @returns {number[]}
 */
export function timeTicks(t0, t1, step) {
  const offset = new Date(t0).getTimezoneOffset() * 60000;
  const first = Math.ceil((t0 - offset) / step) * step + offset;
  /** @type {number[]} */
  const out = [];
  for (let t = first; t <= t1; t += step) out.push(t);
  return out;
}
