#include "api/api.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "api/assets.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "json/value.hpp"
#include "json/writer.hpp"
#include "zql/functions.hpp"
#include "zql/plan.hpp"

namespace fs = std::filesystem;

namespace zr::api {

using net::ConnHandle;
using net::HttpRequest;
using net::HttpResponse;
using net::HttpServer;

namespace {

HttpResponse json_ok(std::string body, int status = 200) { return HttpResponse::json(status, std::move(body)); }

std::string zql_error_json(const zql::Diagnostic& d) {
  std::string out;
  json::Writer(out)
      .begin_object()
      .kv("error", d.message)
      .kv_int("line", d.pos.line)
      .kv_int("col", d.pos.col)
      .kv_int("offset", static_cast<int64_t>(d.pos.offset))
      .end_object();
  return out;
}

const char* mime_for(std::string_view path) {
  auto ends = [&](std::string_view ext) { return path.size() >= ext.size() && path.ends_with(ext); };
  if (ends(".html")) return "text/html; charset=utf-8";
  if (ends(".css")) return "text/css; charset=utf-8";
  if (ends(".js") || ends(".mjs")) return "text/javascript; charset=utf-8";
  if (ends(".svg")) return "image/svg+xml";
  if (ends(".json")) return "application/json";
  if (ends(".png")) return "image/png";
  if (ends(".ico")) return "image/x-icon";
  if (ends(".woff2")) return "font/woff2";
  return "application/octet-stream";
}

// Descripción de agregaciones para la ayuda del editor.
struct AggHelp {
  const char* name;
  const char* help;
};
constexpr AggHelp kAggHelp[] = {
    {"count", "count(*) o count(x): eventos o valores no nulos (exacto)"},
    {"sum", "sum(x): suma exacta con compensación de Neumaier"},
    {"avg", "avg(x): media; avg(cond) da la proporción en que cond es cierta"},
    {"min", "min(x)"},
    {"max", "max(x)"},
    {"stddev", "stddev(x): desviación típica muestral (Welford)"},
    {"variance", "variance(x): varianza muestral"},
    {"p50", "p50(x), p90, p95, p99, p999...: cuantiles con DDSketch (error relativo <= 1 %)"},
    {"quantile", "quantile(x, 0.99) o percentile(x, 99)"},
    {"median", "median(x): p50"},
    {"distinct", "distinct(x) o count(DISTINCT x): distintos con HyperLogLog (~0,8 %)"},
    {"topk", "topk(x, k): los k valores más frecuentes (Space-Saving)"},
    {"first", "first(x): primer valor por tiempo de evento"},
    {"last", "last(x): último valor por tiempo de evento"},
    {"rate", "rate() eventos por segundo; rate(x) suma de x por segundo"},
    {"window_start", "window_start(): inicio de la ventana (ms)"},
    {"window_end", "window_end(): fin de la ventana (ms)"},
    {"window_seconds", "window_seconds(): duración de la ventana en segundos"},
};

}  // namespace

Api::Api(HttpServer& server, ApiOptions opts) : server_(server), opts_(std::move(opts)) {}

void Api::attach(engine::Engine& engine) {
  engine_ = &engine;
  install_routes();
  server_.set_handler([this](HttpServer& s, const ConnHandle& h, HttpRequest& req) { handle(s, h, req); });
  server_.set_close_hook([this](uint64_t conn) { on_close(conn); });
}

// ---------------------------------------------------------------------------
// EngineListener: de cualquier hilo al hilo del servidor
// ---------------------------------------------------------------------------

void Api::reply(const engine::ReplyTo& to, engine::Response r) {
  server_.post([this, to, r = std::move(r)]() mutable {
    HttpResponse resp;
    resp.status = r.status;
    resp.content_type = std::move(r.content_type);
    resp.body = std::move(r.body);
    resp.headers.emplace_back("Cache-Control", "no-store");
    server_.respond(ConnHandle{to.conn, to.request}, std::move(resp));
  });
}

void Api::publish(engine::Publication p) {
  server_.post([this, p = std::move(p)]() { fan_out(p); });
}

void Api::fan_out(const engine::Publication& p) {
  auto send_all = [&](std::vector<uint64_t>& subs) {
    subs.erase(std::remove_if(subs.begin(), subs.end(),
                              [&](uint64_t conn) { return !server_.sse_send(conn, p.event, *p.data); }),
               subs.end());
  };
  switch (p.topic) {
    case engine::Topic::QueryResult:
    case engine::Topic::QueryPartial: {
      auto it = query_subs_.find(p.key);
      if (it != query_subs_.end()) send_all(it->second);
      break;
    }
    case engine::Topic::Alert:
    case engine::Topic::Metrics:
    case engine::Topic::Catalog: send_all(live_subs_); break;
    case engine::Topic::Tail: {
      uint64_t tail = 0;
      for (char c : p.key) tail = tail * 10 + static_cast<uint64_t>(c - '0');
      auto it = conn_of_tail_.find(tail);
      if (it != conn_of_tail_.end()) server_.sse_send(it->second, p.event, *p.data);
      break;
    }
  }
}

void Api::on_close(uint64_t conn) {
  // Se puede suscribir a una consulta que aún no existe: al cerrar se borran
  // las entradas vacías para que nombres arbitrarios no se acumulen.
  for (auto it = query_subs_.begin(); it != query_subs_.end();) {
    auto& subs = it->second;
    subs.erase(std::remove(subs.begin(), subs.end(), conn), subs.end());
    it = subs.empty() ? query_subs_.erase(it) : std::next(it);
  }
  live_subs_.erase(std::remove(live_subs_.begin(), live_subs_.end(), conn), live_subs_.end());
  auto it = tail_of_conn_.find(conn);
  if (it != tail_of_conn_.end()) {
    engine_->remove_tail(it->second);
    conn_of_tail_.erase(it->second);
    tail_of_conn_.erase(it);
  }
}

// ---------------------------------------------------------------------------
// Despacho
// ---------------------------------------------------------------------------

bool Api::authorized(const HttpRequest& req) const {
  if (opts_.auth_token.empty()) return true;
  const std::string_view auth = req.header("Authorization");
  if (auth.size() > 7 && iequals(auth.substr(0, 7), "Bearer ") && auth.substr(7) == opts_.auth_token) return true;
  // EventSource no puede poner cabeceras: se admite ?token= también.
  return req.query_param("token") == opts_.auth_token;
}

void Api::handle(HttpServer& s, const ConnHandle& h, HttpRequest& req) {
  if (req.method == "OPTIONS") {
    HttpResponse r;
    r.status = 204;
    r.headers = {{"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
                 {"Access-Control-Allow-Headers", "Authorization, Content-Type"},
                 {"Access-Control-Max-Age", "86400"}};
    s.respond(h, std::move(r));
    return;
  }
  const bool api = req.path.starts_with("/v1/");
  if (api && !authorized(req)) {
    HttpResponse r = HttpResponse::error(401, "falta el token o no es válido (Authorization: Bearer <token>)");
    r.headers.emplace_back("WWW-Authenticate", "Bearer");
    s.respond(h, std::move(r));
    return;
  }
  const net::Router::Handler* handler = nullptr;
  std::string allowed;
  switch (router_.match(req, handler, allowed)) {
    case net::Router::Match::Found: (*handler)(s, h, req); return;
    case net::Router::Match::MethodNotAllowed: {
      HttpResponse r = HttpResponse::error(405, cat("método no permitido; usa ", allowed));
      r.headers.emplace_back("Allow", allowed);
      s.respond(h, std::move(r));
      return;
    }
    case net::Router::Match::NotFound:
      if (!api && (req.method == "GET" || req.method == "HEAD")) {
        serve_static(s, h, req);
        return;
      }
      s.respond(h, HttpResponse::error(404, cat("no existe la ruta ", req.path)));
      return;
  }
}

void Api::serve_static(HttpServer& s, const ConnHandle& h, HttpRequest& req) {
  std::string path(req.path);
  if (path == "/" || path.empty()) path = "/index.html";
  // Solo rutas relativas simples: sin "..", sin barras dobles (fs::path con
  // una parte absoluta descarta web_dir: "//etc/passwd" sería "/etc/passwd"),
  // sin barras invertidas ni ':' (unidades de Windows) y sin bytes nulos.
  if (path.front() != '/' || path.find("..") != std::string::npos || path.find("//") != std::string::npos ||
      path.find_first_of(std::string_view("\\:\0", 3)) != std::string::npos) {
    s.respond(h, HttpResponse::error(400, "ruta inválida"));
    return;
  }
  std::string body;
  const char* type = mime_for(path);
  if (!opts_.web_dir.empty()) {
    std::ifstream f(fs::path(opts_.web_dir) / path.substr(1), std::ios::binary);
    if (!f) {
      s.respond(h, HttpResponse::error(404, cat("no existe ", path)));
      return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    body = ss.str();
  } else {
    const assets::Asset* a = assets::find(path);
    if (!a) {
      s.respond(h, HttpResponse::error(404, cat("no existe ", path)));
      return;
    }
    body.assign(reinterpret_cast<const char*>(a->data), a->size);
    type = a->content_type;
  }
  std::string etag = "\"";
  const uint64_t hv = hash_bytes(body.data(), body.size());
  static constexpr char hex[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) etag.push_back(hex[(hv >> (i * 4)) & 15]);
  etag.push_back('"');
  HttpResponse r;
  r.headers = {{"ETag", etag}, {"Cache-Control", "no-cache"}};
  if (req.header("If-None-Match") == etag) {
    r.status = 304;
    s.respond(h, std::move(r));
    return;
  }
  r.content_type = type;
  r.body = std::move(body);
  s.respond(h, std::move(r));
}

// ---------------------------------------------------------------------------
// Rutas
// ---------------------------------------------------------------------------

void Api::install_routes() {
  auto& R = router_;
  engine::Engine& E = *engine_;

  R.add("GET", "/health", [](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    std::string out;
    json::Writer(out).begin_object().kv("status", "ok").kv("version", ZENRIFT_VERSION).end_object();
    s.respond(h, json_ok(std::move(out)));
  });

  R.add("GET", "/v1/stats", [&E](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    std::string out;
    json::Writer(out)
        .begin_object()
        .key("engine")
        .raw(E.stats_json())
        .key("server")
        .begin_object()
        .kv_int("connections", static_cast<int64_t>(s.connections()))
        .kv("poller", s.poller_name())
        .kv_int("port", s.bound_port())
        .end_object()
        .end_object();
    s.respond(h, json_ok(std::move(out)));
  });

  R.add("GET", "/v1/metrics", [&E](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    s.defer(h);
    E.metrics_history(reply_to(h));
  });

  R.add("GET", "/v1/live", [this](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    if (s.start_sse(h)) live_subs_.push_back(h.id);
  });

  // ------------------------------------------------------------ streams
  R.add("GET", "/v1/streams", [&E](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    s.respond(h, json_ok(E.streams_json()));
  });

  R.add("POST", "/v1/streams", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    json::Value body;
    json::ParseError perr;
    if (!json::parse(req.body, body, &perr) || !body.is_object()) {
      s.respond(h, HttpResponse::error(400, "se esperaba un objeto JSON {\"name\": ..., \"partitions\": ...}"));
      return;
    }
    // Convertir a entero un double fuera de rango es comportamiento indefinido:
    // se comprueba el rango antes de cada conversión.
    auto whole = [](double v, double lo, double hi) { return std::isfinite(v) && v == std::floor(v) && v >= lo && v <= hi; };
    engine::StreamConfig cfg;
    cfg.name = std::string(body.get_string("name"));
    const double partitions = body.get_number("partitions", E.config().default_partitions);
    if (!whole(partitions, 1, 64)) {
      s.respond(h, HttpResponse::error(400, "partitions debe ser un entero entre 1 y 64"));
      return;
    }
    cfg.partitions = static_cast<uint32_t>(partitions);
    cfg.ts_field = std::string(body.get_string("ts_field", "ts"));
    if (const json::Value* rb = body.get("retention_bytes")) {
      uint64_t n = 0;
      if (rb->is_number() && whole(rb->as_number(), 0, 9.0e18))
        cfg.retention_bytes = static_cast<uint64_t>(rb->as_number());
      else if (rb->is_number() || !parse_size(rb->as_string(), n)) {
        s.respond(h, HttpResponse::error(400, "retention_bytes inválido (p. ej. \"512MB\" o un entero de bytes)"));
        return;
      } else {
        cfg.retention_bytes = n;
      }
    }
    if (const json::Value* rt = body.get("retention")) {
      int64_t ms = 0;
      if (rt->is_number() && whole(rt->as_number(), 0, 9.0e18))
        cfg.retention_ms = static_cast<int64_t>(rt->as_number());
      else if (rt->is_number() || !parse_duration(rt->as_string(), ms)) {
        s.respond(h, HttpResponse::error(400, "retention inválido (p. ej. \"7d\" o milisegundos)"));
        return;
      } else {
        cfg.retention_ms = ms;
      }
    }
    if (cfg.ts_field.empty()) cfg.ts_field = "ts";
    s.defer(h);
    E.create_stream(std::move(cfg), reply_to(h));
  });

  R.add("GET", "/v1/streams/{name}", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    bool found = false;
    std::string out = E.stream_json(std::string(req.param("name")), found);
    if (!found) {
      s.respond(h, HttpResponse::error(404, cat("no existe el stream '", req.param("name"), "'")));
      return;
    }
    s.respond(h, json_ok(std::move(out)));
  });

  R.add("DELETE", "/v1/streams/{name}", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    const std::string cascade = req.query_param("cascade");
    s.defer(h);
    E.delete_stream(std::string(req.param("name")), cascade == "true" || cascade == "1", reply_to(h));
  });

