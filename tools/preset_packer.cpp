// Build-time tool: concatenates every .milk file under a directory tree into
// a single blob file, plus a generated .cpp manifest (offset/size per file)
// used by src/preset_extractor.cpp to unpack them at runtime. The blob is
// embedded into the executable as the PRESETS_PACK RCDATA resource
// (see app_presets.rc.in), so the app ships with no external presets folder.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry {
    std::string relativePath;
    uint64_t offset;
    uint64_t size;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "Usage: preset_packer <presets_dir> <out_pack_file> <out_manifest_cpp>\n");
        return 1;
    }

    fs::path presetsDir = argv[1];
    fs::path outPack = argv[2];
    fs::path outManifest = argv[3];

    if (!fs::is_directory(presetsDir)) {
        std::fprintf(stderr, "preset_packer: '%s' is not a directory\n", presetsDir.string().c_str());
        return 1;
    }

    std::vector<fs::path> files;
    for (auto& entry : fs::recursive_directory_iterator(presetsDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".milk") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    fs::create_directories(outPack.parent_path());
    std::ofstream pack(outPack, std::ios::binary | std::ios::trunc);
    if (!pack) {
        std::fprintf(stderr, "preset_packer: failed to open '%s' for writing\n", outPack.string().c_str());
        return 1;
    }

    std::vector<Entry> entries;
    entries.reserve(files.size());
    uint64_t offset = 0;
    for (auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "preset_packer: warning: could not read '%s', skipping\n", path.string().c_str());
            continue;
        }
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        pack.write(data.data(), static_cast<std::streamsize>(data.size()));

        // generic_string() always uses forward slashes, which are both valid
        // path separators on Windows and safe to embed in a C string literal
        // (Windows filenames can't contain '\' or '"').
        entries.push_back({fs::relative(path, presetsDir).generic_string(), offset, data.size()});
        offset += data.size();
    }
    pack.close();

    std::ofstream manifest(outManifest, std::ios::trunc);
    if (!manifest) {
        std::fprintf(stderr, "preset_packer: failed to open '%s' for writing\n", outManifest.string().c_str());
        return 1;
    }
    manifest << "#include \"embedded_presets.h\"\n\n";
    manifest << "const EmbeddedPresetEntry kEmbeddedPresets[] = {\n";
    for (auto& entry : entries) {
        manifest << "    { \"" << entry.relativePath << "\", " << entry.offset << "ULL, " << entry.size << "ULL },\n";
    }
    manifest << "};\n\n";
    manifest << "const uint32_t kEmbeddedPresetCount = " << entries.size() << ";\n";
    manifest.close();

    std::printf("preset_packer: packed %zu presets (%llu bytes) into '%s'\n",
                entries.size(), static_cast<unsigned long long>(offset), outPack.string().c_str());
    return 0;
}
