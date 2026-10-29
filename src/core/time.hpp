// Utilidades de tiempo: relojes, RFC 3339 y duraciones ("10s", "1h30m").
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zr {

// Milisegundos desde la época Unix (reloj de pared).
int64_t wall_ms() noexcept;
// Reloj monótono, para medir intervalos.
int64_t mono_ms() noexcept;
int64_t mono_us() noexcept;

// Días desde 1970-01-01 para una fecha civil proléptica gregoriana.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) noexcept;
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) noexcept;

// "2026-10-29T13:45:00.123Z", con fracción opcional y desfase ±HH:MM.
// También acepta separador espacio y solo fecha ("2026-10-29" = medianoche UTC).
bool parse_rfc3339(std::string_view s, int64_t& out_ms) noexcept;
// Escribe "YYYY-MM-DDTHH:MM:SS.mmmZ" al final de out.
void append_rfc3339(std::string& out, int64_t ms);
std::string format_rfc3339(int64_t ms);

// Duraciones: secuencias número+unidad (ms, s, m, h, d, w), p. ej. "1h30m", "1.5s".
bool parse_duration(std::string_view s, int64_t& out_ms) noexcept;
std::string format_duration(int64_t ms);

// División entera con redondeo hacia -infinito (para alinear ventanas).
constexpr int64_t floor_div(int64_t a, int64_t b) noexcept {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

constexpr int64_t align_down(int64_t t, int64_t step) noexcept { return floor_div(t, step) * step; }

// Convierte un número de época a milisegundos adivinando la unidad por su
// magnitud: segundos, milisegundos, microsegundos o nanosegundos.
int64_t epoch_number_to_ms(double v) noexcept;

}  // namespace zr
