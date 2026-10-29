#!/usr/bin/env python3
"""Pruebas de integración de ZenRift contra el binario real.

Arranca `zenrift serve` en un puerto libre con un directorio de datos temporal
y lo ejerce de punta a punta por HTTP: ingesta (NDJSON, array JSON, chunked,
100-continue), streams, consultas con resultados exactos, backfill, SSE, tail
filtrado, alertas, errores y límites, autenticación, pipelining, reinicio con
recuperación y un log con la cola rota por una caída.

    python3 tests/integration/run_integration.py [ruta/al/zenrift]

Solo usa la biblioteca estándar (Python 3.8 o posterior).
"""

import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / ("zenrift.exe" if os.name == "nt" else "zenrift")


class Fallo(AssertionError):
    """Una comprobación de la prueba no se cumplió."""


def check(cond, msg):
    if not cond:
        raise Fallo(msg)


def wait_for(fn, timeout, what):
    """Reintenta `fn` hasta que devuelva algo verdadero o venza el plazo."""
    deadline = time.time() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.time() > deadline:
            raise Fallo(f"tiempo agotado esperando {what}")
        time.sleep(0.1)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def now_ms():
    return int(time.time() * 1000)


# ---------------------------------------------------------------------------
# Servidor y clientes
# ---------------------------------------------------------------------------


class Server:
    """Un proceso `zenrift serve` con su directorio de datos."""

    def __init__(self, binary, data_dir, *extra):
        self.binary = binary
        self.data_dir = Path(data_dir)
        self.extra = list(extra)
        self.port = None
        self.proc = None
        self.log_path = self.data_dir.with_suffix(".log")
        self.log = None

    def start(self):
        self.port = free_port()
        args = [str(self.binary), "serve", "--port", str(self.port), "--data-dir", str(self.data_dir), *self.extra]
        self.log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise Fallo(f"el servidor terminó al arrancar (código {self.proc.returncode}); ver {self.log_path}")
            try:
                if self.request("GET", "/health", timeout=1)[0] == 200:
                    return self
            except OSError:
                pass
            time.sleep(0.05)
        raise Fallo("el servidor no respondió a /health en 15 s")

    def stop(self):
        """Parada ordenada (SIGTERM): debe terminar sola y con código 0."""
        if self.proc is None:
            return
        if os.name == "nt":
            self.proc.terminate()
        else:
            self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise Fallo("el servidor no se detuvo en 20 s tras SIGTERM")
        finally:
            self.log.close()
            self.proc = None
        if os.name != "nt":
            check(code == 0, f"el servidor salió con código {code}; ver {self.log_path}")

    def kill(self):
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
            self.log.close()
            self.proc = None

    def log_text(self):
        return self.log_path.read_text(encoding="utf-8", errors="replace")

    def request(self, method, path, body=None, headers=None, timeout=10):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            headers = dict(headers or {})
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
                headers.setdefault("Content-Type", "application/json")
            elif isinstance(body, str):
                body = body.encode()
            conn.request(method, path, body=body, headers=headers)
            r = conn.getresponse()
            data = r.read()
            return r.status, {k.lower(): v for k, v in r.getheaders()}, data
        finally:
            conn.close()

    def json(self, method, path, body=None, headers=None, expect=200):
        status, _, data = self.request(method, path, body, headers)
        ok = status in expect if isinstance(expect, (tuple, list, set)) else status == expect
        check(ok, f"{method} {path}: se esperaba {expect} y llegó {status}: {data[:400]!r}")
        return json.loads(data) if data else None


