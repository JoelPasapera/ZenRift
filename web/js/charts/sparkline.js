// @ts-check
/**
 * Minigráfica en SVG para listas (eventos por ventana de cada consulta).
 */

const NS = 'http://www.w3.org/2000/svg';

/**
 * @param {number[]} values
 * @param {{ width?: number, height?: number, label?: string }} [opts]
 * @returns {SVGSVGElement}
 */
export function sparkline(values, opts = {}) {
  const w = opts.width ?? 96;
  const hgt = opts.height ?? 22;
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', `0 0 ${w} ${hgt}`);
  svg.setAttribute('width', String(w));
  svg.setAttribute('height', String(hgt));
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', opts.label ?? 'Evolución reciente');
  if (values.length < 2) return svg;
  const max = Math.max(...values, 1);
  const min = Math.min(...values, 0);
  const span = max - min || 1;
  const pts = values.map((v, i) => {
    const x = (i / (values.length - 1)) * (w - 4) + 2;
    const y = hgt - 2 - ((v - min) / span) * (hgt - 4);
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  const line = document.createElementNS(NS, 'polyline');
  line.setAttribute('points', pts.join(' '));
  line.setAttribute('fill', 'none');
  line.setAttribute('stroke', 'var(--pen)');
  line.setAttribute('stroke-width', '1.5');
  line.setAttribute('stroke-linejoin', 'round');
  svg.appendChild(line);
  return svg;
}
