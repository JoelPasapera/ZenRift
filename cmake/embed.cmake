# Incrusta los archivos del panel web (web/) en el binario como arrays de bytes,
# para que ZenRift sea un único ejecutable sin archivos externos.
#
# Uso desde CMakeLists.txt:
#   include(cmake/embed.cmake)
#   zenrift_embed_dir(<dir_web> <archivo_cpp_generado>)
#
# El mismo archivo se ejecuta en modo script (cmake -P) durante la compilación,
# así que editar algo en web/ regenera el .cpp automáticamente.

if(CMAKE_SCRIPT_MODE_FILE)
  # ---- Modo script: genera el .cpp ------------------------------------------
  file(GLOB_RECURSE files RELATIVE "${SRC_DIR}" "${SRC_DIR}/*")
  list(SORT files)
  set(out "// Archivo generado por cmake/embed.cmake. No editar.\n")
  string(APPEND out "#include \"api/assets.hpp\"\n\nnamespace zr::assets {\n\nnamespace {\n\n")
  set(table "")
  set(index 0)
  foreach(rel IN LISTS files)
    get_filename_component(ext "${rel}" LAST_EXT)
    string(TOLOWER "${ext}" ext)
    if(ext STREQUAL ".html")
      set(mime "text/html; charset=utf-8")
    elseif(ext STREQUAL ".css")
      set(mime "text/css; charset=utf-8")
    elseif(ext STREQUAL ".js" OR ext STREQUAL ".mjs")
      set(mime "text/javascript; charset=utf-8")
    elseif(ext STREQUAL ".svg")
      set(mime "image/svg+xml")
    elseif(ext STREQUAL ".json")
      set(mime "application/json")
    elseif(ext STREQUAL ".png")
      set(mime "image/png")
    elseif(ext STREQUAL ".ico")
      set(mime "image/x-icon")
    elseif(ext STREQUAL ".woff2")
      set(mime "font/woff2")
    else()
      continue()
    endif()
    file(READ "${SRC_DIR}/${rel}" hex HEX)
    string(LENGTH "${hex}" hexlen)
    math(EXPR size "${hexlen} / 2")
    string(APPEND out "const unsigned char f${index}[] = {\n")
    set(pos 0)
    while(pos LESS hexlen)
      string(SUBSTRING "${hex}" ${pos} 64 chunk)
      string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," chunk "${chunk}")
      string(APPEND out "  ${chunk}\n")
      math(EXPR pos "${pos} + 64")
    endwhile()
    string(APPEND out "  0x00};\n\n")
    string(APPEND table "    {\"/${rel}\", \"${mime}\", f${index}, ${size}},\n")
    math(EXPR index "${index} + 1")
  endforeach()
  string(APPEND out "}  // namespace\n\nconst Asset kAssets[] = {\n${table}    {nullptr, nullptr, nullptr, 0},\n};\n\n")
  string(APPEND out "const size_t kAssetCount = ${index};\n\n}  // namespace zr::assets\n")
  file(WRITE "${OUT_FILE}.tmp" "${out}")
  file(COPY_FILE "${OUT_FILE}.tmp" "${OUT_FILE}" ONLY_IF_DIFFERENT)
  file(REMOVE "${OUT_FILE}.tmp")
  return()
endif()

function(zenrift_embed_dir src_dir out_file)
  file(GLOB_RECURSE web_files CONFIGURE_DEPENDS "${src_dir}/*")
  add_custom_command(
    OUTPUT ${out_file}
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${src_dir} -DOUT_FILE=${out_file} -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
    DEPENDS ${web_files} ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
    COMMENT "Incrustando el panel web"
    VERBATIM)
endfunction()
