#pragma once
// Stub implementation of NENative::ZipArchive.
// Provides a minimal interface so Valhalla compiles without the proprietary
// NavEngine miniz-based ZipArchive library.
// Zip-based tile extracts (.zip) will NOT be loadable — use .tar extracts instead.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NENative {

class ZipArchive {
public:
  struct Entry {
    std::string path;
    size_t uncompressedSize = 0;
  };

  ZipArchive() = default;
  ~ZipArchive() = default;

  // Always returns false — zip support is stubbed out.
  bool loadFromMMapFile(const std::string& /*path*/) { return false; }

  // Returns empty entries list.
  const std::vector<Entry>& entries() const { return empty_entries_; }

  // Returns nullptr — never called when loadFromMMapFile returns false.
  const Entry* findEntry(const std::string& /*path*/) const { return nullptr; }

  // Always returns false.
  bool decompressEntry(const Entry* /*entry*/, char* /*buffer*/) { return false; }

private:
  std::vector<Entry> empty_entries_;
};

} // namespace NENative
