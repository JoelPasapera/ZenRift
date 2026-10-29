// @ts-check
/**
 * Utilidades mínimas de DOM: construcción de elementos sin plantillas HTML
 * (nunca se interpola texto como HTML, así que los datos de los eventos no
 * pueden inyectar marcado).
 */

/** @typedef {Node | string | number | null | undefined | false} Child */

/**
 * Crea un elemento con atributos, propiedades y manejadores.
 * Las claves "on*" con función se registran como eventos; `class`, `text`,
 * `style` (objeto o cadena) y `dataset` tienen tratamiento especial.
 * @template {keyof HTMLElementTagNameMap} K
 * @param {K} tag
 * @param {Record<string, any> | null} [attrs]
 * @param {...(Child | Child[])} children
 * @returns {HTMLElementTagNameMap[K]}
 */
export function h(tag, attrs, ...children) {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [key, value] of Object.entries(attrs)) {
      if (value === undefined || value === null || value === false) continue;
      if (key === 'class') el.className = String(value);
      else if (key === 'text') el.textContent = String(value);
      else if (key === 'style' && typeof value === 'object') Object.assign(el.style, value);
      else if (key === 'dataset' && typeof value === 'object') Object.assign(el.dataset, value);
      else if (key.startsWith('on') && typeof value === 'function') el.addEventListener(key.slice(2), value);
      else if (value === true) el.setAttribute(key, '');
      else el.setAttribute(key, String(value));
    }
  }
  append(el, children);
  return el;
}

/**
 * @param {Element} el
 * @param {(Child | Child[])[]} children
 */
function append(el, children) {
  for (const c of children) {
    if (Array.isArray(c)) append(el, c);
    else if (c instanceof Node) el.appendChild(c);
    else if (c !== null && c !== undefined && c !== false) el.appendChild(document.createTextNode(String(c)));
  }
}

/**
 * Vacía un elemento y, opcionalmente, le pone hijos nuevos.
 * @param {Element} el
 * @param {...(Child | Child[])} children
 */
export function replace(el, ...children) {
  el.replaceChildren();
  append(el, children);
}

/**
 * Busca un elemento obligatorio.
 * @template {Element} T
 * @param {string} selector
 * @param {ParentNode} [root]
 * @returns {T}
 */
export function $(selector, root = document) {
  const el = root.querySelector(selector);
  if (!el) throw new Error(`No existe ${selector}`);
  return /** @type {T} */ (el);
}

/**
 * Ejecuta `fn` como mucho una vez cada `ms` milisegundos (la última llamada gana).
 * @template {any[]} A
 * @param {(...args: A) => void} fn
 * @param {number} ms
 * @returns {(...args: A) => void}
 */
export function debounce(fn, ms) {
  /** @type {ReturnType<typeof setTimeout> | undefined} */
  let timer;
  return (...args) => {
    if (timer !== undefined) clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
}

/** @returns {boolean} */
export function reducedMotion() {
  return window.matchMedia('(prefers-reduced-motion: reduce)').matches;
}
