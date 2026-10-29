#include "storage/partition_log.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>

#include "core/crc32c.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"

namespace fs = std::filesystem;

namespace zr::storage {

namespace {

constexpr char kMagic[8] = {'Z', 'R', 'L', 'O', 'G', 'v', '1', '\n'};
constexpr uint64_t kFileHeader = sizeof(kMagic);
constexpr uint64_t kRecordHeader = 16;  // len (4) + crc (4) + ts (8)

inline void put_u32(char* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void put_i64(char* p, int64_t v) { std::memcpy(p, &v, 8); }
inline uint32_t get_u32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline int64_t get_i64(const char* p) {
  int64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

std::string segment_name(int64_t base) {
  std::string digits = std::to_string(base);
  return std::string(20 - std::min<size_t>(20, digits.size()), '0') + digits + ".log";
}

bool parse_segment_name(const std::string& name, int64_t& base) {
  if (name.size() != 24 || name.compare(20, 4, ".log") != 0) return false;
  return parse_int64(std::string_view(name).substr(0, 20), base) && base >= 0;
}

// Lector secuencial de registros con búfer, que tolera registros que cruzan
// el límite del bloque leído.
class Scanner {
 public:
  enum class Status { Ok, End, Corrupt, IoError };

  Scanner(File& f, uint64_t pos, uint64_t end, uint32_t max_record, std::string& buf)
      : file_(f), pos_(pos), end_(end), max_record_(max_record), buf_(buf) {
    buf_.clear();
  }

  Status next(int64_t& ts, std::string_view& payload, std::string& why) {
    if (pos_ >= end_) return Status::End;
    if (end_ - pos_ < kRecordHeader) {
      why = "cabecera de registro truncada";
      return Status::Corrupt;
    }
    if (!ensure(kRecordHeader, why)) return Status::IoError;
    const char* h = buf_.data() + (pos_ - buf_pos_);
    const uint32_t len = get_u32(h);
    const uint32_t crc = get_u32(h + 4);
    if (len > max_record_ || pos_ + kRecordHeader + len > end_) {
      why = len > max_record_ ? "longitud de registro imposible" : "registro truncado";
      return Status::Corrupt;
    }
    if (!ensure(kRecordHeader + len, why)) return Status::IoError;
    h = buf_.data() + (pos_ - buf_pos_);
    const uint32_t actual = crc32c(h + 8, 8 + len);
    if (actual != crc) {
      why = "CRC no coincide";
      return Status::Corrupt;
    }
    ts = get_i64(h + 8);
    payload = std::string_view(h + kRecordHeader, len);
    pos_ += kRecordHeader + len;
    return Status::Ok;
  }

  uint64_t pos() const noexcept { return pos_; }

 private:
  // Garantiza que [pos_, pos_ + n) está en el búfer.
  bool ensure(uint64_t n, std::string& why) {
    if (pos_ >= buf_pos_ && pos_ + n <= buf_pos_ + buf_.size()) return true;
    const uint64_t want = std::max<uint64_t>(n, 1u << 20);
    const uint64_t avail = end_ - pos_;
    const size_t to_read = static_cast<size_t>(std::min(want, avail));
    buf_.resize(to_read);
    size_t got = 0;
    if (!file_.read_at(buf_.data(), to_read, pos_, got, why)) return false;
    buf_.resize(got);
    buf_pos_ = pos_;
    if (got < n) {
      why = "lectura incompleta";
      return false;
    }
    return true;
  }

  File& file_;
  uint64_t pos_;
  uint64_t end_;
  uint32_t max_record_;
  std::string& buf_;
  uint64_t buf_pos_ = 0;
};

}  // namespace

struct PartitionLog::Segment {
  struct IndexEntry {
    uint32_t rel;  // offset relativo a base
    uint64_t pos;  // posición en el archivo
  };
  int64_t base = 0;
  int64_t count = 0;
  uint64_t size = 0;  // tamaño lógico (incluye lo que aún está en el búfer)
  int64_t min_ts = std::numeric_limits<int64_t>::max();
  int64_t max_ts = std::numeric_limits<int64_t>::min();
  std::string path;
  File file;
  std::vector<IndexEntry> index;
  uint64_t last_index_pos = 0;

  int64_t end() const noexcept { return base + count; }
};

PartitionLog::PartitionLog() = default;
PartitionLog::~PartitionLog() { close(); }

bool PartitionLog::open(const std::string& dir, const LogOptions& opts, std::string& err) {
  close();
  dir_ = dir;
  opts_ = opts;
  recovery_ = RecoveryReport();
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) {
    err = cat("no se pudo crear ", dir_, ": ", ec.message());
    return false;
  }
  std::vector<int64_t> bases;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    int64_t base;
    if (entry.is_regular_file() && parse_segment_name(entry.path().filename().string(), base)) bases.push_back(base);
  }
  if (ec) {
    err = cat("no se pudo listar ", dir_, ": ", ec.message());
    return false;
  }
  std::sort(bases.begin(), bases.end());
  for (int64_t base : bases)
    if (!open_segment(base, err)) return false;
  if (segments_.empty() && !open_segment(0, err)) return false;
  next_offset_ = segments_.back()->end();
  recovery_.segments = segments_.size();
  for (size_t i = 1; i < segments_.size(); ++i) {
    if (segments_[i]->base < segments_[i - 1]->end())
      recovery_.warnings.push_back(cat("segmentos solapados en ", dir_, ": ", segments_[i - 1]->path, " y ",
                                       segments_[i]->path));
  }
  return true;
}

void PartitionLog::close() {
  std::string err;
  if (!segments_.empty()) flush(err);
  segments_.clear();
  wbuf_.clear();
  next_offset_ = 0;
}

bool PartitionLog::open_segment(int64_t base, std::string& err) {
  auto seg = std::make_unique<Segment>();
  seg->base = base;
  seg->path = (fs::path(dir_) / segment_name(base)).string();
  if (!seg->file.open(seg->path, err)) return false;
  if (!recover_segment(*seg, err)) return false;
  segments_.push_back(std::move(seg));
  return true;
}

bool PartitionLog::recover_segment(Segment& seg, std::string& err) {
  uint64_t size = 0;
  if (!seg.file.size(size, err)) return false;
  if (size < kFileHeader) {
    if (size > 0) {
      recovery_.warnings.push_back(cat(seg.path, ": cabecera incompleta, se reinicia el segmento"));
      recovery_.truncated_bytes += size;
      if (!seg.file.truncate(0, err)) return false;
    }
    if (!seg.file.append(kMagic, kFileHeader, err)) return false;
    seg.size = kFileHeader;
    return true;
  }
  char magic[kFileHeader];
  size_t got = 0;
  if (!seg.file.read_at(magic, kFileHeader, 0, got, err)) return false;
  if (got != kFileHeader || std::memcmp(magic, kMagic, kFileHeader) != 0) {
    err = cat(seg.path, " no es un segmento de ZenRift (cabecera desconocida)");
    return false;
  }
  std::string buf;
  Scanner sc(seg.file, kFileHeader, size, opts_.max_record_bytes, buf);
  uint64_t last_index = 0;
  while (true) {
    const uint64_t pos = sc.pos();
    int64_t ts;
    std::string_view payload;
    std::string why;
    const auto st = sc.next(ts, payload, why);
    if (st == Scanner::Status::End) break;
    if (st == Scanner::Status::IoError) {
      err = cat(seg.path, ": ", why);
      return false;
    }
    if (st == Scanner::Status::Corrupt) {
      recovery_.warnings.push_back(cat(seg.path, ": ", why, " en la posición ", pos, "; se truncan ", size - pos,
                                       " bytes"));
      recovery_.truncated_bytes += size - pos;
      if (!seg.file.truncate(pos, err)) return false;
      size = pos;
      break;
    }
    if (seg.count == 0 || pos - last_index >= opts_.index_interval) {
      seg.index.push_back({static_cast<uint32_t>(seg.count), pos});
      last_index = pos;
    }
    ++seg.count;
    seg.min_ts = std::min(seg.min_ts, ts);
    seg.max_ts = std::max(seg.max_ts, ts);
  }
  seg.size = size;
  seg.last_index_pos = last_index;
  recovery_.records += static_cast<uint64_t>(seg.count);
  return true;
}

bool PartitionLog::roll(std::string& err) {
  if (!sync(err)) return false;
  return open_segment(next_offset_, err);
}

bool PartitionLog::append(int64_t ts, std::string_view payload, int64_t& offset, std::string& err) {
  if (payload.size() > opts_.max_record_bytes) {
    err = cat("evento de ", payload.size(), " bytes supera el máximo de ", opts_.max_record_bytes);
    return false;
  }
  const uint64_t rec_len = kRecordHeader + payload.size();
  Segment* seg = segments_.back().get();
  if (seg->count > 0 && seg->size + rec_len > opts_.segment_bytes) {
    if (!roll(err)) return false;
    seg = segments_.back().get();
  }
  const uint64_t pos = seg->size;
  char header[kRecordHeader];
  put_u32(header, static_cast<uint32_t>(payload.size()));
  put_i64(header + 8, ts);
  uint32_t crc = crc32c(header + 8, 8);
  crc = crc32c(payload.data(), payload.size(), crc);
  put_u32(header + 4, crc);
  wbuf_.append(header, kRecordHeader);
  wbuf_.append(payload);
  if (seg->count == 0 || pos - seg->last_index_pos >= opts_.index_interval) {
    seg->index.push_back({static_cast<uint32_t>(seg->count), pos});
    seg->last_index_pos = pos;
  }
  ++seg->count;
  seg->size += rec_len;
  seg->min_ts = std::min(seg->min_ts, ts);
  seg->max_ts = std::max(seg->max_ts, ts);
  offset = next_offset_++;
  if (wbuf_.size() >= opts_.write_buffer) return flush(err);
  return true;
}

bool PartitionLog::flush(std::string& err) {
  if (wbuf_.empty() || segments_.empty()) return true;
  if (!segments_.back()->file.append(wbuf_.data(), wbuf_.size(), err)) return false;
  wbuf_.clear();
  return true;
}

bool PartitionLog::sync(std::string& err) {
  if (!flush(err)) return false;
  return segments_.empty() || segments_.back()->file.sync(err);
}

PartitionLog::Segment* PartitionLog::find_segment(int64_t offset) const {
  // Último segmento con base <= offset.
  auto it = std::upper_bound(segments_.begin(), segments_.end(), offset,
                             [](int64_t o, const std::unique_ptr<Segment>& s) { return o < s->base; });
  if (it == segments_.begin()) return nullptr;
  return std::prev(it)->get();
}

bool PartitionLog::locate(Segment& seg, int64_t offset, uint64_t& pos, std::string& err) {
  const uint32_t rel = static_cast<uint32_t>(offset - seg.base);
  uint32_t cur = 0;
  pos = kFileHeader;
  // Última entrada del índice con rel <= objetivo (siempre existe la de rel 0).
  auto it = std::upper_bound(seg.index.begin(), seg.index.end(), rel,
                             [](uint32_t r, const Segment::IndexEntry& e) { return r < e.rel; });
  if (it != seg.index.begin()) {
    --it;
    cur = it->rel;
    pos = it->pos;
  }
  // Avanzar registro a registro desde ahí (menos de ~4 KB).
  char header[kRecordHeader];
  while (cur < rel) {
    size_t got = 0;
    if (!seg.file.read_at(header, kRecordHeader, pos, got, err)) return false;
    if (got != kRecordHeader) {
      err = "índice inconsistente";
      return false;
    }
    pos += kRecordHeader + get_u32(header);
    ++cur;
  }
  return true;
}

bool PartitionLog::read(LogCursor& cursor, size_t max_records, size_t max_bytes, std::string& scratch,
                        std::vector<LogRecord>& out, std::string& err) {
  out.clear();
  scratch.clear();
  if (!flush(err)) return false;
  if (cursor.offset < start_offset()) {
    cursor.offset = start_offset();
    cursor.segment_base = -1;
  }
  struct Pending {
    int64_t offset;
    int64_t ts;
    size_t at;
    size_t len;
  };
  std::vector<Pending> pending;
  std::string block;
  while (pending.size() < max_records && scratch.size() < max_bytes && cursor.offset < next_offset_) {
    Segment* seg = find_segment(cursor.offset);
    if (!seg) break;
    if (cursor.offset >= seg->end()) {
      // Hueco (segmento truncado en la recuperación): saltar al siguiente.
      auto it = std::find_if(segments_.begin(), segments_.end(),
                             [&](const std::unique_ptr<Segment>& s) { return s->base > cursor.offset; });
      if (it == segments_.end()) break;
      cursor.offset = (*it)->base;
      cursor.segment_base = -1;
      continue;
    }
    uint64_t pos;
    if (cursor.segment_base == seg->base) {
      pos = cursor.file_pos;
    } else if (!locate(*seg, cursor.offset, pos, err)) {
      return false;
    }
    Scanner sc(seg->file, pos, seg->size, opts_.max_record_bytes, block);
    while (pending.size() < max_records && scratch.size() < max_bytes && cursor.offset < seg->end()) {
      int64_t ts;
      std::string_view payload;
      std::string why;
      const auto st = sc.next(ts, payload, why);
      if (st == Scanner::Status::End) break;
      if (st != Scanner::Status::Ok) {
        err = cat(seg->path, ": ", why, " al leer el offset ", cursor.offset);
        return false;
      }
      pending.push_back({cursor.offset, ts, scratch.size(), payload.size()});
      scratch.append(payload);
      ++cursor.offset;
    }
    cursor.segment_base = seg->base;
    cursor.file_pos = sc.pos();
    // Al agotar un segmento cerrado, el siguiente se localiza desde su inicio;
    // en el activo se conserva la posición para seguir leyendo lo nuevo.
    if (cursor.offset >= seg->end() && seg != segments_.back().get()) cursor.segment_base = -1;
  }
  out.reserve(pending.size());
  for (const auto& p : pending) out.push_back({p.offset, p.ts, std::string_view(scratch.data() + p.at, p.len)});
  return true;
}

int64_t PartitionLog::offset_for_time(int64_t t) const {
  for (const auto& s : segments_)
    if (s->count > 0 && s->max_ts >= t) return s->base;
  return next_offset_;
}

size_t PartitionLog::enforce_retention(int64_t now_ms, uint64_t max_bytes, int64_t max_age_ms) {
  size_t removed = 0;
  while (segments_.size() > 1) {
    const Segment& oldest = *segments_.front();
    const bool too_big = max_bytes > 0 && size_bytes() > max_bytes;
    const bool too_old = max_age_ms > 0 && oldest.count > 0 && oldest.max_ts < now_ms - max_age_ms;
    if (!too_big && !too_old) break;
    const std::string path = oldest.path;
    segments_.erase(segments_.begin());  // cierra el archivo
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) log::warn("no se pudo borrar ", path, ": ", ec.message());
    ++removed;
  }
  return removed;
}

bool PartitionLog::destroy(std::string& err) {
  wbuf_.clear();
  segments_.clear();
  std::error_code ec;
  fs::remove_all(dir_, ec);
  if (ec) {
    err = cat("no se pudo borrar ", dir_, ": ", ec.message());
    return false;
  }
  next_offset_ = 0;
  return true;
}

int64_t PartitionLog::start_offset() const noexcept { return segments_.empty() ? 0 : segments_.front()->base; }

uint64_t PartitionLog::size_bytes() const noexcept {
  uint64_t t = 0;
  for (const auto& s : segments_) t += s->size;
  return t;
}

int64_t PartitionLog::min_ts() const noexcept {
  int64_t m = std::numeric_limits<int64_t>::max();
  for (const auto& s : segments_)
    if (s->count > 0) m = std::min(m, s->min_ts);
  return m;
}

int64_t PartitionLog::max_ts() const noexcept {
  int64_t m = std::numeric_limits<int64_t>::min();
  for (const auto& s : segments_)
    if (s->count > 0) m = std::max(m, s->max_ts);
  return m;
}

}  // namespace zr::storage
