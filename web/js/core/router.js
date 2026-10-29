// @ts-check
/**
 * Enrutado por hash: #/seccion/arg?clave=valor. Cada vista se monta en el
 * contenedor principal y se desmonta (cerrando sus flujos) al salir.
 */

/**
 * @typedef {{ args: string[], query: URLSearchParams }} Route
 * @typedef {{ mount(el: HTMLElement, route: Route): void | Promise<void>, unmount?(): void }} View
 */

export class Router {
  /**
   * @param {HTMLElement} outlet
   * @param {Record<string, () => View>} views
   * @param {string} fallback
   */
  constructor(outlet, views, fallback) {
    this.outlet = outlet;
    this.views = views;
    this.fallback = fallback;
    /** @type {View | null} */
    this.current = null;
    this.onChange = () => this.render();
  }

  start() {
    window.addEventListener('hashchange', this.onChange);
    this.render();
  }

  /**
   * @param {string} name
   * @param {string[]} [args]
   */
  go(name, args = []) {
    location.hash = `#/${[name, ...args.map(encodeURIComponent)].join('/')}`;
  }

  render() {
    const raw = location.hash.replace(/^#\/?/, '');
    const [pathPart, queryPart = ''] = raw.split('?');
    const segments = pathPart.split('/').filter(Boolean).map((s) => decodeURIComponent(s));
    let name = segments[0] ?? this.fallback;
    if (!(name in this.views)) name = this.fallback;
    if (this.current?.unmount) this.current.unmount();
    this.outlet.replaceChildren();
    const view = this.views[name]();
    this.current = view;
    for (const a of document.querySelectorAll('.rail a')) {
      if (!(a instanceof HTMLElement)) continue;
      if (a.dataset.route === name) a.setAttribute('aria-current', 'page');
      else a.removeAttribute('aria-current');
    }
    const result = view.mount(this.outlet, { args: segments.slice(1), query: new URLSearchParams(queryPart) });
    if (result instanceof Promise) result.catch((err) => console.error(err));
    this.outlet.focus({ preventScroll: true });
  }
}
