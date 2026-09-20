# MilkDrop Visualizer

A standalone music visualizer powered by [projectM](https://github.com/projectM-visualizer/projectm), rendering classic MilkDrop presets in real time. Captures system audio via WASAPI loopback (Windows) or microphone input via SDL, and displays the visualization in an OpenGL window.

## Dependencies

- [projectM 4](https://github.com/projectM-visualizer/projectm) (with playlist support)
- [SDL2](https://www.libsdl.org/)
- [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf)
- [GLEW](https://glew.sourceforge.net/)
- CMake 3.21+
- C++17 compiler (MSVC recommended on Windows)

All libraries can be installed via [vcpkg](https://vcpkg.io/):

```
vcpkg install projectm sdl2 sdl2-ttf glew
```

## Building

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The build copies required DLLs to the output directory automatically.

## Usage

```
MilkdropVisualizer [options]
```

### Options

| Flag | Description |
|------|-------------|
| `-p, --presets <path>` | Path to directory containing `.milk` preset files (default: presets embedded in the executable, or `PROJECTM_PRESETS_PATH` env var) |
| `-a, --audio <source>` | Audio source: `loopback`, `mic`, device index, or device name (default: `loopback` on Windows) |
| `--list-audio` | List available audio capture devices and exit |
| `-f, --fullscreen` | Start in fullscreen mode |
| `--profile` | Log per-frame performance stats to `profile.log` |
| `-h, --help` | Show help and exit |

### Examples

```bash
# Launch with default settings (system audio, windowed)
MilkdropVisualizer

# Use a specific preset directory and start fullscreen
MilkdropVisualizer -p "C:\presets\my_collection" -f

# Use microphone input instead of system audio
MilkdropVisualizer --audio mic

# Use a specific audio device by index
MilkdropVisualizer --audio 0

# Use a specific audio device by name
MilkdropVisualizer --audio "Realtek"

# List available audio devices
MilkdropVisualizer --list-audio
```

## Keyboard Controls

| Key | Action |
|-----|--------|
| `H` | Toggle on-screen help overlay |
| `Right` / `Left` | Next / previous preset |
| `Space` | Lock / unlock current preset |
| `Y` | Toggle shuffle mode |
| `R` | Random preset |
| `F` / `F11` | Toggle fullscreen |
| `I` | Switch audio source (WASAPI loopback / mic) |
| `Up` / `Down` | Adjust beat sensitivity |
| `Esc` / `Ctrl+Q` | Quit |

## Presets

The full preset library ships baked into the executable — no external presets folder is required to run it. At build time, [`tools/preset_packer.cpp`](tools/preset_packer.cpp) recursively collects every `.milk` file under `MILKDROP_PRESETS_SOURCE_DIR` (a CMake cache variable, defaulting to `D:/appdev/milkdrop/presets-cream-of-the-crop`) into a single blob that's embedded as a resource in the `.exe`. On first launch, the app unpacks that blob into `%APPDATA%\MilkdropVisualizer\presets` (a one-time cost of well under a minute); later launches detect the cached copy and skip straight to loading.

**To update the embedded preset set:**

1. Point `MILKDROP_PRESETS_SOURCE_DIR` at the folder you want to embed (subfolders are scanned recursively), either by editing the cache variable or reconfiguring:
   ```bash
   cmake -B build -DMILKDROP_PRESETS_SOURCE_DIR="D:/path/to/presets"
   ```
2. Delete the previously generated pack so it gets rebuilt:
   ```bash
   rm build/generated/presets.pack
   ```
3. Rebuild as usual (`cmake --build build --config Release`). The next run of the app will detect the new pack (the size/count no longer matches its cached marker) and re-extract automatically.

To use a preset folder without rebuilding at all — e.g. while testing new presets — pass `-p`/`--presets` or set `PROJECTM_PRESETS_PATH`; either bypasses the embedded set entirely and reads straight from disk.

## Audio Sources

On Windows, the visualizer defaults to **WASAPI loopback** capture, which mirrors whatever audio is playing through the default output device. This means any music, video, or game audio is visualized without extra configuration.

Alternatively, **microphone input** can be selected via the `-a mic` flag or by pressing `I` at runtime to cycle through sources. Specific capture devices can be targeted by index or name substring.

## License

This project uses the projectM library, which is licensed under the LGPL. See the [projectM repository](https://github.com/projectM-visualizer/projectm) for details.
