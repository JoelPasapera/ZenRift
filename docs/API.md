# API HTTP

Todo lo que hace el panel lo hace a través de esta API, así que cualquier
cosa que veas en él se puede automatizar. Las respuestas son JSON; los flujos
en vivo son [Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html)
(`text/event-stream`), que se consumen con `EventSource` en el navegador o con
`curl -N`.

Base por defecto: `http://127.0.0.1:7070`.

## Convenciones

**Autenticación.** Si el servidor arranca con `auth_token`, todo lo que está
bajo `/v1/` exige `Authorization: Bearer <token>`. Como `EventSource` no puede
poner cabeceras, también se acepta `?token=<token>` en la URL. `/health` y el
panel son públicos (el panel pide el token la primera vez que lo necesita).

**Errores.** Cuerpo `{"error": "mensaje"}` con el código adecuado:

| Código | Cuándo |
|---|---|
| 400 | JSON o parámetros inválidos, ZQL que no compila (con `line`, `col` y `offset`), todos los eventos de un lote rechazados |
| 401 | falta el token o no es válido |
| 404 | stream, consulta o ruta inexistentes |
| 405 | método no admitido (cabecera `Allow` con los válidos) |
| 409 | el stream o la consulta ya existen; borrar un stream con consultas sin `cascade` |
| 413 | cuerpo mayor que `max_body` |
| 431 | cabeceras demasiado grandes |
| 503 | la partición no está abierta (arranque o borrado en curso) |
| 507 | no se pudo escribir en disco |

**HTTP.** HTTP/1.1 con keep-alive y peticiones en tubería (pipelining),
cuerpos con `Content-Length` o `Transfer-Encoding: chunked`, y
`Expect: 100-continue`. Una petición con `Content-Length` y
`Transfer-Encoding` a la vez se rechaza (evita el contrabando de peticiones).
CORS se activa con `cors_origin`.

---

## Ingesta

### `POST /v1/streams/{stream}/events`

Acepta, detectándolo por el contenido:

- **NDJSON**: un objeto por línea (`application/x-ndjson`). Las líneas rotas se
  rechazan una a una; el resto del lote entra.
- **Array JSON**: `[{...}, {...}]`.
- **Un objeto** suelto.

Si el stream no existe y `auto_create_streams` está activo (por defecto), se
crea con `partitions` particiones. Cada lote va entero a una partición; los
lotes se reparten entre particiones por turnos.

```bash
curl -s localhost:7070/v1/streams/requests/events --data-binary @- <<'EOF'
{"ts": 1761760000123, "service": "checkout", "status": 200, "latency_ms": 84.2}
{"ts": "2026-10-29T13:45:00Z", "service": "payments", "status": 503, "latency_ms": 1290}
EOF
```

```json
{"stream":"requests","partition":0,"accepted":2,"rejected":0,"first_offset":1840,"last_offset":1841,"errors":[]}
```

`errors` trae los primeros rechazos con su posición (`"evento 12 (byte 808):
objeto sin cerrar"`, contando desde 1). La respuesta llega cuando el lote está
en el log y entregado al sistema operativo; con `fsync = always`, también
sincronizado a disco.

Marca de tiempo: el campo `ts_field` del stream (`ts` por defecto), número de
época (s, ms, µs o ns; se detecta por magnitud) o texto RFC 3339. Si falta, se
usa la hora de llegada. Eventos con una marca más de `max_future` (1 h) en el
futuro se rechazan.

---

## Streams

### `GET /v1/streams`

```json
[{
  "name": "requests", "partitions": 2, "ts_field": "ts", "created_ms": 1793304200000,
  "events": 360750, "stored": 703800, "bytes_in": 81234567, "rejected": 0,
  "disk_bytes": 168211456, "last_event_ms": 1793304912345,
  "partition_stats": [{"partition": 0, "shard": 0, "start_offset": 0, "end_offset": 351900, "segments": 3, "disk_bytes": 84105728}],
  "queries": ["trafico_por_servicio", "alerta_errores"]
}]
```

