// @ts-check
/**
 * Publicación/suscripción mínima con tipos de evento por nombre.
 * @template {Record<string, any>} Events
 */
export class Emitter {
  constructor() {
    /** @type {Map<keyof Events, Set<(payload: any) => void>>} */
    this.handlers = new Map();
  }

  /**
   * Suscribe y devuelve la función para darse de baja.
   * @template {keyof Events} K
   * @param {K} type
   * @param {(payload: Events[K]) => void} fn
   * @returns {() => void}
   */
  on(type, fn) {
    let set = this.handlers.get(type);
    if (!set) {
      set = new Set();
      this.handlers.set(type, set);
    }
    set.add(fn);
    return () => set?.delete(fn);
  }

  /**
   * @template {keyof Events} K
   * @param {K} type
   * @param {Events[K]} payload
   */
  emit(type, payload) {
    const set = this.handlers.get(type);
    if (!set) return;
    for (const fn of [...set]) {
      try {
        fn(payload);
      } catch (err) {
        console.error(`Error en el manejador de "${String(type)}"`, err);
      }
    }
  }
}
