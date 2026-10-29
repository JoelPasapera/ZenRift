// @ts-check
/**
 * Gráfica de líneas multiserie para la evolución de una columna de resultados
 * por ventana. Los puntos finales van con línea continua; el resultado parcial
 * de la ventana en curso, con trazo discontinuo y círculo hueco: así se ve
 * qué está cerrado y qué puede cambiar todavía.
 */

import { clock, num, shortClock } from '../core/format.js';
import { niceTicks, timeStep, timeTicks } from './scale.js';
import { Surface } from './surface.js';

/**
 * @typedef {{ t: number, v: number }} Point
 * @typedef {{ name: string, color: string, points: Point[], partial?: Point | null }} Series
 */

export class LineChart {
  /** @param {HTMLElement} container */
  constructor(container) {
    /** @type {Series[]} */
    this.series = [];
    /** @type {number | null} */
    this.hoverX = null;
    this.tooltip = document.createElement('div');
    this.tooltip.className = 'tooltip';
    this.tooltip.hidden = true;
    container.appendChild(this.tooltip);
    this.surface = new Surface(container, () => this.draw());
    this.onMove = (/** @type {PointerEvent} */ e) => {
      const r = this.surface.canvas.getBoundingClientRect();
      this.hoverX = e.clientX - r.left;
      this.draw();
    };
    this.onLeave = () => {
      this.hoverX = null;
      this.tooltip.hidden = true;
      this.draw();
    };
    this.surface.canvas.addEventListener('pointermove', this.onMove);
    this.surface.canvas.addEventListener('pointerleave', this.onLeave);
  }

  /** @param {Series[]} series */
  set(series) {
    this.series = series;
    this.draw();
  }

