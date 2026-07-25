#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {

constexpr char EMBEDDED_SECTION_MAGIC[8] =
    {'E', 'R', 'W', 'T', 'S', 'E', 'C', '\0'};
constexpr uint32_t EMBEDDED_SECTION_VERSION = 1;
constexpr uint64_t EMBEDDED_SECTION_ALIGNMENT = 4096;

enum class EmbeddedSectionType : uint32_t {
    Lz4AxisPlaneX = 1,
    Lz4AxisPlaneY = 2,
    Lz4AxisPlaneZ = 3,
    RzfpAxisLeafX = 16,
    RzfpAxisLeafY = 17,
    RzfpAxisLeafZ = 18,
};

#pragma pack(push, 1)
struct EmbeddedSectionDirectoryHeader {
    char magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint64_t directory_bytes;
    uint64_t package_bytes;
    uint64_t reserved[4];
};

struct EmbeddedSectionEntry {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t bytes;
    uint64_t reserved[5];
};
#pragma pack(pop)

static_assert(sizeof(EmbeddedSectionDirectoryHeader) == 64,
              "embedded section directory header must be 64 bytes");
static_assert(sizeof(EmbeddedSectionEntry) == 64,
              "embedded section entry must be 64 bytes");

struct EmbeddedSectionInput {
    EmbeddedSectionType type{};
    std::string path;
};

struct EmbeddedSectionInfo {
    EmbeddedSectionType type{};
    uint64_t offset = 0;
    uint64_t bytes = 0;
};

struct EmbeddedPackageStats {
    uint64_t source_bytes = 0;
    uint64_t section_bytes = 0;
    uint64_t padding_bytes = 0;
    uint64_t directory_bytes = 0;
    uint64_t package_bytes = 0;
};

bool readEmbeddedSectionDirectory(
    int fd,
    uint64_t directoryOffset,
    uint64_t directoryBytes,
    uint64_t fileBytes,
    std::vector<EmbeddedSectionInfo>& sections);

const EmbeddedSectionInfo* findEmbeddedSection(
    const std::vector<EmbeddedSectionInfo>& sections,
    EmbeddedSectionType type);

// Appends each source file unchanged, writes one final directory, then commits
// the primary header. On failure the primary file is truncated back to its
// original size and its original header remains valid.
bool embedSectionsInPlace(
    const std::string& primaryPath,
    const std::vector<EmbeddedSectionInput>& inputs,
    bool removeSources,
    EmbeddedPackageStats* stats = nullptr);

} // namespace erwt3d