`events` cuenta lo recibido desde que arrancó el proceso; `stored`, lo que
conserva el log tras la retención.

### `POST /v1/streams`

```json
{"name": "pedidos", "partitions": 4, "ts_field": "created_at", "retention": "3d", "retention_bytes": "2GB"}
```

`partitions` entre 1 y 64. `retention` y `retention_bytes` (por partición)
aceptan texto (`"3d"`, `"512MB"`) o números (ms y bytes); 0 es sin límite.
Responde `201` con la configuración.

### `GET /v1/streams/{stream}`

El mismo objeto que en la lista.

### `DELETE /v1/streams/{stream}[?cascade=true]`

Borra el stream y sus archivos. Si tiene consultas responde `409`, salvo con
`cascade=true`, que las borra también:
`{"deleted": "pedidos", "queries_deleted": 2}`.

### `GET /v1/streams/{stream}/events?partition=0&from=0&limit=100`

Lee el log de una partición desde un offset (máximo 10 000 por llamada):

```json
{"stream": "requests", "partition": 0, "from": 0, "next": 100, "start": 0, "end": 351900,
 "events": [{"offset": 0, "ts": 1793304200123, "event": {"service": "checkout", "...": "..."}}]}
```

Para recorrerlo entero, repite con `from = next` hasta que `next == end`.

### `GET /v1/streams/{stream}/tail[?where=<expresión ZQL>]` (SSE)

Los eventos que llegan, en vivo, filtrados en el servidor con una expresión
ZQL (la misma VM que usan las consultas):

```bash
curl -N "localhost:7070/v1/streams/requests/tail?where=status%20%3E%3D%20500"
```

Eventos SSE:

- `events`: array JSON con los eventos de un lote que pasan el filtro.
- `dropped`: `{"dropped": n}` cuando se superan `tail_rate` eventos/s (500):
  el tail limita su ritmo para no saturar al cliente; la ingesta no se frena.

Un filtro inválido responde `400` con el diagnóstico antes de abrir el flujo.

---

## Consultas

### `POST /v1/queries`

```json
{"name": "errores_por_servicio",
 "zql": "SELECT service, avg(status >= 500) * 100 AS error_pct FROM requests GROUP BY service WINDOW TUMBLING 10s",
 "from": "now"}
```

También en texto plano: `Content-Type: text/plain`, la consulta en el cuerpo y
`?name=...&from=...` en la URL.

