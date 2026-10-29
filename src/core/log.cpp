#include "core/log.hpp"

#include <cstdio>
#include <mutex>

#include "core/time.hpp"

namespace zr::log {

namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mutex;
thread_local std::string t_name = "main";

const char* level_name(Level lv) noexcept {
  switch (lv) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    default: return "?    ";
  }
}

}  // namespace

void set_level(Level lv) noexcept { g_level.store(static_cast<int>(lv), std::memory_order_relaxed); }

Level level() noexcept { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

bool parse_level(std::string_view s, Level& out) noexcept {
  if (iequals(s, "debug")) out = Level::Debug;
  else if (iequals(s, "info")) out = Level::Info;
  else if (iequals(s, "warn") || iequals(s, "warning")) out = Level::Warn;
  else if (iequals(s, "error")) out = Level::Error;
  else if (iequals(s, "off") || iequals(s, "none")) out = Level::Off;
  else return false;
  return true;
}

void set_thread_name(std::string_view name) { t_name.assign(name); }

void write(Level lv, std::string_view msg) {
  std::string line;
  line.reserve(msg.size() + 48);
  append_rfc3339(line, wall_ms());
  line.push_back(' ');
  line.append(level_name(lv));
  line.append(" [");
  line.append(t_name);
  line.append("] ");
  line.append(msg);
  line.push_back('\n');
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

}  // namespace zr::log
