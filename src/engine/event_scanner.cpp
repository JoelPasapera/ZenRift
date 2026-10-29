#include "engine/event_scanner.hpp"

#include <cstring>

#include "core/hash.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "json/lex.hpp"

namespace zr::engine {

using zql::Value;
namespace lex = json::lex;

bool EventScanner::timestamp_of(const Value& v, int64_t& out_ms) noexcept {
  if (v.type == zql::VType::Num) {
    if (!(v.num >= 0) || v.num > 9.2e18) return false;
    out_ms = epoch_number_to_ms(v.num);
    return true;
  }
  if (v.type == zql::VType::Str) {
    if (parse_rfc3339(v.str(), out_ms)) return true;
    double d;
    if (parse_double(v.str(), d) && d >= 0) {
      out_ms = epoch_number_to_ms(d);
      return true;
    }
  }
  return false;
}

bool EventScanner::parse_value(const char*& p, const char* end, int depth, Ctx& c, int32_t leaf, uint64_t path_hash) {
  if (p >= end) return fail(c, p, "fin inesperado del JSON");
  switch (*p) {
    case '{':
    case '[': {
      const char* start = p;
      const bool descend = c.layout->is_prefix(path_hash);
      bool ok;
      if (descend)
        ok = *p == '{' ? parse_object(p, end, depth + 1, c) : parse_array(p, end, depth + 1, c);
      else
        ok = lex::skip_value(p, end, depth + 1) || fail(c, start, "JSON inválido");
      if (!ok) return false;
      if (leaf >= 0) c.slots[leaf] = Value::json(std::string_view(start, static_cast<size_t>(p - start)));
      return true;
    }
    case '"': {
      std::string_view raw;
      bool esc;
      const char* start = p;
      if (!lex::scan_string(p, end, raw, esc)) return fail(c, start, "cadena inválida");
      if (leaf >= 0) {
        if (esc) {
          char* buf = static_cast<char*>(c.arena->alloc(raw.size() + 1, 1));
          const size_t n = lex::unescape_into(raw, buf);
          c.slots[leaf] = Value::string(std::string_view(buf, n));
        } else {
          c.slots[leaf] = Value::string(raw);
        }
      }
      return true;
    }
    case 't':
      if (!lex::match_literal(p, end, "true")) return fail(c, p, "literal inválido");
      if (leaf >= 0) c.slots[leaf] = Value::boolean(true);
      return true;
    case 'f':
      if (!lex::match_literal(p, end, "false")) return fail(c, p, "literal inválido");
      if (leaf >= 0) c.slots[leaf] = Value::boolean(false);
      return true;
    case 'n':
      if (!lex::match_literal(p, end, "null")) return fail(c, p, "literal inválido");
      if (leaf >= 0) c.slots[leaf] = Value::null();
      return true;
    default: {
      double d;
      const char* start = p;
      if (!lex::scan_number(p, end, d)) return fail(c, start, "valor JSON inválido");
      if (leaf >= 0) c.slots[leaf] = Value::number(d);
      return true;
    }
  }
}

bool EventScanner::parse_object(const char*& p, const char* end, int depth, Ctx& c) {
  if (depth > lex::kMaxDepth) return fail(c, p, "anidamiento demasiado profundo");
  ++p;  // '{'
  p = lex::skip_ws(p, end);
  if (p < end && *p == '}') {
    ++p;
    return true;
  }
  const size_t base_len = path_.size();
  while (true) {
    p = lex::skip_ws(p, end);
    if (p >= end || *p != '"') return fail(c, p, "se esperaba una clave entre comillas");
    std::string_view raw_key;
    bool esc;
    const char* kstart = p;
    if (!lex::scan_string(p, end, raw_key, esc)) return fail(c, kstart, "clave inválida");
    if (base_len) path_ += '.';
    if (esc) {
      key_buf_.clear();
      lex::unescape(raw_key, key_buf_);
      path_ += key_buf_;
    } else {
      path_.append(raw_key);
    }
    const uint64_t h = hash_str(path_);
    const int32_t leaf = c.layout->find_leaf(h, path_);
    p = lex::skip_ws(p, end);
    if (p >= end || *p != ':') return fail(c, p, "se esperaba ':'");
    p = lex::skip_ws(p + 1, end);
    if (!parse_value(p, end, depth, c, leaf, h)) return false;
    path_.resize(base_len);
    p = lex::skip_ws(p, end);
    if (p >= end) return fail(c, p, "objeto sin cerrar");
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p == '}') {
      ++p;
      return true;
    }
    return fail(c, p, "se esperaba ',' o '}'");
  }
}

bool EventScanner::parse_array(const char*& p, const char* end, int depth, Ctx& c) {
  if (depth > lex::kMaxDepth) return fail(c, p, "anidamiento demasiado profundo");
  ++p;  // '['
  p = lex::skip_ws(p, end);
  if (p < end && *p == ']') {
    ++p;
    return true;
  }
  const size_t base_len = path_.size();
  for (uint64_t index = 0;; ++index) {
    p = lex::skip_ws(p, end);
    path_ += '.';
    append_uint(path_, index);
    const uint64_t h = hash_str(path_);
    const int32_t leaf = c.layout->find_leaf(h, path_);
    if (!parse_value(p, end, depth, c, leaf, h)) return false;
    path_.resize(base_len);
    p = lex::skip_ws(p, end);
    if (p >= end) return fail(c, p, "array sin cerrar");
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p == ']') {
      ++p;
      return true;
    }
    return fail(c, p, "se esperaba ',' o ']'");
  }
}

