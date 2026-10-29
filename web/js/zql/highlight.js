// @ts-check
/**
 * Resaltado de sintaxis de ZQL. Reproduce las reglas del lexer del servidor
 * (palabras clave sin distinguir mayúsculas, duraciones pegadas al número,
 * comentarios -- y bloque, cadenas con '' escapada) para colorear el editor.
 */

const KEYWORDS = new Set([
  'select', 'from', 'where', 'group', 'by', 'having', 'order', 'limit', 'window', 'tumbling', 'hopping', 'sliding',
  'every', 'lateness', 'allowed', 'emit', 'final', 'alert', 'if', 'for', 'windows', 'window', 'ventanas', 'as',
  'and', 'or', 'not', 'in', 'is', 'null', 'true', 'false', 'like', 'ilike', 'between', 'case', 'when', 'then',
  'else', 'end', 'asc', 'desc', 'distinct',
]);

/** @typedef {'kw' | 'fn' | 'num' | 'dur' | 'str' | 'com' | 'op' | 'id' | 'ws'} TokType */
/** @typedef {{ type: TokType, start: number, end: number }} Tok */

/**
 * @param {string} c
 * @returns {boolean}
 */
const isIdentStart = (c) => /[A-Za-z_\u0080-￿]/.test(c);
/** @param {string} c */
const isIdent = (c) => /[A-Za-z0-9_\u0080-￿]/.test(c);
/** @param {string} c */
const isDigit = (c) => c >= '0' && c <= '9';

/**
 * Divide el texto en tokens con su posición (índices UTF-16 del string).
 * Nunca falla: lo que no reconoce lo marca como operador.
 * @param {string} src
 * @returns {Tok[]}
 */
export function tokenize(src) {
  /** @type {Tok[]} */
  const out = [];
  let i = 0;
  const n = src.length;
  // Fin del último nombre o índice que puede continuar una ruta: en
  // "tags.0.name" el '.' pegado es un separador, no el decimal ".0".
  let pathEnd = -1;
  while (i < n) {
    const c = src[i];
    const start = i;
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r') {
      while (i < n && /\s/.test(src[i])) i++;
      out.push({ type: 'ws', start, end: i });
    } else if (c === '-' && src[i + 1] === '-') {
      while (i < n && src[i] !== '\n') i++;
      out.push({ type: 'com', start, end: i });
    } else if (c === '/' && src[i + 1] === '*') {
      const close = src.indexOf('*/', i + 2);
      i = close < 0 ? n : close + 2;
      out.push({ type: 'com', start, end: i });
    } else if (c === "'") {
      i++;
      while (i < n) {
        if (src[i] === "'") {
          if (src[i + 1] === "'") {
            i += 2;
            continue;
          }
          i++;
          break;
        }
        i++;
      }
      out.push({ type: 'str', start, end: i });
    } else if (c === '"' || c === '`') {
      const close = src.indexOf(c, i + 1);
      i = close < 0 ? n : close + 1;
      out.push({ type: 'id', start, end: i });
      pathEnd = i;
    } else if (c === '.' && i === pathEnd) {
      i++;
      out.push({ type: 'op', start, end: i });
      if (isDigit(src[i] ?? '')) {
        const index = i;
        while (i < n && isDigit(src[i])) i++;
        out.push({ type: 'id', start: index, end: i });
        pathEnd = i;
      }
    } else if (isDigit(c) || (c === '.' && isDigit(src[i + 1] ?? ''))) {
      while (i < n && (isDigit(src[i]) || src[i] === '.')) i++;
      if ((src[i] === 'e' || src[i] === 'E') && /[0-9+-]/.test(src[i + 1] ?? '')) {
        i += 2;
        while (i < n && isDigit(src[i])) i++;
      }
      if (i < n && isIdentStart(src[i])) {
        while (i < n && (isIdent(src[i]) || src[i] === '.')) i++;
        out.push({ type: 'dur', start, end: i });
      } else {
        out.push({ type: 'num', start, end: i });
      }
    } else if (isIdentStart(c)) {
      while (i < n && isIdent(src[i])) i++;
      const word = src.slice(start, i).toLowerCase();
      let j = i;
      while (j < n && (src[j] === ' ' || src[j] === '\t')) j++;
      if (src[j] === '(' && !['in', 'and', 'or', 'not'].includes(word)) out.push({ type: 'fn', start, end: i });
      else if (KEYWORDS.has(word)) out.push({ type: 'kw', start, end: i });
      else {
        out.push({ type: 'id', start, end: i });
        pathEnd = i;
      }
    } else {
      i++;
      out.push({ type: 'op', start, end: i });
    }
  }
  return out;
}

/**
 * Convierte un desplazamiento en bytes UTF-8 (como los informa el servidor)
 * en un índice del string de JavaScript.
 * @param {string} text
 * @param {number} byteOffset
 * @returns {number}
 */
export function byteToIndex(text, byteOffset) {
  let bytes = 0;
  let i = 0;
  while (i < text.length && bytes < byteOffset) {
    const cp = /** @type {number} */ (text.codePointAt(i));
    bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    i += cp > 0xffff ? 2 : 1;
  }
  return i;
}

/**
 * Pinta el texto resaltado en `target`. Si hay un error, subraya el token que
 * empieza en (o contiene) `errorIndex`.
 * @param {HTMLElement} target
 * @param {string} text
 * @param {number | null} errorIndex
 */
export function renderHighlighted(target, text, errorIndex) {
  const frag = document.createDocumentFragment();
  let marked = false;
  for (const t of tokenize(text)) {
    const piece = text.slice(t.start, t.end);
    const isErr = errorIndex !== null && !marked && t.type !== 'ws' && errorIndex >= t.start && errorIndex < t.end;
    if (t.type === 'ws' && !isErr) {
      frag.appendChild(document.createTextNode(piece));
      continue;
    }
    const span = document.createElement('span');
    span.className = `tok-${t.type}${isErr ? ' tok-err' : ''}`;
    span.textContent = piece;
    frag.appendChild(span);
    if (isErr) marked = true;
  }
  // Error al final del texto (p. ej. "falta FROM"): marcar el hueco final.
  if (errorIndex !== null && !marked) {
    const span = document.createElement('span');
    span.className = 'tok-err';
    span.textContent = ' ';
    frag.appendChild(span);
  }
  // Un salto final necesita contenido para que el <pre> mida igual que el textarea.
  frag.appendChild(document.createTextNode('\n'));
  target.replaceChildren(frag);
}
