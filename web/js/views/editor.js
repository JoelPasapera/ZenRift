// @ts-check
/**
 * Editor ZQL: resaltado de sintaxis, validación en vivo contra el compilador
 * del servidor (POST /v1/check), plan de ejecución y creación de la consulta.
 */

import { api, ApiError } from '../api/client.js';
import { debounce, h, replace } from '../core/dom.js';
import { toast } from '../core/toast.js';
import { refreshCatalog, store } from '../state/store.js';
import { EXAMPLES } from '../zql/examples.js';
import { byteToIndex, renderHighlighted } from '../zql/highlight.js';

/** @typedef {import('../core/router.js').View} View */

const STARTER = `SELECT service, count(*) AS n, p99(latency_ms) AS p99
FROM requests
GROUP BY service
WINDOW TUMBLING 10s
EMIT EVERY 1s`;

/** @returns {View} */
export function editorView() {
  const nameInput = h('input', { name: 'name', required: true, pattern: '[A-Za-z0-9_.\\-]{1,64}', placeholder: 'p. ej. errores_por_servicio', autocomplete: 'off' });
  const fromSelect = h(
    'select',
    { name: 'from' },
    h('option', { value: 'now' }, 'Desde ahora'),
    h('option', { value: '1h' }, 'Desde hace una hora (lee el log)'),
    h('option', { value: '24h' }, 'Desde hace 24 horas (lee el log)'),
    h('option', { value: 'beginning' }, 'Desde el inicio del log'),
  );
  const textarea = h('textarea', { spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', 'aria-label': 'Consulta ZQL' });
  const highlight = h('pre', { 'aria-hidden': 'true' });
  const diagnostic = h('p', { class: 'diagnostic', role: 'status' });
  const plan = h('pre', { class: 'plan' }, 'El plan aparecerá aquí cuando la consulta compile.');
  const createBtn = h('button', { class: 'primary', type: 'submit' }, 'Crear consulta');
  const helpList = h('ul', { class: 'help-list' });
  /** @type {number | null} */
  let errorIndex = null;
  let lastChecked = '';
  let checking = 0;

  function paint() {
    renderHighlighted(highlight, textarea.value, errorIndex);
  }

  const check = debounce(async () => {
    const text = textarea.value;
    if (!text.trim()) {
      errorIndex = null;
      replace(diagnostic, '');
      paint();
      return;
    }
    if (text === lastChecked) return;
    const ticket = ++checking;
    try {
      const r = await api.post('/v1/check', text, 'text/plain');
      if (ticket !== checking) return;
      lastChecked = text;
      if (r.ok) {
        errorIndex = null;
        diagnostic.className = 'diagnostic fine';
        replace(diagnostic, `Compila. Columnas: ${r.columns.join(', ')}.`);
        replace(plan, r.explain);
      } else {
        errorIndex = byteToIndex(text, r.error.offset);
        diagnostic.className = 'diagnostic error';
        replace(diagnostic, `Línea ${r.error.line}, columna ${r.error.col}: ${r.error.error}`);
      }
    } catch (err) {
      diagnostic.className = 'diagnostic error';
      replace(diagnostic, `No se pudo validar: ${err instanceof Error ? err.message : err}`);
    }
    paint();
  }, 250);

  function onInput() {
    // Al editar, el subrayado de error deja de ser fiable hasta revalidar.
    errorIndex = null;
    paint();
    check();
  }

  /**
   * Inserta texto en la posición del cursor.
   * @param {string} text
   */
  function insert(text) {
    const { selectionStart: a, selectionEnd: b, value } = textarea;
    textarea.value = value.slice(0, a) + text + value.slice(b);
    textarea.selectionStart = textarea.selectionEnd = a + text.length;
    textarea.focus();
    onInput();
  }

  /**
   * @param {string} name
   * @param {string} zql
   */
  function load(name, zql) {
    nameInput.value = name;
    textarea.value = zql;
    lastChecked = '';
    onInput();
  }

  /** @param {SubmitEvent} e */
  async function submit(e) {
    e.preventDefault();
    const name = nameInput.value.trim();
    if (!name) {
      nameInput.focus();
      return;
    }
    createBtn.disabled = true;
    try {
      await api.post('/v1/queries', { name, zql: textarea.value, from: fromSelect.value });
      toast(`Consulta «${name}» creada`);
      await refreshCatalog();
      location.hash = `#/consultas/${encodeURIComponent(name)}`;
    } catch (err) {
      if (err instanceof ApiError && err.body && typeof err.body.offset === 'number') {
        errorIndex = byteToIndex(textarea.value, err.body.offset);
        paint();
      }
      diagnostic.className = 'diagnostic error';
      replace(diagnostic, err instanceof Error ? err.message : String(err));
    } finally {
      createBtn.disabled = false;
    }
  }

  async function loadHelp() {
    try {
      const r = await api.get('/v1/zql/functions');
      /** @param {{ name: string, help: string }} f */
      const item = (f) =>
        h('li', null, h('button', { type: 'button', onclick: () => insert(`${f.name}(`), title: 'Insertar en el cursor' }, f.name), ' ', h('span', { class: 'muted' }, f.help.replace(/^[^:]*:\s*/, '')));
      replace(helpList, h('li', null, h('strong', null, 'Agregaciones')), r.aggregates.map(item), h('li', null, h('strong', null, 'Funciones')), r.functions.map(item));
    } catch {
      replace(helpList, h('li', { class: 'muted' }, 'No se pudo cargar la ayuda.'));
    }
  }

  return {
    mount(el, route) {
      textarea.addEventListener('input', onInput);
      textarea.addEventListener('scroll', () => {
        highlight.scrollTop = textarea.scrollTop;
      });
      textarea.addEventListener('keydown', (e) => {
        if (e.key === 'Tab' && !e.shiftKey) {
          e.preventDefault();
          insert('  ');
        }
        if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
          e.preventDefault();
          createBtn.click();
        }
      });
      const editor = h('div', { class: 'editor' }, highlight, textarea);
      replace(
        el,
        h(
          'div',
          { class: 'view-head' },
          h('div', null, h('h1', null, 'Editor ZQL'), h('p', null, 'Escribe una consulta continua. Se compila mientras escribes; Ctrl+Enter la crea.')),
        ),
        h(
          'div',
          { class: 'columns' },
          h(
            'form',
            { class: 'stack', onsubmit: submit },
            h('div', { class: 'examples' }, h('span', { class: 'muted' }, 'Ejemplos:'), EXAMPLES.map((ex) => h('button', { type: 'button', onclick: () => load(ex.name, ex.zql) }, ex.title))),
            editor,
            diagnostic,
            h('div', { class: 'form-row' }, h('label', null, 'Nombre', nameInput), h('label', null, 'Qué eventos cuenta', fromSelect), createBtn),
            h('section', { class: 'sheet' }, h('header', null, h('h2', null, 'Plan')), plan),
          ),
          h('section', { class: 'sheet reference' }, h('header', null, h('h2', null, 'Referencia')), helpList),
        ),
      );
      const from = route.query.get('desde');
      const existing = from ? store.queries.find((q) => q.name === from) : null;
      if (existing) load(`${existing.name}_copia`, existing.zql);
      else load('', STARTER);
      loadHelp();
    },
  };
}