class SSE:
    """Cliente mínimo de Server-Sent Events sobre un socket."""

    def __init__(self, port, path, timeout=10):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.sendall(f"GET {path} HTTP/1.1\r\nHost: zenrift\r\nAccept: text/event-stream\r\n\r\n".encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self._recv()
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        self.status = int(head.split(b" ", 2)[1])
        self.head = head.decode("latin-1")
        self.body = b""  # cuerpo de una respuesta de error

    def _recv(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise Fallo("el servidor cerró el flujo SSE")
        self.buf += chunk

    def next_event(self, timeout):
        """Devuelve (evento, datos) del siguiente evento con datos, o falla."""
        deadline = time.time() + timeout
        while True:
            while b"\n\n" in self.buf:
                block, self.buf = self.buf.split(b"\n\n", 1)
                name, data = "message", []
                for line in block.decode("utf-8").split("\n"):
                    if line.startswith("event:"):
                        name = line[6:].strip()
                    elif line.startswith("data:"):
                        data.append(line[5:].lstrip(" "))
                if data:
                    return name, "\n".join(data)
            left = deadline - time.time()
            if left <= 0:
                raise Fallo("tiempo agotado esperando un evento SSE")
            self.sock.settimeout(left)
            try:
                self._recv()
            except socket.timeout:
                raise Fallo("tiempo agotado esperando un evento SSE")

    def wait_for(self, name, timeout, pred=lambda d: True):
        deadline = time.time() + timeout
        while True:
            ev, data = self.next_event(max(0.05, deadline - time.time()))
            if ev == name:
                parsed = json.loads(data)
                if pred(parsed):
                    return parsed

    def close(self):
        self.sock.close()


def raw_exchange(port, payload, read_responses=1, timeout=10):
    """Envía bytes crudos y lee `read_responses` respuestas con Content-Length."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall(payload)
        buf = b""
        out = []
        while len(out) < read_responses:
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    raise Fallo(f"conexión cerrada tras {len(out)} respuesta(s)")
                buf += chunk
            head, rest = buf.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            status = int(lines[0].split(" ")[1])
            headers = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:])}
            n = int(headers.get("content-length", "0"))
            while len(rest) < n:
                chunk = s.recv(65536)
                if not chunk:
                    raise Fallo("cuerpo incompleto")
                rest += chunk
            out.append((status, headers, rest[:n]))
            buf = rest[n:]
        return out, s
    except BaseException:
        s.close()
        raise


# ---------------------------------------------------------------------------
# Datos de prueba
# ---------------------------------------------------------------------------

# Dos ventanas de 10 s hace 10 minutos: el reloj de eventos avanza por
# inactividad y las ventanas cierran en cuanto dejan de llegar eventos.
T0 = (now_ms() - 600_000) // 10_000 * 10_000


def rfc3339(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms / 1000)) + f".{ms % 1000:03d}Z"


VENTANA_A = [("norte", v) for v in (10, 20, 30, 40, 50)] + [("sur", v) for v in (5, 15, 25)] + [
    ("este", 100),
    ("este", 200),
    ("este", None),  # sin importe: cuenta en count(*) pero no en sum ni count(importe)
]
VENTANA_B = [("norte", 1), ("sur", 2), ("sur", 3)]


def ventas_ndjson(rows, base, rfc_last=False):
    lines = []
    for i, (region, importe) in enumerate(rows):
        ts = base + 100 * i
        ev = {"ts": rfc3339(ts) if (rfc_last and i == len(rows) - 1) else ts, "region": region, "cliente": f"c{i}"}
        if importe is not None:
            ev["importe"] = importe
        lines.append(json.dumps(ev))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Pruebas (se ejecutan en orden y comparten el servidor)
# ---------------------------------------------------------------------------


class Ctx:
    def __init__(self, binary, workdir):
        self.binary = binary
        self.workdir = Path(workdir)
        self.server = Server(binary, self.workdir / "datos")
        self.sse = {}


def t_salud_y_panel(c):
    s = c.server
    body = s.json("GET", "/health")
    check(body["status"] == "ok", "health no dice ok")
    status, hdrs, html = s.request("GET", "/")
    check(status == 200 and b"ZenRift" in html, "el panel no se sirve en /")
    check(hdrs["content-type"].startswith("text/html"), "tipo de contenido del panel")
    status, hdrs2, _ = s.request("GET", "/", headers={"If-None-Match": hdrs["etag"]})
    check(status == 304, f"ETag no produce 304 (llegó {status})")
    status, hdrs3, _ = s.request("GET", "/js/main.js")
    check(status == 200 and "javascript" in hdrs3["content-type"], "main.js no se sirve como JavaScript")
    for bad in ("//etc/passwd", "/../catalog.json", "/js/..%2f..%2fcatalog.json"):
        status, _, _ = s.request("GET", bad)
        check(status in (400, 404), f"ruta peligrosa {bad} devolvió {status}")
    check(s.request("GET", "/no-existe.js")[0] == 404, "archivo inexistente no da 404")


def t_streams(c):
    s = c.server
    created = s.json("POST", "/v1/streams", {"name": "ventas", "partitions": 2}, expect=201)
    check(created["partitions"] == 2, "particiones del stream")
    s.json("POST", "/v1/streams", {"name": "ventas", "partitions": 2}, expect=409)
    s.json("POST", "/v1/streams", {"name": "mal nombre"}, expect=400)
    for bad in (0, 65, 1.5, -1, 1e30):
        s.json("POST", "/v1/streams", {"name": "x", "partitions": bad}, expect=400)
    s.json("POST", "/v1/streams", {"name": "x", "retention": "nunca"}, expect=400)
    info = s.json("GET", "/v1/streams/ventas")
    check(info["name"] == "ventas" and len(info["partition_stats"]) == 2, "detalle del stream")
    s.json("GET", "/v1/streams/no_existe", expect=404)


def t_consultas_y_suscripciones(c):
    s = c.server
    q = s.json(
        "POST",
        "/v1/queries",
        {
            "name": "q_ventas",
            "zql": "SELECT region, count(*) AS n, count(importe) AS con_importe, sum(importe) AS total, "
            "p50(importe) AS mediana, max(importe) AS maximo FROM ventas GROUP BY region WINDOW TUMBLING 10s",
        },
        expect=201,
    )
    check(q["columns"] == ["region", "n", "con_importe", "total", "mediana", "maximo"], f"columnas: {q['columns']}")
    s.json(
        "POST",
        "/v1/queries",
        {"name": "alerta_ventas", "zql": "SELECT region, count(*) AS n FROM ventas GROUP BY region "
         "WINDOW TUMBLING 10s ALERT IF n >= 5 FOR 1 WINDOWS"},
        expect=201,
    )
    # Texto plano con el nombre en la URL.
    status, _, data = s.request(
        "POST", "/v1/queries?name=q_texto", "SELECT count(*) AS n FROM ventas", {"Content-Type": "text/plain"}
    )
    check(status == 201, f"crear consulta en texto plano: {status} {data[:200]!r}")
    s.json("POST", "/v1/queries", {"name": "q_ventas", "zql": "SELECT count(*) FROM ventas"}, expect=409)
    err = s.json("POST", "/v1/queries", {"name": "rota", "zql": "SELECT count( FROM ventas"}, expect=400)
    check({"error", "line", "col", "offset"} <= set(err), f"diagnóstico incompleto: {err}")
    s.json("POST", "/v1/queries", "{no es json", {"Content-Type": "application/json"}, expect=400)

    # Suscripciones abiertas antes de la ingesta.
    c.sse["consulta"] = SSE(s.port, "/v1/queries/q_ventas/stream")
    check(c.sse["consulta"].status == 200, "SSE de consulta")
    where = urllib.parse.quote("importe >= 100 AND region = 'este'")
    c.sse["tail"] = SSE(s.port, f"/v1/streams/ventas/tail?where={where}")
    check(c.sse["tail"].status == 200, "SSE de tail")
    c.sse["live"] = SSE(s.port, "/v1/live")
    bad = SSE(s.port, "/v1/streams/ventas/tail?where=" + urllib.parse.quote("importe >>= 3"))
    check(bad.status == 400, f"filtro inválido en tail devolvió {bad.status}")
    bad.close()


def t_ingesta(c):
    s = c.server
    body_a = ventas_ndjson(VENTANA_A, T0 + 1000)
    body_a += '{"ts": 1, "region": "rota"\n'  # línea mal formada
    r = s.json("POST", "/v1/streams/ventas/events", body_a, {"Content-Type": "application/x-ndjson"})
    check(r["accepted"] == len(VENTANA_A) and r["rejected"] == 1, f"ingesta A: {r}")
    check(r["errors"] and "12" in r["errors"][0], f"el error no indica la línea 12: {r['errors']}")
    # Ventana B como array JSON, con una marca de tiempo RFC 3339.
    rows_b = [json.loads(l) for l in ventas_ndjson(VENTANA_B, T0 + 11000, rfc_last=True).splitlines()]
    r = s.json("POST", "/v1/streams/ventas/events", rows_b)
    check(r["accepted"] == len(VENTANA_B) and r["rejected"] == 0, f"ingesta B: {r}")
    # Todo rechazado -> 400.
    s.json("POST", "/v1/streams/ventas/events", "nada que ver\n", expect=400)


def t_ingesta_chunked_y_continue(c):
    s = c.server
    conn = http.client.HTTPConnection("127.0.0.1", s.port, timeout=10)
    lines = [json.dumps({"ts": now_ms(), "k": i}) + "\n" for i in range(50)]
    conn.request("POST", "/v1/streams/otros/events", body=iter(l.encode() for l in lines), encode_chunked=True,
                 headers={"Content-Type": "application/x-ndjson", "Transfer-Encoding": "chunked"})
    resp = conn.getresponse()
    r = json.loads(resp.read())
    conn.close()
    check(resp.status == 200 and r["accepted"] == 50, f"chunked: {resp.status} {r}")
    check(s.json("GET", "/v1/streams/otros")["partitions"] >= 1, "el stream no se creó solo")

    body = (json.dumps({"ts": now_ms(), "k": "continue"}) + "\n").encode()
    sock = socket.create_connection(("127.0.0.1", s.port), timeout=10)
    try:
        sock.sendall(
            f"POST /v1/streams/otros/events HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n"
            "Expect: 100-continue\r\n\r\n".encode()
        )
        interim = sock.recv(4096)
        check(interim.startswith(b"HTTP/1.1 100"), f"sin 100 Continue: {interim[:60]!r}")
        sock.sendall(body)
        final = b""
        while b"accepted" not in final:
            chunk = sock.recv(4096)
            check(chunk, "conexión cerrada antes de la respuesta final")
            final += chunk
        check(b"200 OK" in final and b'"accepted":1' in final, f"respuesta tras 100-continue: {final[:200]!r}")
    finally:
        sock.close()


def results_by_window(s, name, expected_windows, timeout=20):
    def ready():
        res = s.json("GET", f"/v1/queries/{name}/results?last=1000")["results"]
        return res if len(res) >= expected_windows else None

    res = wait_for(ready, timeout, f"{expected_windows} ventana(s) cerradas de {name}")
    out = {}
    for w in res:
        rows = [dict(zip(w["columns"], row)) for row in w["rows"]]
        out[w["start_ms"]] = {"events": w["events"], "rows": rows, "partial": w["partial"]}
    return out


def by_key(rows, key):
    return {r[key]: r for r in rows}


def check_ventas(windows):
    check(set(windows) == {T0, T0 + 10_000}, f"ventanas inesperadas: {sorted(windows)} (T0 = {T0})")
    a = windows[T0]
    check(a["events"] == len(VENTANA_A) and not a["partial"], f"eventos en A: {a['events']}")
    rows = by_key(a["rows"], "region")
    check(rows["norte"]["n"] == 5 and rows["norte"]["total"] == 150 and rows["norte"]["maximo"] == 50, f"norte A: {rows['norte']}")
    check(abs(rows["norte"]["mediana"] - 30) <= 0.31, f"p50 de norte fuera del 1 %: {rows['norte']['mediana']}")
    check(rows["sur"]["n"] == 3 and rows["sur"]["total"] == 45, f"sur A: {rows['sur']}")
    este = rows["este"]
    check(este["n"] == 3 and este["con_importe"] == 2 and este["total"] == 300, f"este A (con un importe nulo): {este}")
    b = by_key(windows[T0 + 10_000]["rows"], "region")
    check(set(b) == {"norte", "sur"}, f"grupos en B: {set(b)}")
    check(b["norte"]["n"] == 1 and b["sur"]["n"] == 2 and b["sur"]["total"] == 5, f"B: {b}")


def t_resultados_exactos(c):
    check_ventas(results_by_window(c.server, "q_ventas", 2))


def t_sse(c):
    res = c.sse["consulta"].wait_for("result", 10, lambda d: d["query"] == "q_ventas")
    check(res["partial"] is False and res["columns"][0] == "region", f"evento result: {res}")
    events = c.sse["tail"].wait_for("events", 10)
    check(events and all(e["importe"] >= 100 and e["region"] == "este" for e in events), f"tail filtrado: {events}")
    stored = len(VENTANA_A) + len(VENTANA_B)
    metrics = c.sse["live"].wait_for("metrics", 5, lambda d: d["events_stored"] >= stored)
    check(metrics["streams"] >= 1 and metrics["queries"] >= 3, f"métricas: {metrics}")
    for sse in c.sse.values():
        sse.close()
    c.sse.clear()


def t_backfill_y_alertas(c):
    s = c.server
    s.json(
        "POST",
        "/v1/queries",
        {"name": "q_historia", "zql": "SELECT count(*) AS n, sum(importe) AS total FROM ventas WINDOW TUMBLING 10s",
         "from": "beginning"},
        expect=201,
    )
    w = results_by_window(s, "q_historia", 2)
    check(T0 in w and T0 + 10_000 in w, f"faltan las ventanas con datos: {sorted(w)[:5]}")
    check(w[T0]["rows"][0]["n"] == len(VENTANA_A) and w[T0]["rows"][0]["total"] == 495, f"backfill A: {w[T0]}")
    check(w[T0 + 10_000]["rows"][0]["n"] == len(VENTANA_B), f"backfill B: {w[T0 + 10_000]}")
    # Una consulta global emite también las ventanas sin eventos (n = 0), pero
    # como mucho 60 seguidas: un hueco largo no inunda el historial.
    empty = [k for k in w if k not in (T0, T0 + 10_000)]
    check(all(w[k]["rows"][0]["n"] == 0 for k in empty), "ventanas vacías con eventos")
    check(len(empty) <= 60, f"{len(empty)} ventanas vacías seguidas (máximo 60)")

    def transitions():
        alerts = s.json("GET", "/v1/alerts")
        mine = [a for a in alerts["history"] if a["query"] == "alerta_ventas"]
        states = {(a["group"].get("region"), a["state"]) for a in mine}
        return states if {("norte", "firing"), ("norte", "resolved")} <= states else None

    states = wait_for(transitions, 10, "la alerta de norte disparada y resuelta")
    check(("sur", "firing") not in states, f"sur no debía dispararse: {states}")


def t_lectura_del_log(c):
    s = c.server
    total = 0
    for p in (0, 1):
        r = s.json("GET", f"/v1/streams/ventas/events?partition={p}&from=0&limit=1000")
        total += len(r["events"])
        for e in r["events"]:
            check({"offset", "ts", "event"} <= set(e), f"forma del evento leído: {e}")
    check(total == len(VENTANA_A) + len(VENTANA_B), f"eventos en el log: {total}")
    s.json("GET", "/v1/streams/ventas/events?partition=7", expect=404)


def t_check_y_funciones(c):
    s = c.server
    ok = s.json("POST", "/v1/check", "SELECT p99(x) AS p FROM s WINDOW HOPPING 1m EVERY 10s",
                {"Content-Type": "text/plain"})
    check(ok["ok"] and ok["window_ms"] == 60000 and ok["advance_ms"] == 10000, f"check: {ok}")
    bad = s.json("POST", "/v1/check", {"zql": "SELECT FROM"})
    check(not bad["ok"] and bad["error"]["line"] == 1, f"check inválido: {bad}")
    fns = s.json("GET", "/v1/zql/functions")
    check(len(fns["aggregates"]) > 10 and len(fns["functions"]) > 5, "lista de funciones")


def t_errores_http(c):
    s = c.server
    status, _, data = s.request("GET", "/v1/nada")
    check(status == 404 and b"error" in data, f"404 JSON: {status}")
    status, hdrs, _ = s.request("PUT", "/v1/streams")
    check(status == 405 and "GET" in hdrs.get("allow", ""), f"405 con Allow: {status} {hdrs}")
    smuggle = (b"POST /v1/streams/otros/events HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
               b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
    (resp,), sock = raw_exchange(s.port, smuggle)
    sock.close()
    check(resp[0] == 400, f"Content-Length + Transfer-Encoding a la vez devolvió {resp[0]}")


def t_pipelining(c):
    s = c.server
    payload = (b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
               b"GET /v1/streams HTTP/1.1\r\nHost: x\r\n\r\n"
               b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    responses, sock = raw_exchange(s.port, payload, read_responses=3)
    try:
        check([r[0] for r in responses] == [200, 200, 200], f"respuestas en tubería: {[r[0] for r in responses]}")
        check(json.loads(responses[1][2])[0]["name"] in ("otros", "ventas"), "orden de las respuestas en tubería")
        check(responses[2][1].get("connection") == "close", "Connection: close no se respeta")
        sock.settimeout(5)
        check(sock.recv(1) == b"", "el servidor no cerró tras Connection: close")
    finally:
        sock.close()


def t_reinicio(c):
    s = c.server
    before = {st["name"]: st["stored"] for st in s.json("GET", "/v1/streams")}
    s.stop()
    s.start()
    after = {st["name"]: st["stored"] for st in s.json("GET", "/v1/streams")}
    check(after == before, f"streams tras reiniciar: {after} != {before}")
    names = {q["name"] for q in s.json("GET", "/v1/queries")}
    check({"q_ventas", "alerta_ventas", "q_texto", "q_historia"} <= names, f"consultas tras reiniciar: {names}")
    # Las consultas reprocesan el log (horizonte de recuperación de 1 h).
    check_ventas(results_by_window(s, "q_ventas", 2))


def t_cola_rota(c):
    s = c.server
    stored = s.json("GET", "/v1/streams/ventas")["stored"]
    s.kill()  # caída sin parada ordenada
    segs = sorted((c.workdir / "datos" / "streams" / "ventas" / "p0").glob("*.log"))
    check(segs, "no hay segmentos de ventas/p0")
    with open(segs[-1], "ab") as f:
        f.write(b"\x40\x00\x00\x00\xde\xad")  # registro a medio escribir
    s.start()
    check(s.json("GET", "/v1/streams/ventas")["stored"] == stored, "la recuperación perdió o inventó registros")
    check("trunc" in s.log_text().lower(), "el log del servidor no menciona la cola truncada")
    r = s.json("POST", "/v1/streams/ventas/events", json.dumps({"ts": T0 + 15000, "region": "oeste"}) + "\n")
    check(r["accepted"] == 1, "no se puede escribir tras recuperar")
    check(s.json("GET", "/v1/streams/ventas")["stored"] == stored + 1, "conteo tras escribir")


def t_borrado(c):
    s = c.server
    s.json("DELETE", "/v1/streams/ventas", expect=409)
    s.json("DELETE", "/v1/queries/q_texto")
    s.json("DELETE", "/v1/queries/q_texto", expect=404)
    r = s.json("DELETE", "/v1/streams/ventas?cascade=true")
    check(r["queries_deleted"] == 3, f"borrado en cascada: {r}")
    s.json("GET", "/v1/streams/ventas", expect=404)
    check(not (c.workdir / "datos" / "streams" / "ventas").exists(), "los archivos del stream siguen en disco")


def t_auth_y_limites(c):
    s = Server(c.binary, c.workdir / "datos_auth", "--auth-token", "secreto", "--max-body", "4KB").start()
    try:
        status, hdrs, _ = s.request("GET", "/v1/streams")
        check(status == 401 and hdrs.get("www-authenticate") == "Bearer", f"sin token: {status}")
        check(s.request("GET", "/v1/streams", headers={"Authorization": "Bearer otro"})[0] == 401, "token erróneo")
        check(s.request("GET", "/v1/streams", headers={"Authorization": "Bearer secreto"})[0] == 200, "token válido")
        check(s.request("GET", "/v1/streams?token=secreto")[0] == 200, "token en la URL (EventSource)")
        check(s.request("GET", "/health")[0] == 200, "/health no debe pedir token")
        big = (json.dumps({"ts": now_ms(), "relleno": "x" * 8000}) + "\n").encode()
        status, _, _ = s.request("POST", "/v1/streams/s/events", big, {"Authorization": "Bearer secreto"})
        check(status == 413, f"cuerpo de 8 KB con límite de 4 KB devolvió {status}")
    finally:
        s.stop()


def t_panel_desde_disco(c):
    """Con --web-dir el panel se lee del disco: ninguna ruta debe salir de ese directorio."""
    secret = c.workdir / "secreto.txt"
    secret.write_text("no deberías leer esto")
    s = Server(c.binary, c.workdir / "datos_web", "--web-dir", str(ROOT / "web")).start()
    try:
        check(s.request("GET", "/")[0] == 200, "el panel no se sirve desde disco")
        for bad in (f"/{secret}", f"//{secret}".replace("///", "//"), "/../secreto.txt", "/%2e%2e/secreto.txt",
                    "/css/..%2f..%2fsecreto.txt", "/C:/Windows/win.ini", "/css\\..\\x"):
            status, _, data = s.request("GET", bad)
            check(status in (400, 404) and b"deber" not in data, f"{bad} devolvió {status}")
    finally:
        s.stop()


TESTS = [
    t_salud_y_panel,
    t_streams,
    t_consultas_y_suscripciones,
    t_ingesta,
    t_ingesta_chunked_y_continue,
    t_resultados_exactos,
    t_sse,
    t_backfill_y_alertas,
    t_lectura_del_log,
    t_check_y_funciones,
    t_errores_http,
    t_pipelining,
    t_reinicio,
    t_cola_rota,
    t_borrado,
    t_auth_y_limites,
    t_panel_desde_disco,
]


def main():
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    if not binary.exists():
        print(f"no existe el binario {binary}; compílalo antes (cmake --build build)")
        return 2
    workdir = Path(tempfile.mkdtemp(prefix="zenrift-it-"))
    ctx = Ctx(binary, workdir)
    failures = 0
    t_start = time.time()
    try:
        ctx.server.start()
        for test in TESTS:
            name = test.__name__[2:]
            t0 = time.time()
            try:
                test(ctx)
                print(f"  ok     {name:<32} {1000 * (time.time() - t0):8.0f} ms")
            except Fallo as e:
                failures += 1
                print(f"  FALLO  {name:<32} {e}")
            except Exception as e:  # noqa: BLE001 — cualquier excepción es un fallo de la prueba
                failures += 1
                print(f"  FALLO  {name:<32} {type(e).__name__}: {e}")
        ctx.server.stop()
    finally:
        ctx.server.kill()
        for sse in ctx.sse.values():
            sse.close()
    print(f"\n{len(TESTS)} pruebas de integración, {failures} fallidas ({time.time() - t_start:.1f} s)")
    if failures:
        print(f"Registros del servidor en {workdir}")
    else:
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
