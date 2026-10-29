#include "core/time.hpp"

#include <chrono>
#include <cmath>

namespace zr {

int64_t wall_ms() noexcept {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t mono_ms() noexcept {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t mono_us() noexcept {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Algoritmos de Howard Hinnant ("chrono-Compatible Low-Level Date Algorithms").
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) noexcept {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yy + (m <= 2);
}

namespace {

bool digits(std::string_view s, size_t pos, size_t n, int64_t& out) noexcept {
  if (pos + n > s.size()) return false;
  int64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

bool leap(int64_t y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned days_in_month(int64_t y, unsigned m) noexcept {
  static constexpr unsigned k[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : k[m - 1];
}

void put2(std::string& out, unsigned v) {
  out.push_back(static_cast<char>('0' + v / 10));
  out.push_back(static_cast<char>('0' + v % 10));
}

}  // namespace

bool parse_rfc3339(std::string_view s, int64_t& out_ms) noexcept {
  int64_t y, mo, d;
  if (!digits(s, 0, 4, y) || s.size() < 10 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' ||
      !digits(s, 8, 2, d))
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > static_cast<int64_t>(days_in_month(y, static_cast<unsigned>(mo))))
    return false;
  int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  if (s.size() == 10) {
    out_ms = days * 86400000;
    return true;
  }
  const char sep = s[10];
  if (sep != 'T' && sep != 't' && sep != ' ') return false;
  int64_t hh, mi, ss;
  if (!digits(s, 11, 2, hh) || s.size() < 19 || s[13] != ':' || !digits(s, 14, 2, mi) || s[16] != ':' ||
      !digits(s, 17, 2, ss))
    return false;
  if (hh > 23 || mi > 59 || ss > 60) return false;  // 60: segundo intercalar
  size_t pos = 19;
  int64_t frac_ms = 0;
  if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
    ++pos;
    int scale = 100, count = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (scale > 0) {
        frac_ms += (s[pos] - '0') * scale;
        scale /= 10;
      }
      ++pos;
      ++count;
    }
    if (count == 0) return false;
  }
  int64_t offset_min = 0;
  if (pos < s.size()) {
    const char z = s[pos];
    if (z == 'Z' || z == 'z') {
      ++pos;
    } else if (z == '+' || z == '-') {
      int64_t oh, om;
      if (!digits(s, pos + 1, 2, oh)) return false;
      size_t mpos = pos + 3;
      if (mpos < s.size() && s[mpos] == ':') ++mpos;
      if (!digits(s, mpos, 2, om)) return false;
      if (oh > 23 || om > 59) return false;
      offset_min = (oh * 60 + om) * (z == '+' ? 1 : -1);
      pos = mpos + 2;
    } else {
      return false;
    }
  }
  if (pos != s.size()) return false;
  const int64_t secs = days * 86400 + hh * 3600 + mi * 60 + ss - offset_min * 60;
  out_ms = secs * 1000 + frac_ms;
  return true;
}

void append_rfc3339(std::string& out, int64_t ms) {
  const int64_t days = floor_div(ms, 86400000);
  int64_t rem = ms - days * 86400000;
  int64_t y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  const unsigned hh = static_cast<unsigned>(rem / 3600000);
  rem %= 3600000;
  const unsigned mi = static_cast<unsigned>(rem / 60000);
  rem %= 60000;
  const unsigned ss = static_cast<unsigned>(rem / 1000);
  const unsigned mss = static_cast<unsigned>(rem % 1000);
  if (y < 0 || y > 9999) {
    out += "0000-01-01T00:00:00.000Z";
    return;
  }
  const unsigned yy = static_cast<unsigned>(y);
  out.push_back(static_cast<char>('0' + yy / 1000));
  out.push_back(static_cast<char>('0' + (yy / 100) % 10));
  out.push_back(static_cast<char>('0' + (yy / 10) % 10));
  out.push_back(static_cast<char>('0' + yy % 10));
  out.push_back('-');
  put2(out, m);
  out.push_back('-');
  put2(out, d);
  out.push_back('T');
  put2(out, hh);
  out.push_back(':');
  put2(out, mi);
  out.push_back(':');
  put2(out, ss);
  out.push_back('.');
  out.push_back(static_cast<char>('0' + mss / 100));
  out.push_back(static_cast<char>('0' + (mss / 10) % 10));
  out.push_back(static_cast<char>('0' + mss % 10));
  out.push_back('Z');
}

std::string format_rfc3339(int64_t ms) {
  std::string s;
  s.reserve(24);
  append_rfc3339(s, ms);
  return s;
}

bool parse_duration(std::string_view s, int64_t& out_ms) noexcept {
  if (s.empty()) return false;
  double total = 0;
  size_t i = 0;
  while (i < s.size()) {
    // Parte numérica (entera y decimal opcional).
    const size_t start = i;
    double v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    if (i < s.size() && s[i] == '.') {
      ++i;
      double scale = 0.1;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v += (s[i++] - '0') * scale;
        scale /= 10;
      }
    }
    if (i == start) return false;
    // Unidad.
    const size_t ustart = i;
    while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) ++i;
    std::string_view unit = s.substr(ustart, i - ustart);
    double mult;
    if (unit == "ms") mult = 1;
    else if (unit == "s" || unit == "sec" || unit == "secs") mult = 1000;
    else if (unit == "m" || unit == "min" || unit == "mins") mult = 60000;
    else if (unit == "h" || unit == "hr" || unit == "hrs") mult = 3600000;
    else if (unit == "d") mult = 86400000;
    else if (unit == "w") mult = 604800000;
    else return false;
    total += v * mult;
  }
  if (!(total >= 0) || total > 9.2e18) return false;
  out_ms = static_cast<int64_t>(std::llround(total));
  return true;
}

std::string format_duration(int64_t ms) {
  if (ms == 0) return "0s";
  std::string out;
  if (ms < 0) {
    out.push_back('-');
    ms = -ms;
  }
  struct Unit {
    int64_t ms;
    const char* name;
  };
  static constexpr Unit units[] = {{86400000, "d"}, {3600000, "h"}, {60000, "m"}, {1000, "s"}, {1, "ms"}};
  for (const auto& u : units) {
    if (ms >= u.ms) {
      out += std::to_string(ms / u.ms);
      out += u.name;
      ms %= u.ms;
    }
  }
  return out;
}

int64_t epoch_number_to_ms(double v) noexcept {
  const double a = std::fabs(v);
  double ms;
  if (a < 1e11) ms = v * 1000.0;        // segundos
  else if (a < 1e14) ms = v;            // milisegundos
  else if (a < 1e17) ms = v / 1000.0;   // microsegundos
  else ms = v / 1e6;                    // nanosegundos
  return static_cast<int64_t>(std::floor(ms));
}

}  // namespace zr