  R.add("POST", "/v1/streams/{name}/events", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    const std::string name(req.param("name"));
    if (!engine::valid_name(name)) {
      s.respond(h, HttpResponse::error(400, "nombre de stream inválido"));
      return;
    }
    s.defer(h);
    E.ingest(name, std::string(req.body), reply_to(h));
  });

  R.add("GET", "/v1/streams/{name}/events", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    int64_t partition = 0, from = 0, limit = 100;
    parse_int64(req.query_param("partition", "0"), partition);
    parse_int64(req.query_param("from", "0"), from);
    parse_int64(req.query_param("limit", "100"), limit);
    s.defer(h);
    if (!E.read_events(std::string(req.param("name")), static_cast<uint32_t>(std::clamp<int64_t>(partition, 0, 65535)), from,
                       static_cast<uint32_t>(std::clamp<int64_t>(limit, 1, 10000)), reply_to(h)))
      s.respond(h, HttpResponse::error(404, "no existe el stream o la partición"));
  });

  R.add("GET", "/v1/streams/{name}/tail", [this, &E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    const std::string name(req.param("name"));
    std::shared_ptr<const zql::FilterPlan> filter;
    const std::string where = req.query_param("where");
    if (!where.empty()) {
      auto fp = std::make_shared<zql::FilterPlan>();
      zql::Diagnostic d;
      if (!zql::compile_filter(where, *fp, d)) {
        s.respond(h, HttpResponse::json(400, zql_error_json(d)));
        return;
      }
      filter = std::move(fp);
    }
    const uint64_t tail = next_tail_++;
    if (!E.add_tail(tail, name, filter)) {
      s.respond(h, HttpResponse::error(404, cat("no existe el stream '", name, "'")));
      return;
    }
    if (!s.start_sse(h)) {
      E.remove_tail(tail);
      return;
    }
    tail_of_conn_[h.id] = tail;
    conn_of_tail_[tail] = h.id;
  });

  // ------------------------------------------------------------ consultas
  R.add("GET", "/v1/queries", [&E](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    s.defer(h);
    E.list_queries(reply_to(h));
  });

  R.add("POST", "/v1/queries", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    std::string name, zql_text, from = "now";
    const std::string_view ct = req.header("Content-Type");
    if (ct.starts_with("text/plain") || ct.starts_with("application/sql")) {
      name = req.query_param("name");
      zql_text = std::string(req.body);
      from = req.query_param("from", "now");
    } else {
      json::Value body;
      if (!json::parse(req.body, body) || !body.is_object()) {
        s.respond(h, HttpResponse::error(400, "se esperaba {\"name\": ..., \"zql\": ..., \"from\": \"now\"}"));
        return;
      }
      name = std::string(body.get_string("name"));
      zql_text = std::string(body.get_string("zql"));
      from = std::string(body.get_string("from", "now"));
    }
    if (zql_text.empty()) {
      s.respond(h, HttpResponse::error(400, "falta la consulta ZQL"));
      return;
    }
    s.defer(h);
    E.create_query(std::move(name), std::move(zql_text), std::move(from), reply_to(h));
  });

  R.add("POST", "/v1/check", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    std::string text(req.body);
    if (!req.header("Content-Type").starts_with("text/plain")) {
      json::Value body;
      if (json::parse(req.body, body) && body.is_object()) text = std::string(body.get_string("zql"));
    }
    zql::QueryPlan plan;
    zql::Diagnostic d;
    if (!zql::compile_query(text, E.config().compile, plan, d)) {
      std::string out;
      json::Writer(out)
          .begin_object()
          .kv_bool("ok", false)
          .key("error")
          .raw(zql_error_json(d))
          .end_object();
      s.respond(h, json_ok(std::move(out)));
      return;
    }
    std::string out;
    json::Writer w(out);
    w.begin_object().kv_bool("ok", true).kv("stream", plan.stream).kv("explain", plan.explain(false));
    w.key("columns").begin_array();
    for (const auto& c : plan.columns) w.str(c.name);
    w.end_array().key("keys").begin_array();
    for (const auto& k : plan.key_names) w.str(k);
    w.end_array();
    w.kv_int("window_ms", plan.window_ms).kv_int("advance_ms", plan.advance_ms).kv_int("emit_every_ms", plan.emit_every_ms);
    w.kv_bool("has_alert", plan.has_alert).end_object();
    s.respond(h, json_ok(std::move(out)));
  });

  R.add("GET", "/v1/zql/functions", [](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    std::string out;
    json::Writer w(out);
    w.begin_object().key("aggregates").begin_array();
    for (const auto& a : kAggHelp) w.begin_object().kv("name", a.name).kv("help", a.help).end_object();
    w.end_array().key("functions").begin_array();
    for (size_t i = 0; i < zql::function_count(); ++i) {
      const auto& f = zql::function_at(static_cast<uint32_t>(i));
      w.begin_object().kv("name", f.name).kv("help", f.help).end_object();
    }
    w.end_array().end_object();
    s.respond(h, json_ok(std::move(out)));
  });

  R.add("GET", "/v1/queries/{name}", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    s.defer(h);
    E.get_query(std::string(req.param("name")), reply_to(h));
  });

  R.add("DELETE", "/v1/queries/{name}", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    s.defer(h);
    E.delete_query(std::string(req.param("name")), reply_to(h));
  });

  R.add("GET", "/v1/queries/{name}/results", [&E](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    int64_t last = 60;
    parse_int64(req.query_param("last", "60"), last);
    s.defer(h);
    E.get_results(std::string(req.param("name")), static_cast<uint32_t>(std::clamp<int64_t>(last, 1, 100000)),
                  reply_to(h));
  });

  R.add("GET", "/v1/queries/{name}/stream", [this](HttpServer& s, const ConnHandle& h, HttpRequest& req) {
    if (s.start_sse(h)) query_subs_[std::string(req.param("name"))].push_back(h.id);
  });

  R.add("GET", "/v1/alerts", [&E](HttpServer& s, const ConnHandle& h, HttpRequest&) {
    s.defer(h);
    E.list_alerts(reply_to(h));
  });
}

}  // namespace zr::api
