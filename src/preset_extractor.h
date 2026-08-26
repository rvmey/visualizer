#pragma once

#include <string>

// Extracts the .milk presets embedded in this executable's PRESETS_PACK
// resource (populated at build time by tools/preset_packer.cpp) into a
// per-user cache directory, skipping the copy if a prior run already
// extracted the same pack. Returns the directory presets were extracted to,
// or an empty string if this executable has no embedded presets (e.g. a
// build that skipped the packing step) or extraction failed.
std::string extractEmbeddedPresets();
