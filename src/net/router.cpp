#include "net/router.hpp"

#include "core/strings.hpp"

namespace zr::net {

namespace {

std::vector<std::string_view> path_segments(std::string_view path) {
  std::vector<std::string_view> out;
  size_t start = 1;  // la ruta empieza por '/'
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    if (slash == std::string_view::npos) slash = path.size();
    if (slash > start) out.push_back(path.substr(start, slash - start));
    start = slash + 1;
  }
  return out;
}

}  // namespace

void Router::add(std::string_view method, std::string_view pattern, Handler h) {
  Route r;
  r.method = std::string(method);
  for (auto seg : path_segments(pattern)) r.segments.emplace_back(seg);
  r.handler = std::move(h);
  routes_.push_back(std::move(r));
}

Router::Match Router::match(HttpRequest& req, const Handler*& out, std::string& allowed) const {
  const auto segs = path_segments(req.path);
  bool path_matched = false;
  allowed.clear();
  for (const auto& r : routes_) {
    if (r.segments.size() != segs.size()) continue;
    std::vector<std::pair<std::string, std::string>> params;
    bool ok = true;
    for (size_t i = 0; i < segs.size() && ok; ++i) {
      const std::string& p = r.segments[i];
      if (p.size() > 2 && p.front() == '{' && p.back() == '}')
        params.emplace_back(p.substr(1, p.size() - 2), url_decode(segs[i]));
      else
        ok = p == segs[i];
    }
    if (!ok) continue;
    path_matched = true;
    const bool method_ok = r.method == req.method || (req.method == "HEAD" && r.method == "GET");
    if (!method_ok) {
      if (!allowed.empty()) allowed += ", ";
      allowed += r.method;
      continue;
    }
    req.params = std::move(params);
    out = &r.handler;
    return Match::Found;
  }
  return path_matched ? Match::MethodNotAllowed : Match::NotFound;
}

}  // namespace zr::net