  draw() {
    const { ctx, width: W, height: H, palette: P } = this.surface;
    this.surface.clear();
    const left = 54;
    const right = W - 14;
    const top = 12;
    const bottom = H - 26;
    /** @type {number[]} */
    const ts = [];
    /** @type {number[]} */
    const vs = [];
    for (const s of this.series) {
      for (const p of s.points) {
        ts.push(p.t);
        vs.push(p.v);
      }
      if (s.partial) {
        ts.push(s.partial.t);
        vs.push(s.partial.v);
      }
    }
    if (ts.length === 0) {
      ctx.fillStyle = P.inkSoft;
      ctx.font = `14px ${P.font}`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('Esperando la primera ventana…', W / 2, H / 2);
      return;
    }
    let t0 = Math.min(...ts);
    let t1 = Math.max(...ts);
    if (t1 === t0) {
      t0 -= 30000;
      t1 += 30000;
    }
    const vMin = Math.min(0, ...vs);
    const vMax = Math.max(...vs);
    const yt = niceTicks(vMin, vMax * 1.08 || 1, 4);
    /** @param {number} t */
    const x = (t) => left + ((t - t0) / (t1 - t0)) * (right - left);
    /** @param {number} v */
    const y = (v) => bottom - ((v - yt.min) / (yt.max - yt.min)) * (bottom - top);

    ctx.lineWidth = 1;
    ctx.font = `12px ${P.font}`;
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'right';
    for (const v of yt.ticks) {
      const py = Math.round(y(v)) + 0.5;
      ctx.strokeStyle = v === 0 ? P.gridMajor : P.grid;
      ctx.beginPath();
      ctx.moveTo(left, py);
      ctx.lineTo(right, py);
      ctx.stroke();
      ctx.fillStyle = P.inkFaint;
      ctx.fillText(num(v), left - 8, py);
    }
    const step = timeStep(t1 - t0, right - left);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    // Las etiquetas de los extremos se desplazan para no salirse del lienzo,
    // y se omite la que pisaría a la anterior tras el desplazamiento.
    let labelEnd = -Infinity;
    for (const t of timeTicks(t0, t1, step)) {
      const px = Math.round(x(t)) + 0.5;
      ctx.strokeStyle = P.grid;
      ctx.beginPath();
      ctx.moveTo(px, top);
      ctx.lineTo(px, bottom);
      ctx.stroke();
      const label = step < 60000 ? clock(t) : shortClock(t);
      const half = ctx.measureText(label).width / 2;
      const cx = Math.min(Math.max(px, half + 2), W - half - 2);
      if (cx - half < labelEnd + 8) continue;
      labelEnd = cx + half;
      ctx.fillStyle = P.inkFaint;
      ctx.fillText(label, cx, bottom + 7);
    }

    const showDots = ts.length / Math.max(1, this.series.length) < 40;
    for (const s of this.series) {
      ctx.strokeStyle = s.color;
      ctx.fillStyle = s.color;
      ctx.lineWidth = 1.75;
      ctx.lineJoin = 'round';
      ctx.beginPath();
      s.points.forEach((p, i) => (i ? ctx.lineTo(x(p.t), y(p.v)) : ctx.moveTo(x(p.t), y(p.v))));
      ctx.stroke();
      if (showDots)
        for (const p of s.points) {
          ctx.beginPath();
          ctx.arc(x(p.t), y(p.v), 2.2, 0, Math.PI * 2);
          ctx.fill();
        }
      if (s.partial) {
        const lastPt = s.points[s.points.length - 1];
        if (lastPt) {
          ctx.setLineDash([4, 3]);
          ctx.beginPath();
          ctx.moveTo(x(lastPt.t), y(lastPt.v));
          ctx.lineTo(x(s.partial.t), y(s.partial.v));
          ctx.stroke();
          ctx.setLineDash([]);
        }
        ctx.beginPath();
        ctx.arc(x(s.partial.t), y(s.partial.v), 3.5, 0, Math.PI * 2);
        ctx.fillStyle = P.paper;
        ctx.fill();
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
    }

    // Cruz y detalle de la ventana más cercana al puntero.
    if (this.hoverX === null || this.hoverX < left || this.hoverX > right) {
      this.tooltip.hidden = true;
      return;
    }
    const tHover = t0 + ((this.hoverX - left) / (right - left)) * (t1 - t0);
    let best = ts[0];
    for (const t of ts) if (Math.abs(t - tHover) < Math.abs(best - tHover)) best = t;
    const px = Math.round(x(best)) + 0.5;
    ctx.strokeStyle = P.inkSoft;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(px, top);
    ctx.lineTo(px, bottom);
    ctx.stroke();
    /** @type {{ name: string, color: string, v: number, partial: boolean }[]} */
    const rows = [];
    for (const s of this.series) {
      const p = s.points.find((q) => q.t === best);
      if (p) rows.push({ name: s.name, color: s.color, v: p.v, partial: false });
      else if (s.partial && s.partial.t === best) rows.push({ name: s.name, color: s.color, v: s.partial.v, partial: true });
    }
    rows.sort((a, b) => b.v - a.v);
    this.tooltip.replaceChildren();
    const head = document.createElement('div');
    head.textContent = `Ventana hasta las ${clock(best)}${rows.some((r) => r.partial) ? ' (parcial)' : ''}`;
    head.className = 'muted';
    this.tooltip.appendChild(head);
    for (const r of rows.slice(0, 10)) {
      const line = document.createElement('div');
      const sw = document.createElement('span');
      sw.className = 'swatch';
      sw.style.background = r.color;
      line.append(sw, `${r.name}: `);
      const b = document.createElement('strong');
      b.textContent = num(r.v);
      line.appendChild(b);
      this.tooltip.appendChild(line);
    }
    this.tooltip.hidden = false;
    const tw = this.tooltip.offsetWidth;
    this.tooltip.style.left = `${Math.min(Math.max(px + 12, 0), W - tw - 4)}px`;
    this.tooltip.style.top = `${top + 4}px`;
  }

  destroy() {
    this.surface.canvas.removeEventListener('pointermove', this.onMove);
    this.surface.canvas.removeEventListener('pointerleave', this.onLeave);
    this.tooltip.remove();
    this.surface.destroy();
  }
}