`from`: `now` (por defecto), una duración (`1h`), `beginning` o una fecha
RFC 3339. Ver [ZQL.md](ZQL.md#desde-cuándo-cuenta-una-consulta).

Responde `201` con la consulta (como `GET /v1/queries/{name}`). Si no compila,
`400`:

```json
{"error": "se esperaba ')' tras '*' y se encontró 'FROM'", "line": 1, "col": 16, "offset": 15}
```

### `POST /v1/check`

Compila sin crear (es lo que usa el editor mientras escribes). Cuerpo en texto
plano o `{"zql": "..."}`. Siempre responde `200`:

```json
{"ok": true, "stream": "requests", "columns": ["service", "error_pct"], "keys": ["service"],
 "window_ms": 10000, "advance_ms": 10000, "emit_every_ms": 0, "has_alert": false,
 "explain": "Consulta continua sobre el stream 'requests'\n  Agrupa por: ..."}
```

```json
{"ok": false, "error": {"error": "...", "line": 2, "col": 7, "offset": 31}}
```

### `GET /v1/queries` y `GET /v1/queries/{name}`

```json
{
  "name": "errores_por_servicio", "stream": "requests", "zql": "SELECT ...", "from": "now",
  "start_ms": 1793304200000, "created_ms": 1793304200000,
  "columns": ["service", "error_pct"], "keys": ["service"],
  "window": {"type": "tumbling", "size_ms": 10000, "advance_ms": 10000, "lateness_ms": 2000},
  "emit_every_ms": 0, "alert": null,
  "stats": {"events": 452550, "matched": 452550, "late": 0, "dropped": 0, "lag": 0,
            "partitions": 2, "partitions_live": 2, "windows": 241, "partials": 0,
            "last_window_end_ms": 1793304910000, "alerts_firing": 0}
}
```

El detalle (`GET /v1/queries/{name}`) añade `explain`, `latest` (último
resultado final) y `latest_partial`. En `stats`: `events` vistos, `matched`
que pasan el `WHERE`, `late` descartados por tardíos, `dropped` por el límite
de grupos, `lag` registros pendientes mientras se pone al día releyendo el log
y `partitions_live` cuántas particiones ya van en vivo.

### `GET /v1/queries/{name}/results?last=60`

Los últimos resultados finales (se guardan `history`, 360, por consulta) y el
parcial en curso:

```json
{"results": [{
   "query": "errores_por_servicio", "partial": false,
   "start": "2026-10-29T15:09:20.000Z", "end": "2026-10-29T15:09:30.000Z",
   "start_ms": 1793304560000, "end_ms": 1793304570000,
   "events": 15006, "groups": 6, "truncated": false,
   "columns": ["service", "error_pct"],
   "rows": [["auth", 0.58], ["catalog", 0.44]]
 }],
 "partial": null}
```

### `GET /v1/queries/{name}/stream` (SSE)

Eventos `result` (cada ventana cerrada) y `partial` (con `EMIT EVERY`), con el
mismo formato que un elemento de `results`. Se puede abrir antes de crear la
consulta.

### `DELETE /v1/queries/{name}`

`{"deleted": "errores_por_servicio"}`.

### `GET /v1/zql/functions`

`{"aggregates": [{"name", "help"}], "functions": [{"name", "help"}]}`.

---

## Alertas

### `GET /v1/alerts`

```json
{"active": [{
   "id": "alerta_errores:8c1d0f3a9e2b4c71", "query": "alerta_errores", "state": "firing",
   "condition": "error_pct > 5 AND peticiones > 30",
   "group": {"service": "payments"}, "values": {"error_pct": 24.64, "peticiones": 694},
   "at": "2026-10-29T15:09:40.000Z", "at_ms": 1793304580000,
   "since": "2026-10-29T15:09:30.000Z", "since_ms": 1793304570000}],
 "history": [ ...transiciones firing/resolved, la más reciente primero... ]}
```

`group` son las claves del grupo (vacío en consultas globales) y `values`, las
columnas del resultado que la disparó.

---

## Sistema

### `GET /health`

`{"status": "ok", "version": "2.0.0"}`. Sin autenticación.

### `GET /v1/stats`

```json
{"engine": {"version": "2.0.0", "started_ms": 1793304200000, "uptime_ms": 712345, "shards": 1, "streams": 1,
            "events_total": 360750, "events_stored": 703800, "disk_bytes": 168211456,
            "data_dir": "./data", "fsync": "interval", "queues": {"coordinator": 0, "shards": [0]}},
 "server": {"connections": 3, "poller": "epoll", "port": 7070}}
```

`queues` es la profundidad de las colas de mensajes: si crece sin parar, el
motor no da abasto.

### `GET /v1/metrics`

Las últimas 300 muestras (una por segundo):

```json
[{"t": 1793304912000, "events_per_sec": 1500, "bytes_per_sec": 336000, "late_per_sec": 0,
  "windows_per_sec": 0.6, "events_total": 360750, "events_stored": 703800,
  "streams": 1, "queries": 6, "alerts_active": 0, "stream_events": {"requests": 360750}}]
```

### `GET /v1/live` (SSE)

El flujo del panel:

- `metrics`: una muestra como las de `/v1/metrics`, cada segundo.
- `alert`: cada transición de alerta (mismo objeto que en `/v1/alerts`).
- `catalog`: `{"t": ...}` cuando se crean o borran streams o consultas.
