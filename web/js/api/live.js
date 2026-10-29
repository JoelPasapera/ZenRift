// @ts-check
/**
 * Suscripción a un flujo de Server-Sent Events del servidor.
 *
 * EventSource ya reintenta solo tras un corte (el servidor indica retry: 2 s);
 * aquí solo se traduce cada evento JSON a su manejador y se informa del estado.
 */

import { withToken } from './client.js';

/** @typedef {'connecting' | 'live' | 'error'} LiveState */

export class LiveStream {
  /**
   * @param {string} path ruta del flujo (p. ej. /v1/live)
   * @param {Record<string, (data: any) => void>} handlers manejador por nombre de evento
   * @param {(state: LiveState) => void} [onState]
   */
  constructor(path, handlers, onState) {
    this.path = path;
    this.handlers = handlers;
    this.onState = onState ?? (() => {});
    /** @type {EventSource | null} */
    this.source = null;
    this.open();
  }

  open() {
    this.close();
    this.onState('connecting');
    const src = new EventSource(withToken(this.path));
    this.source = src;
    src.onopen = () => this.onState('live');
    src.onerror = () => this.onState(src.readyState === EventSource.CLOSED ? 'error' : 'connecting');
    for (const [event, fn] of Object.entries(this.handlers)) {
      src.addEventListener(event, (e) => {
        const msg = /** @type {MessageEvent<string>} */ (e);
        let data;
        try {
          data = JSON.parse(msg.data);
        } catch {
          return;
        }
        fn(data);
      });
    }
  }

  close() {
    if (this.source) {
      this.source.close();
      this.source = null;
    }
  }
}
