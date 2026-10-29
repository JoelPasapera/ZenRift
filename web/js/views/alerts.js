// @ts-check
/**
 * Alertas: las que están disparadas ahora y el historial de transiciones.
 */

import { h, replace } from '../core/dom.js';
import { ago, dateTime, num } from '../core/format.js';
import { refreshAlerts, store } from '../state/store.js';

/**
 * @typedef {import('../core/router.js').View} View
 * @typedef {import('../state/store.js').Alert} Alert
 */

/**
 * @param {Record<string, any>} obj
 * @returns {string}
 */
function pairs(obj) {
  return Object.entries(obj)
    .map(([k, v]) => `${k} = ${typeof v === 'number' ? num(v) : Array.isArray(v) ? `${v.length} elementos` : v}`)
    .join(', ');
}

/**
 * @param {Alert} a
 * @param {boolean} current
 * @returns {HTMLLIElement}
 */
function item(a, current) {
  const group = pairs(a.group);
  return h(
    'li',
    { class: a.state === 'firing' ? 'firing' : null },
    h('span', { class: 'alert-mark', title: a.state === 'firing' ? 'disparada' : 'resuelta' }),
    h(
      'span',
      { class: 'alert-title' },
      h('a', { href: `#/consultas/${encodeURIComponent(a.query)}` }, a.query),
      group ? ` en ${group}` : '',
      current ? '' : a.state === 'firing' ? ' se disparó' : ' se resolvió',
    ),
    h('span', { class: 'alert-time', title: dateTime(a.at_ms) }, current ? `desde ${ago(a.since_ms)}` : ago(a.at_ms)),
    h('span', { class: 'alert-meta' }, `Condición: ${a.condition}. Valores: ${pairs(a.values)}`),
  );
}

/** @returns {View} */
export function alertsView() {
  const active = h('div');
  const history = h('div');
  /** @type {(() => void)[]} */
  const offs = [];

  function render() {
    replace(
      active,
      store.alerts.active.length
        ? h('ul', { class: 'alert-list' }, store.alerts.active.map((a) => item(a, true)))
        : h('p', { class: 'empty' }, 'No hay alertas disparadas ahora mismo.'),
    );
    replace(
      history,
      store.alerts.history.length
        ? h('ul', { class: 'alert-list' }, store.alerts.history.slice(0, 100).map((a) => item(a, false)))
        : h('p', { class: 'empty' }, 'Todavía no ha cambiado ninguna alerta. Añade ALERT IF a una consulta para vigilar una condición.'),
    );
  }

  return {
    mount(el) {
      replace(
        el,
        h('div', { class: 'view-head' }, h('div', null, h('h1', null, 'Alertas'), h('p', null, 'Una alerta se dispara cuando su condición se cumple en las ventanas seguidas que pide su FOR, y se resuelve en la primera ventana que deja de cumplirla.'))),
        h('div', { class: 'stack' }, h('section', { class: 'sheet' }, h('header', null, h('h2', null, 'Disparadas')), active), h('section', { class: 'sheet' }, h('header', null, h('h2', null, 'Historial')), history)),
      );
      render();
      offs.push(store.bus.on('alerts', render));
      refreshAlerts().catch(() => {});
      const timer = setInterval(render, 5000);
      offs.push(() => clearInterval(timer));
    },
    unmount() {
      offs.forEach((off) => off());
    },
  };
}