void EventScanner::scan_batch(std::string_view body, const FieldLayout& layout, int32_t ts_slot, int64_t ingest_ms,
                              Arena& arena, std::vector<ScannedEvent>& events, std::vector<Value>& slots,
                              ScanReport& report) {
  events.clear();
  slots.clear();
  const size_t L = layout.size();
  const char* const begin = body.data();
  const char* const end = begin + body.size();
  const char* p = lex::skip_ws(begin, end);
  size_t index = 0;

  auto reject = [&](size_t i, const char* at, const char* msg) {
    ++report.rejected;
    if (report.errors.size() < limits_.max_errors_reported)
      report.errors.push_back(cat("evento ", i + 1, " (byte ", static_cast<size_t>(at - begin), "): ", msg));
  };

  // Analiza un objeto evento en p; devuelve false si el JSON está roto.
  auto one = [&](const char*& q) -> bool {
    const size_t i = index++;
    const size_t base = slots.size();
    slots.resize(base + L);
    Ctx c;
    c.layout = &layout;
    c.slots = slots.data() + base;
    c.arena = &arena;
    path_.clear();
    const char* start = q;
    if (!parse_object(q, end, 0, c)) {
      slots.resize(base);
      reject(i, c.err_at ? c.err_at : q, c.err_msg ? c.err_msg : "JSON inválido");
      return false;
    }
    std::string_view raw(start, static_cast<size_t>(q - start));
    if (raw.size() > limits_.max_event_bytes) {
      slots.resize(base);
      reject(i, start, "el evento supera el tamaño máximo");
      return true;
    }
    ScannedEvent ev;
    ev.ts = ingest_ms;
    ev.ts_defaulted = true;
    if (ts_slot >= 0) {
      const Value& tv = c.slots[ts_slot];
      if (!tv.is_null()) {
        int64_t ts;
        if (!timestamp_of(tv, ts)) {
          slots.resize(base);
          reject(i, start, "marca de tiempo inválida (usa milisegundos de época o RFC 3339)");
          return true;
        }
        ev.ts = ts;
        ev.ts_defaulted = false;
      }
    }
    if (ev.ts > ingest_ms + limits_.max_future_ms) {
      slots.resize(base);
      reject(i, start, "marca de tiempo demasiado en el futuro");
      return true;
    }
    // El log y las salidas NDJSON necesitan una línea por evento: los saltos
    // de línea fuera de cadenas son espacio en blanco y se cambian por espacios.
    if (std::memchr(raw.data(), '\n', raw.size()) || std::memchr(raw.data(), '\r', raw.size())) {
      char* copy = static_cast<char*>(arena.alloc(raw.size(), 1));
      for (size_t k = 0; k < raw.size(); ++k) copy[k] = (raw[k] == '\n' || raw[k] == '\r') ? ' ' : raw[k];
      raw = std::string_view(copy, raw.size());
    }
    ev.raw = raw;
    events.push_back(ev);
    ++report.accepted;
    return true;
  };

  if (p < end && *p == '[') {
    // Array JSON de objetos.
    ++p;
    p = lex::skip_ws(p, end);
    if (p < end && *p == ']') return;
    while (p < end) {
      p = lex::skip_ws(p, end);
      if (p < end && *p == '{') {
        if (!one(p)) return;  // dentro de un array no hay forma segura de resincronizar
      } else {
        const char* at = p;
        const size_t i = index++;
        if (!lex::skip_value(p, end)) {
          reject(i, at, "JSON inválido");
          return;
        }
        reject(i, at, "cada elemento del array debe ser un objeto");
      }
      p = lex::skip_ws(p, end);
      if (p < end && *p == ',') {
        ++p;
        continue;
      }
      if (p < end && *p == ']') {
        ++p;
        p = lex::skip_ws(p, end);
        if (p != end) reject(index, p, "contenido inesperado tras el array");
        return;
      }
      reject(index, p, "se esperaba ',' o ']' en el array");
      return;
    }
    reject(index, p, "array sin cerrar");
    return;
  }

  // NDJSON (o un único objeto): objetos separados por espacio en blanco. Ante
  // una línea rota se descarta hasta el siguiente salto de línea y se sigue.
  while (true) {
    p = lex::skip_ws(p, end);
    if (p >= end) return;
    if (*p == '{') {
      if (one(p)) continue;
    } else {
      reject(index++, p, "se esperaba un objeto JSON");
    }
    const void* nl = std::memchr(p, '\n', static_cast<size_t>(end - p));
    if (!nl) return;
    p = static_cast<const char*>(nl) + 1;
  }
}

bool EventScanner::scan_stored(std::string_view raw, const FieldLayout& layout, Arena& arena, Value* slots) {
  for (size_t i = 0; i < layout.size(); ++i) slots[i] = Value::null();
  Ctx c;
  c.layout = &layout;
  c.slots = slots;
  c.arena = &arena;
  path_.clear();
  const char* p = raw.data();
  const char* end = p + raw.size();
  p = lex::skip_ws(p, end);
  if (p >= end || *p != '{') return false;
  return parse_object(p, end, 0, c);
}

}  // namespace zr::engine
