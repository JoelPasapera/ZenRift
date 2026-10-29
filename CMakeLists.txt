cmake_minimum_required(VERSION 3.21)
project(ZenRift VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de compilación" FORCE)
endif()

option(ZENRIFT_BUILD_TESTS "Compilar las pruebas" ON)
option(ZENRIFT_BUILD_BENCH "Compilar los benchmarks" ON)
option(ZENRIFT_NATIVE "Optimizar para la CPU de esta máquina (-march=native)" OFF)
set(ZENRIFT_SANITIZE "" CACHE STRING "Sanitizers: address,undefined | thread")

find_package(Threads REQUIRED)

# --- Opciones de compilación comunes ----------------------------------------
add_library(zenrift_options INTERFACE)
target_compile_definitions(zenrift_options INTERFACE ZENRIFT_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(zenrift_options INTERFACE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  target_compile_definitions(zenrift_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(zenrift_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual)
  if(ZENRIFT_NATIVE)
    target_compile_options(zenrift_options INTERFACE -march=native)
  endif()
  if(ZENRIFT_SANITIZE)
    target_compile_options(zenrift_options INTERFACE -fsanitize=${ZENRIFT_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(zenrift_options INTERFACE -fsanitize=${ZENRIFT_SANITIZE})
  endif()
endif()
target_link_libraries(zenrift_options INTERFACE Threads::Threads)
if(WIN32)
  target_link_libraries(zenrift_options INTERFACE ws2_32)
endif()

# --- Panel web embebido en el binario ----------------------------------------
include(cmake/embed.cmake)
zenrift_embed_dir(${CMAKE_CURRENT_SOURCE_DIR}/web ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp)

# --- Biblioteca del motor ------------------------------------------------------
file(GLOB_RECURSE ZENRIFT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER ZENRIFT_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")
add_library(zenrift_lib STATIC ${ZENRIFT_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp)
target_include_directories(zenrift_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(zenrift_lib PUBLIC zenrift_options)

# --- Ejecutable ----------------------------------------------------------------
add_executable(zenrift src/main.cpp)
target_link_libraries(zenrift PRIVATE zenrift_lib)

# --- Pruebas -------------------------------------------------------------------
if(ZENRIFT_BUILD_TESTS)
  enable_testing()
  file(GLOB ZENRIFT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(zenrift_tests ${ZENRIFT_TEST_SOURCES})
  target_link_libraries(zenrift_tests PRIVATE zenrift_lib)
  add_test(NAME unitarias COMMAND zenrift_tests)
  # Pruebas de punta a punta contra el binario real (solo biblioteca estándar de Python).
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME integracion
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_integration.py
                     $<TARGET_FILE:zenrift>)
  endif()
endif()

# --- Benchmarks ----------------------------------------------------------------
if(ZENRIFT_BUILD_BENCH)
  add_executable(zenrift_bench bench/bench_main.cpp)
  target_link_libraries(zenrift_bench PRIVATE zenrift_lib)
endif()
