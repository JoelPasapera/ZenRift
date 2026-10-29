// @ts-check
/**
 * Lienzo nítido en pantallas de alta densidad que se redimensiona solo, y la
 * paleta leída de las variables CSS (cambia con el tema claro/oscuro).
 */

/**
 * @typedef {{
 *   paper: string, grid: string, gridMajor: string, ink: string, inkSoft: string, inkFaint: string,
 *   pen: string, penFill: string, red: string, amber: string, series: string[], font: string, mono: string
 * }} Palette
 */

/** @returns {Palette} */
export function readPalette() {
  const cs = getComputedStyle(document.documentElement);
  /** @param {string} name */
  const v = (name) => cs.getPropertyValue(name).trim();
  return {
    paper: v('--paper-raised'),
    grid: v('--grid'),
    gridMajor: v('--grid-major'),
    ink: v('--ink'),
    inkSoft: v('--ink-soft'),
    inkFaint: v('--ink-faint'),
    pen: v('--pen'),
    penFill: v('--pen-fill'),
    red: v('--red'),
    amber: v('--amber'),
    series: [1, 2, 3, 4, 5, 6, 7, 8].map((i) => v(`--series-${i}`)),
    font: v('--font'),
    mono: v('--mono'),
  };
}

export class Surface {
  /**
   * @param {HTMLElement} container elemento con tamaño definido por CSS
   * @param {() => void} onResize se llama tras cada cambio de tamaño o de tema
   */
  constructor(container, onResize) {
    this.container = container;
    this.canvas = document.createElement('canvas');
    container.appendChild(this.canvas);
    const ctx = this.canvas.getContext('2d');
    if (!ctx) throw new Error('Canvas 2D no disponible');
    this.ctx = ctx;
    this.width = 0;
    this.height = 0;
    this.palette = readPalette();
    this.onResize = onResize;
    this.observer = new ResizeObserver(() => this.resize());
    this.observer.observe(container);
    this.scheme = window.matchMedia('(prefers-color-scheme: dark)');
    this.onScheme = () => {
      this.palette = readPalette();
      this.onResize();
    };
    this.scheme.addEventListener('change', this.onScheme);
    // Medida inicial sin avisar: el dueño aún se está construyendo. El
    // ResizeObserver avisa en cuanto empieza a observar.
    this.resize(false);
  }

  /** @param {boolean} [notify] */
  resize(notify = true) {
    const rect = this.container.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    this.width = Math.max(1, Math.floor(rect.width));
    this.height = Math.max(1, Math.floor(rect.height));
    this.canvas.width = Math.round(this.width * dpr);
    this.canvas.height = Math.round(this.height * dpr);
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (notify) this.onResize();
  }

  clear() {
    this.ctx.clearRect(0, 0, this.width, this.height);
  }

  destroy() {
    this.observer.disconnect();
    this.scheme.removeEventListener('change', this.onScheme);
    this.canvas.remove();
  }
}
