// @ts-check
/**
 * Consultas de ejemplo para el editor, pensadas para el stream "requests"
 * que crea el modo demo (y para cualquier log de peticiones HTTP).
 */

/** @type {{ title: string, name: string, zql: string }[]} */
export const EXAMPLES = [
  {
    title: 'Errores por servicio',
    name: 'errores_por_servicio',
    zql: `SELECT service,
       count(*)                 AS peticiones,
       avg(status >= 500) * 100 AS error_pct
FROM requests
GROUP BY service
WINDOW TUMBLING 10s
ORDER BY error_pct DESC
EMIT EVERY 1s`,
  },
  {
    title: 'Latencia por ruta',
    name: 'latencia_por_ruta',
    zql: `SELECT route,
       p50(latency_ms) AS p50,
       p95(latency_ms) AS p95,
       p99(latency_ms) AS p99,
       count(*)        AS n
FROM requests
WHERE status < 500
GROUP BY route
WINDOW HOPPING 1m EVERY 10s
HAVING n >= 10
ORDER BY p99 DESC
LIMIT 8`,
  },
  {
    title: 'Usuarios únicos',
    name: 'usuarios_unicos',
    zql: `SELECT distinct(user)                AS usuarios,
       topk(client.platform, 3)      AS plataformas,
       topk(region, 2)               AS regiones
FROM requests
WINDOW TUMBLING 1m
EMIT EVERY 2s`,
  },
  {
    title: 'Histograma de latencia',
    name: 'histograma_latencia',
    zql: `SELECT bucket(latency_ms, 100) AS desde_ms,
       count(*)                 AS n
FROM requests
WHERE latency_ms < 2s
GROUP BY bucket(latency_ms, 100)
WINDOW TUMBLING 30s
ORDER BY desde_ms`,
  },
  {
    title: 'Alerta de checkout lento',
    name: 'alerta_checkout_lento',
    zql: `SELECT p99(latency_ms) AS p99, count(*) AS n
FROM requests
WHERE service = 'checkout' AND route LIKE '/checkout%'
WINDOW TUMBLING 10s
ALERT IF p99 > 1500 AND n >= 5 FOR 2 WINDOWS`,
  },
];
