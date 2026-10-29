// @ts-check
/**
 * Presentación de valores de resultados: números con formato, top-k como
 * lista compacta, objetos JSON en texto, nulos como raya.
 */

import { h } from '../core/dom.js';
import { num } from '../core/format.js';

/**
 * @param {any} v
 * @returns {boolean}
 */
export function isTopK(v) {
  return Array.isArray(v) && v.every((e) => e && typeof e === 'object' && 'value' in e && 'count' in e);
}

/**
 * Nodo para una celda de resultados.
 * @param {any} v
 * @returns {Node}
 */
export function cell(v) {
  if (v === null || v === undefined) return h('span', { class: 'faint' }, '—');
  if (typeof v === 'number') return document.createTextNode(num(v));
  if (typeof v === 'boolean') return document.createTextNode(v ? 'sí' : 'no');
  if (isTopK(v)) {
    return h(
      'span',
      { class: 'chips' },
      v.map((/** @type {{ value: any, count: number }} */ e) => h('span', null, String(e.value), ' ', h('b', null, num(e.count)))),
    );
  }
  if (typeof v === 'object') return document.createTextNode(JSON.stringify(v));
  return document.createTextNode(String(v));
}

/**
 * Etiqueta de grupo de una fila: los valores no numéricos, unidos.
 * @param {any[]} row
 * @returns {string}
 */
export function groupLabel(row) {
  const parts = row.filter((v) => v !== null && typeof v !== 'number' && typeof v !== 'object').map(String);
  return parts.join(' / ');
}
