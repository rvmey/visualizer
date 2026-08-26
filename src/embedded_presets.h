#pragma once

#include <cstdint>

// Describes one .milk preset packed into the PRESETS_PACK RCDATA resource
// (see app_presets.rc.in / tools/preset_packer.cpp). kEmbeddedPresets and the
// other symbols below are defined in a generated .cpp produced by
// preset_packer at build time, not checked into source control.
struct EmbeddedPresetEntry {
    const char* relativePath;
    uint64_t offset;
    uint64_t size;
};

extern const EmbeddedPresetEntry kEmbeddedPresets[];
extern const uint32_t kEmbeddedPresetCount;
