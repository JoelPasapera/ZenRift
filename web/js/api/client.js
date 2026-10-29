// @ts-check
/**
 * Cliente de la API REST de ZenRift con gestión del token Bearer.
 *
 * Si el servidor responde 401, se pide el token (una sola vez a la vez) y se
 * reintenta la petición. El token se recuerda en localStorage cuando el
 * navegador lo permite.
 */

const TOKEN_KEY = 'zenrift.token';

export class ApiError extends Error {
  /**
   * @param {number} status
   * @param {string} message
   * @param {any} [body]
   */
  constructor(status, message, body) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

/** @type {string} */
let token = '';
try {
  token = localStorage.getItem(TOKEN_KEY) ?? '';
} catch {
  token = '';
}

/** @type {(() => Promise<string | null>) | null} */
let askToken = null;
/** @type {Promise<string | null> | null} */
let pendingAsk = null;

/**
 * Registra cómo pedir el token al usuario (lo pone main.js).
 * @param {() => Promise<string | null>} fn
 */
export function onTokenNeeded(fn) {
  askToken = fn;
}

/** @returns {string} */
export function currentToken() {
  return token;
}

/** @param {string} t */
function saveToken(t) {
  token = t;
  try {
    localStorage.setItem(TOKEN_KEY, t);
  } catch {
    /* almacenamiento no disponible: se usa solo en memoria */
  }
}

/**
 * Añade ?token= a una ruta (para EventSource, que no admite cabeceras).
 * @param {string} path
 * @returns {string}
 */
export function withToken(path) {
  if (!token) return path;
  return `${path}${path.includes('?') ? '&' : '?'}token=${encodeURIComponent(token)}`;
}

/**
 * @param {string} method
 * @param {string} path
 * @param {BodyInit | null} body
 * @param {string | null} contentType
 * @param {boolean} retried
 * @returns {Promise<any>}
 */
async function request(method, path, body, contentType, retried) {
  /** @type {Record<string, string>} */
  const headers = { Accept: 'application/json' };
  if (contentType) headers['Content-Type'] = contentType;
  if (token) headers.Authorization = `Bearer ${token}`;
  let res;
  try {
    res = await fetch(path, { method, headers, body });
  } catch {
    throw new ApiError(0, 'No hay conexión con el servidor');
  }
  if (res.status === 401 && !retried && askToken) {
    pendingAsk ??= askToken().finally(() => {
      pendingAsk = null;
    });
    const t = await pendingAsk;
    if (t) {
      saveToken(t);
      return request(method, path, body, contentType, true);
    }
  }
  const text = await res.text();
  /** @type {any} */
  let data = null;
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      data = text;
    }
  }
  if (!res.ok) {
    const message = data && typeof data === 'object' && typeof data.error === 'string' ? data.error : `Error HTTP ${res.status}`;
    throw new ApiError(res.status, message, data);
  }
  return data;
}

export const api = {
  /** @param {string} path */
  get: (path) => request('GET', path, null, null, false),
  /**
   * @param {string} path
   * @param {any} [payload] objeto (se envía como JSON) o texto
   * @param {string} [contentType]
   */
  post: (path, payload, contentType) => {
    if (typeof payload === 'string') return request('POST', path, payload, contentType ?? 'text/plain', false);
    return request('POST', path, payload === undefined ? null : JSON.stringify(payload), 'application/json', false);
  },
  /** @param {string} path */
  del: (path) => request('DELETE', path, null, null, false),
};
