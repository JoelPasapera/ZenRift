// Catálogo persistente: streams y consultas definidas. Se guarda como JSON
// en <data>/catalog.json con escritura atómica (archivo temporal + fsync +
// rename), así que una caída nunca deja un catálogo a medias.
#pragma once

#include <string>
#include <vector>

#include "engine/types.hpp"

namespace zr::engine {

struct CatalogData {
  std::vector<StreamConfig> streams;
  std::vector<QueryDef> queries;
};

// Si el archivo no existe devuelve true con el catálogo vacío.
bool load_catalog(const std::string& path, CatalogData& out, std::string& err);
bool save_catalog(const std::string& path, const CatalogData& data, std::string& err);

std::string stream_to_json(const StreamConfig& s);

}  // namespace zr::engine
