#include "preset_extractor.h"
#include "embedded_presets.h"

#include <windows.h>

#include <SDL2/SDL.h>

#include <filesystem>
#include <fstream>

namespace {
constexpr const char* kPackResourceName = "PRESETS_PACK";
}

std::string extractEmbeddedPresets()
{
    HMODULE module = GetModuleHandleA(nullptr);
    HRSRC resInfo = FindResourceA(module, kPackResourceName, RT_RCDATA);
    if (!resInfo) {
        return "";
    }
    HGLOBAL resHandle = LoadResource(module, resInfo);
    const uint8_t* packData = resHandle ? static_cast<const uint8_t*>(LockResource(resHandle)) : nullptr;
    DWORD packSize = SizeofResource(module, resInfo);
    if (!packData || packSize == 0) {
        return "";
    }

    char* prefPathC = SDL_GetPrefPath("", "MilkdropVisualizer");
    if (!prefPathC) {
        return "";
    }
    std::filesystem::path presetsDir = std::filesystem::path(prefPathC) / "presets";
    SDL_free(prefPathC);

    // A single line recording the pack's size and preset count: cheap to
    // compare, and changes whenever the embedded preset set is rebuilt, so
    // it's used to skip re-extracting on every launch.
    std::string markerValue = std::to_string(packSize) + ":" + std::to_string(kEmbeddedPresetCount);
    std::filesystem::path markerPath = presetsDir / ".extracted";

    {
        std::ifstream markerIn(markerPath);
        std::string existingMarker;
        if (markerIn && std::getline(markerIn, existingMarker) && existingMarker == markerValue) {
            return presetsDir.string();
        }
    }

    SDL_Log("Extracting %u embedded presets to %s ...", kEmbeddedPresetCount, presetsDir.string().c_str());

    std::error_code ec;
    std::filesystem::remove_all(presetsDir, ec);

    for (uint32_t i = 0; i < kEmbeddedPresetCount; i++) {
        const EmbeddedPresetEntry& entry = kEmbeddedPresets[i];
        std::filesystem::path outPath = presetsDir / entry.relativePath;
        std::filesystem::create_directories(outPath.parent_path(), ec);
        std::ofstream out(outPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(packData + entry.offset), static_cast<std::streamsize>(entry.size));
    }

    std::ofstream markerOut(markerPath, std::ios::trunc);
    markerOut << markerValue;

    SDL_Log("Preset extraction complete.");
    return presetsDir.string();
}
