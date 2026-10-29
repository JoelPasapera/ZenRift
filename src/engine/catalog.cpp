#include "engine/catalog.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#include "core/strings.hpp"
#include "json/value.hpp"
#include "json/writer.hpp"
#include "storage/file.hpp"

namespace fs = std::filesystem;

namespace zr::engine {

bool valid_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '-') return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

namespace {

void write_stream(json::Writer& w, const StreamConfig& s) {
  w.begin_object()
      .kv("name", s.name)
      .kv_int("partitions", s.partitions)
      .kv("ts_field", s.ts_field)
      .kv_int("retention_bytes", static_cast<int64_t>(s.retention_bytes))
      .kv_int("retention_ms", s.retention_ms)
      .kv_int("created_ms", s.created_ms)
      .end_object();
}

// Entero de un campo numérico del catálogo, saturado al rango de int64: el
// archivo puede estar editado a mano, y convertir a entero un double fuera de
// rango es comportamiento indefinido.
int64_t int_field(const json::Value& v, std::string_view key, int64_t def) {
  const double d = v.get_number(key, static_cast<double>(def));
  if (!std::isfinite(d)) return def;
  if (d <= -9.2e18) return std::numeric_limits<int64_t>::min();
  if (d >= 9.2e18) return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(d);
}

}  // namespace

std::string stream_to_json(const StreamConfig& s) {
  std::string out;
  json::Writer w(out);
  write_stream(w, s);
  return out;
}

bool load_catalog(const std::string& path, CatalogData& out, std::string& err) {
  out = CatalogData();
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;
  storage::File f;
  if (!f.open(path, err)) return false;
  uint64_t size = 0;
  if (!f.size(size, err)) return false;
  std::string text(size, '\0');
  size_t got = 0;
  if (!f.read_at(text.data(), text.size(), 0, got, err)) return false;
  text.resize(got);
  json::Value doc;
  json::ParseError perr;
  if (!json::parse(text, doc, &perr)) {
    err = cat(path, ": JSON inválido en el byte ", perr.offset, ": ", perr.message);
    return false;
  }
  if (const json::Value* streams = doc.get("streams"); streams && streams->is_array()) {
    for (const auto& s : streams->items()) {
      StreamConfig c;
      c.name = std::string(s.get_string("name"));
      const int64_t partitions = int_field(s, "partitions", 1);
      c.ts_field = std::string(s.get_string("ts_field", "ts"));
      c.retention_bytes = static_cast<uint64_t>(std::max<int64_t>(0, int_field(s, "retention_bytes", 0)));
      c.retention_ms = std::max<int64_t>(0, int_field(s, "retention_ms", 0));
      c.created_ms = int_field(s, "created_ms", 0);
      if (!valid_name(c.name) || partitions < 1 || partitions > 64) {
        err = cat(path, ": stream inválido '", c.name, "'");
        return false;
      }
      c.partitions = static_cast<uint32_t>(partitions);
      out.streams.push_back(std::move(c));
    }
  }
  if (const json::Value* queries = doc.get("queries"); queries && queries->is_array()) {
    for (const auto& q : queries->items()) {
      QueryDef d;
      d.name = std::string(q.get_string("name"));
      d.zql = std::string(q.get_string("zql"));
      d.from = std::string(q.get_string("from", "now"));
      d.start_ms = int_field(q, "start_ms", 0);
      d.created_ms = int_field(q, "created_ms", 0);
      if (const json::Value* offs = q.get("start_offsets"); offs && offs->is_array())
        for (const auto& o : offs->items())
          d.start_offsets.push_back(o.is_number() && o.as_number() >= 0 && o.as_number() < 9.0e18
                                        ? static_cast<int64_t>(o.as_number())
                                        : -1);
      if (!valid_name(d.name) || d.zql.empty()) {
        err = cat(path, ": consulta inválida '", d.name, "'");
        return false;
      }
      out.queries.push_back(std::move(d));
    }
  }
  return true;
}

bool save_catalog(const std::string& path, const CatalogData& data, std::string& err) {
  std::string out;
  json::Writer w(out);
  w.begin_object().kv_int("version", 1).key("streams").begin_array();
  for (const auto& s : data.streams) write_stream(w, s);
  w.end_array().key("queries").begin_array();
  for (const auto& q : data.queries) {
    w.begin_object()
        .kv("name", q.name)
        .kv("zql", q.zql)
        .kv("from", q.from)
        .kv_int("start_ms", q.start_ms)
        .kv_int("created_ms", q.created_ms);
    if (!q.start_offsets.empty()) {
      w.key("start_offsets").begin_array();
      for (int64_t o : q.start_offsets) w.integer(o);
      w.end_array();
    }
    w.end_object();
  }
  w.end_array().end_object();
  out.push_back('\n');

  const std::string tmp = path + ".tmp";
  std::error_code ec;
  fs::remove(tmp, ec);
  {
    storage::File f;
    if (!f.open(tmp, err) || !f.append(out.data(), out.size(), err) || !f.sync(err)) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    err = cat("no se pudo reemplazar ", path, ": ", ec.message());
    return false;
  }
  return true;
}

}  // namespace zr::engine
