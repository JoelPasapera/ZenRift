// @ts-check
/**
 * Avisos breves en la esquina: confirmaciones, errores y alertas que saltan.
 */

import { $, h } from './dom.js';

/**
 * @param {string} text
 * @param {{ alarm?: boolean, ms?: number }} [opts]
 */
export function toast(text, opts = {}) {
  const box = $('#toasts');
  const el = h('div', { class: opts.alarm ? 'toast alarm' : 'toast', role: opts.alarm ? 'alert' : 'status' }, text);
  box.appendChild(el);
  setTimeout(() => el.remove(), opts.ms ?? (opts.alarm ? 9000 : 4000));
  while (box.children.length > 4) box.firstElementChild?.remove();
}
