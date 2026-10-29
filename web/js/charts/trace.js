// @ts-check
/**
 * Registrador de pluma: la serie se dibuja sobre papel cuadriculado que avanza
 * hacia la izquierda en tiempo real, y la pluma sostiene el último valor en el
 * borde derecho ("ahora"), como un sismógrafo. Es el elemento vivo del panel.
 */

import { reducedMotion } from '../core/dom.js';
import { compactNum, shortClock } from '../core/format.js';
import { niceTicks } from './scale.js';
import { Surface } from './surface.js';

/** @typedef {{ t: number, v: number }} Sample */

export class Trace {
  /**
   * @param {HTMLElement} container
   * @param {{ windowMs?: number, top?: number }} [opts]
   */
  constructor(container, opts = {}) {
    this.windowMs = opts.windowMs ?? 300000;
    this.top = opts.top ?? 88;
    /** @type {Sample[]} */
    this.samples = [];
    this.yMax = 1;
    this.targetMax = 1;
    this.frame = 0;
    this.still = reducedMotion();
    this.surface = new Surface(container, () => this.draw());
    if (this.still) {
      this.timer = setInterval(() => this.draw(), 1000);
    } else {
      const loop = () => {
        this.draw();
        this.frame = requestAnimationFrame(loop);
      };
      this.frame = requestAnimationFrame(loop);
    }
  }

  /** @param {Sample[]} samples */
  set(samples) {
    this.samples = samples.slice();
    this.retarget();
    this.yMax = this.targetMax;
    this.draw();
  }

  /** @param {Sample} s */
  push(s) {
    this.samples.push(s);
    const cutoff = Date.now() - this.windowMs * 1.2;
    while (this.samples.length && this.samples[0].t < cutoff) this.samples.shift();
    this.retarget();
    if (this.still) this.draw();
  }

  retarget() {
    let max = 0;
    const from = Date.now() - this.windowMs;
    for (const s of this.samples) if (s.t >= from && s.v > max) max = s.v;
    this.targetMax = niceTicks(0, Math.max(max * 1.15, 1), 4).max;
  }

  draw() {
    const { ctx, width: W, height: H, palette: P } = this.surface;
    if (W < 10 || H < 10) return;
    // La escala vertical se acerca a su objetivo poco a poco: sin saltos.
    this.yMax += (this.targetMax - this.yMax) * (this.still ? 1 : 0.08);
    const now = Date.now();
    const t0 = now - this.windowMs;
    const penX = W - 64;
    const top = this.top;
    const bottom = H - 26;
    const plotH = Math.max(10, bottom - top);
    /** @param {number} t */
    const x = (t) => ((t - t0) / this.windowMs) * penX;
    /** @param {number} v */
    const y = (v) => bottom - (v / this.yMax) * plotH;

    this.surface.clear();
    ctx.fillStyle = P.paper;
    ctx.fillRect(0, 0, W, H);

    // Papel: líneas verticales cada 10 s (más marcadas cada minuto) que se desplazan.
    ctx.lineWidth = 1;
    const minor = 10000;
    for (let t = Math.ceil(t0 / minor) * minor; t <= now; t += minor) {
      const major = t % 60000 === 0;
      ctx.strokeStyle = major ? P.gridMajor : P.grid;
      const px = Math.round(x(t)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(px, top - 8);
      ctx.lineTo(px, bottom);
      ctx.stroke();
      if (major && px > 30 && px < penX - 30) {
        ctx.fillStyle = P.inkFaint;
        ctx.font = `12px ${P.font}`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillText(shortClock(t), px, bottom + 7);
      }
    }
    // Horizontales en valores redondos, con su etiqueta a la derecha de la pluma.
    const ticks = niceTicks(0, this.yMax, 4).ticks.filter((v) => v <= this.yMax * 1.0001);
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.font = `12px ${P.font}`;
    for (const v of ticks) {
      const py = Math.round(y(v)) + 0.5;
      ctx.strokeStyle = v === 0 ? P.gridMajor : P.grid;
      ctx.beginPath();
      ctx.moveTo(0, py);
      ctx.lineTo(penX, py);
      ctx.stroke();
      ctx.fillStyle = P.inkFaint;
      ctx.fillText(compactNum(v), W - 10, py);
    }

    // Línea de "ahora" donde escribe la pluma.
    ctx.strokeStyle = P.gridMajor;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(penX + 0.5, top - 8);
    ctx.lineTo(penX + 0.5, bottom);
    ctx.stroke();
    ctx.setLineDash([]);

    const pts = this.samples.filter((s) => s.t >= t0 - 5000);
    if (pts.length === 0) return;
    const last = pts[pts.length - 1];

    // Área bajo la traza y la traza en sí; del último dato a la pluma, plano.
    ctx.beginPath();
    pts.forEach((s, i) => (i ? ctx.lineTo(x(s.t), y(s.v)) : ctx.moveTo(x(s.t), y(s.v))));
    ctx.lineTo(penX, y(last.v));
    ctx.lineTo(penX, bottom);
    ctx.lineTo(x(pts[0].t), bottom);
    ctx.closePath();
    ctx.fillStyle = P.penFill;
    ctx.fill();

    ctx.beginPath();
    pts.forEach((s, i) => (i ? ctx.lineTo(x(s.t), y(s.v)) : ctx.moveTo(x(s.t), y(s.v))));
    ctx.lineTo(penX, y(last.v));
    ctx.strokeStyle = P.pen;
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.stroke();

    // La pluma.
    ctx.beginPath();
    ctx.arc(penX, y(last.v), 4.5, 0, Math.PI * 2);
    ctx.fillStyle = P.pen;
    ctx.fill();
    ctx.lineWidth = 2;
    ctx.strokeStyle = P.paper;
    ctx.stroke();
  }

  destroy() {
    cancelAnimationFrame(this.frame);
    if (this.timer) clearInterval(this.timer);
    this.surface.destroy();
  }
}
