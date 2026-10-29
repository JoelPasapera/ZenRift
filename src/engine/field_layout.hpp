// Disposición de campos de una partición: qué rutas JSON necesita extraer el
// escáner y en qué ranura deja cada una.
//
// Solo se materializan los campos que usa alguna consulta o tail de la
// partición (proyección empujada hasta el parser). El escáner consulta, por
// hash de la ruta completa, si una clave es una hoja buscada, y por hash de
// prefijo si merece la pena descender en un objeto o array; si no, lo salta
// sin analizar su contenido.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/flat_map.hpp"

namespace zr::engine {

class FieldLayout {
 public:
  // Construye la disposición; la ranura i corresponde a paths[i].
  void build(std::vector<std::string> paths);

  size_t size() const noexcept { return paths_.size(); }
  const std::vector<std::string>& paths() const noexcept { return paths_; }

  // Ranura de una ruta, o -1.
  int32_t slot_of(std::string_view path) const noexcept;
  // Para el escáner: búsqueda por hash precalculado de la ruta completa.
  int32_t find_leaf(uint64_t hash, std::string_view path) const noexcept;
  bool is_prefix(uint64_t hash) const noexcept { return prefixes_.contains(hash); }

 private:
  std::vector<std::string> paths_;
  FlatMap<uint64_t, int32_t> leaves_;
  FlatMap<uint64_t, uint8_t> prefixes_;
};

}  // namespace zr::engine
