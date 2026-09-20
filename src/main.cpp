#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <projectM-4/projectM.h>
#include <projectM-4/audio.h>
#include <projectM-4/core.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/playlist.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_items.h>
#include <projectM-4/playlist_playback.h>
#include <projectM-4/playlist_callbacks.h>
#include <projectM-4/playlist_memory.h>

#include "audio_capture.h"
#include "preset_switcher.h"
#include "preset_extractor.h"

// --- Help Overlay ---

static const char* kHelpText[] = {
    "MilkDrop Visualizer",
    "",
    "  H              Toggle this help",
    "  Right / Left   Next / previous preset",
    "  Space          Lock / unlock preset",
    "  Y              Toggle shuffle mode",
    "  R              Random preset",
    "  F / F11        Toggle fullscreen",
    "  I              Switch audio source",
    "  Up / Down      Adjust beat sensitivity",
    "  Esc / Ctrl+Q   Quit",
};
static const int kHelpLineCount = sizeof(kHelpText) / sizeof(kHelpText[0]);

struct HelpOverlay {
    GLuint program = 0;
    GLuint vbo = 0;
    GLuint texture = 0;
    int texW = 0;
    int texH = 0;
    float uMax = 0;
    float vMax = 0;
    bool visible = false;

    bool init();
    void render(int screenW, int screenH);
    void cleanup();
};

static GLuint compileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

bool HelpOverlay::init()
{
    if (TTF_Init() < 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
        return false;
    }

    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "C:\\Windows\\Fonts\\cour.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };

    TTF_Font* font = nullptr;
    for (auto* path : fontPaths) {
        font = TTF_OpenFont(path, 18);
        if (font) break;
    }
    if (!font) {
        SDL_Log("Could not open any system font for help overlay");
        return false;
    }

    int lineH = TTF_FontLineSkip(font);
    int padX = 20, padY = 16;

    int maxW = 0;
    for (int i = 0; i < kHelpLineCount; i++) {
        int w = 0;
        if (kHelpText[i][0] != '\0') {
            TTF_SizeUTF8(font, kHelpText[i], &w, nullptr);
        }
        if (w > maxW) maxW = w;
    }

    texW = maxW + padX * 2;
    texH = lineH * kHelpLineCount + padY * 2;

    // Round up to power of two for broad GL compatibility
    auto nextPow2 = [](int v) { int p = 1; while (p < v) p <<= 1; return p; };
    int texPow2W = nextPow2(texW);
    int texPow2H = nextPow2(texH);

    SDL_Surface* surface = SDL_CreateRGBSurface(0, texPow2W, texPow2H, 32,
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF
#else
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
#endif
    );
    if (!surface) {
        TTF_CloseFont(font);
        return false;
    }

    // Semi-transparent dark background
    SDL_Rect bgRect = {0, 0, texW, texH};
    SDL_FillRect(surface, &bgRect,
                 SDL_MapRGBA(surface->format, 0, 0, 0, 180));

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color title = {100, 200, 255, 255};

    for (int i = 0; i < kHelpLineCount; i++) {
        if (kHelpText[i][0] == '\0') continue;

        SDL_Color& col = (i == 0) ? title : white;
        SDL_Surface* line = TTF_RenderUTF8_Blended(font, kHelpText[i], col);
        if (line) {
            SDL_Rect dst = {padX, padY + i * lineH, 0, 0};
            SDL_BlitSurface(line, nullptr, surface, &dst);
            SDL_FreeSurface(line);
        }
    }

    TTF_CloseFont(font);

    // Upload to GL texture
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texPow2W, texPow2H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    uMax = static_cast<float>(texW) / texPow2W;
    vMax = static_cast<float>(texH) / texPow2H;

    SDL_FreeSurface(surface);

    // Shader
    const char* vsSrc =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aUV;\n"
        "out vec2 vUV;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "  vUV = aUV;\n"
        "}\n";
    const char* fsSrc =
        "#version 330 core\n"
        "in vec2 vUV;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D uTex;\n"
        "void main() {\n"
        "  FragColor = texture(uTex, vUV);\n"
        "}\n";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    // Quad vertices: position (NDC) and UV
    // Centered on screen, sized relative to the overlay content
    // We'll compute actual positions at render time, so just use a unit quad
    float verts[] = {
        // pos        uv
        -1, -1,   0, vMax,
         1, -1,   uMax, vMax,
         1,  1,   uMax, 0,
        -1, -1,   0, vMax,
         1,  1,   uMax, 0,
        -1,  1,   0, 0,
    };

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void HelpOverlay::render(int screenW, int screenH)
{
    if (!visible || !texture) return;

    // Convert pixel dimensions to NDC: texW pixels = texW/screenW * 2.0 in NDC
    float ndcW = static_cast<float>(texW) / screenW * 2.0f;
    float ndcH = static_cast<float>(texH) / screenH * 2.0f;

    // Center on screen
    float x0 = -ndcW / 2.0f;
    float x1 =  ndcW / 2.0f;
    float y0 = -ndcH / 2.0f;
    float y1 =  ndcH / 2.0f;

    float verts[] = {
        x0, y0,   0,    vMax,
        x1, y0,   uMax, vMax,
        x1, y1,   uMax, 0,
        x0, y0,   0,    vMax,
        x1, y1,   uMax, 0,
        x0, y1,   0,    0,
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    // Save GL state
    GLboolean depthTest, blend;
    GLint blendSrc, blendDst;
    glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    glGetBooleanv(GL_BLEND, &blend);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDst);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program, "uTex"), 0);

    // The VAO isn't shareable across GL contexts, and this can be rendered
    // under either of the app's two contexts (see PresetSwitcher), so it's
    // created fresh here rather than cached.
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);

    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Restore GL state
    if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (!blend) glDisable(GL_BLEND);
    glBlendFunc(blendSrc, blendDst);
}

void HelpOverlay::cleanup()
{
    if (texture) { glDeleteTextures(1, &texture); texture = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (program) { glDeleteProgram(program); program = 0; }
    TTF_Quit();
}

// --- App Context ---

struct AppContext {
    SDL_Window* window = nullptr;
    PresetSwitcher* switcher = nullptr;
    AudioManager* audio = nullptr;
    HelpOverlay* helpOverlay = nullptr;
    std::string presetPath;
    bool presetLocked = false;
    bool shuffleEnabled = true;
    bool isFullscreen = false;
    float beatSensitivity = 1.0f;
    int lastDrawW = 0;
    int lastDrawH = 0;
    Uint32 lastAutoAdvance = 0;
    // Saved windowed geometry, to restore when leaving borderless fullscreen.
    int savedX = 0, savedY = 0, savedW = 0, savedH = 0;
};

// Keep projectM's render size matched to the window's drawable size.
static void syncWindowSize(AppContext& ctx)
{
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(ctx.window, &w, &h);
    if (w > 0 && h > 0 && (w != ctx.lastDrawW || h != ctx.lastDrawH)) {
        ctx.lastDrawW = w;
        ctx.lastDrawH = h;
        ctx.switcher->resize(w, h);
    }
}

static void updateWindowTitle(AppContext& ctx)
{
    std::string title = "MilkDrop Visualizer";

    uint32_t index = projectm_playlist_get_position(ctx.switcher->front.playlist);
    char* presetName = projectm_playlist_item(ctx.switcher->front.playlist, index);
    if (presetName) {
        std::filesystem::path p(presetName);
        title += " - " + p.stem().string();
        projectm_playlist_free_string(presetName);
    }

    if (ctx.presetLocked) {
        title += " [Locked]";
    }
    title += ctx.shuffleEnabled ? " [Shuffle]" : " [Sequential]";
    title += " | " + ctx.audio->getCurrentSourceName();

    SDL_SetWindowTitle(ctx.window, title.c_str());
}

// Toggle borderless fake-fullscreen. Only the window changes size; projectM's
// render resolution is fixed (see RenderTarget), so no renderer reset occurs.
static void toggleFullscreen(AppContext& ctx)
{
    ctx.isFullscreen = !ctx.isFullscreen;

    if (ctx.isFullscreen) {
        SDL_GetWindowPosition(ctx.window, &ctx.savedX, &ctx.savedY);
        SDL_GetWindowSize(ctx.window, &ctx.savedW, &ctx.savedH);

        int displayIndex = SDL_GetWindowDisplayIndex(ctx.window);
        if (displayIndex < 0) {
            displayIndex = 0;
        }
        SDL_Rect bounds;
        SDL_GetDisplayBounds(displayIndex, &bounds);

        // Size 1px taller than the display so the window does NOT exactly
        // cover it. This keeps Windows from promoting the borderless window to
        // fullscreen-exclusive "independent flip" mode, whose runtime
        // transition stalls the GPU on some drivers. The extra row is offscreen.
        SDL_SetWindowBordered(ctx.window, SDL_FALSE);
        SDL_SetWindowPosition(ctx.window, bounds.x, bounds.y);
        SDL_SetWindowSize(ctx.window, bounds.w, bounds.h + 1);
        SDL_ShowCursor(SDL_DISABLE);
    }
    else {
        SDL_SetWindowBordered(ctx.window, SDL_TRUE);
        SDL_SetWindowSize(ctx.window, ctx.savedW, ctx.savedH);
        SDL_SetWindowPosition(ctx.window, ctx.savedX, ctx.savedY);
        SDL_ShowCursor(SDL_ENABLE);
    }
}

static void handleKeyDown(SDL_Event& event, AppContext& ctx, bool& running)
{
    bool ctrl = (event.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
    SDL_Keycode key = event.key.keysym.sym;

    if (key == SDLK_ESCAPE || (ctrl && key == SDLK_q)) {
        running = false;
    }
    else if (key == SDLK_h) {
        ctx.helpOverlay->visible = !ctx.helpOverlay->visible;
    }
    else if (key == SDLK_RIGHT) {
        if (ctx.switcher->requestSwitch(SwitchDirection::Next)) {
            ctx.lastAutoAdvance = SDL_GetTicks();
        }
    }
    else if (key == SDLK_LEFT) {
        if (ctx.switcher->requestSwitch(SwitchDirection::Previous)) {
            ctx.lastAutoAdvance = SDL_GetTicks();
        }
    }
    else if (key == SDLK_SPACE) {
        ctx.presetLocked = !ctx.presetLocked;
        projectm_set_preset_locked(ctx.switcher->front.pm, ctx.presetLocked);
        projectm_set_preset_locked(ctx.switcher->back.pm, ctx.presetLocked);
        updateWindowTitle(ctx);
    }
    else if (key == SDLK_y) {
        ctx.shuffleEnabled = !ctx.shuffleEnabled;
        projectm_playlist_set_shuffle(ctx.switcher->front.playlist, ctx.shuffleEnabled);
        projectm_playlist_set_shuffle(ctx.switcher->back.playlist, ctx.shuffleEnabled);
        updateWindowTitle(ctx);
    }
    else if (key == SDLK_r) {
        if (ctx.switcher->requestSwitch(SwitchDirection::Next)) {
            ctx.lastAutoAdvance = SDL_GetTicks();
        }
    }
    else if (key == SDLK_f || key == SDLK_F11) {
        toggleFullscreen(ctx);
    }
    else if (key == SDLK_i) {
        ctx.audio->toggleSource();
        updateWindowTitle(ctx);
    }
    else if (key == SDLK_UP) {
        ctx.beatSensitivity = std::min(ctx.beatSensitivity + 0.1f, 5.0f);
        projectm_set_beat_sensitivity(ctx.switcher->front.pm, ctx.beatSensitivity);
        projectm_set_beat_sensitivity(ctx.switcher->back.pm, ctx.beatSensitivity);
    }
    else if (key == SDLK_DOWN) {
        ctx.beatSensitivity = std::max(ctx.beatSensitivity - 0.1f, 0.1f);
        projectm_set_beat_sensitivity(ctx.switcher->front.pm, ctx.beatSensitivity);
        projectm_set_beat_sensitivity(ctx.switcher->back.pm, ctx.beatSensitivity);
    }
}

static void printHelp(const char* programName)
{
    std::printf(
        "MilkDrop Visualizer - Music visualization using projectM\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -p, --presets <path>   Path to directory containing .milk preset files\n"
        "                         (default: presets embedded in this executable, or\n"
        "                         PROJECTM_PRESETS_PATH env var)\n"
        "  -a, --audio <source>   Audio source: loopback, mic, device index, or name\n"
        "                         (default: loopback on Windows, mic otherwise)\n"
        "      --list-audio       List available audio capture devices and exit\n"
        "  -f, --fullscreen       Start in fullscreen mode\n"
        "  -h, --help             Show this help message and exit\n"
        "\n"
        "Keyboard Controls:\n"
        "  H                      Toggle on-screen help\n"
        "  Right/Left Arrow       Next/previous preset\n"
        "  Space                  Lock/unlock current preset\n"
        "  Y                      Toggle shuffle mode\n"
        "  R                      Random preset\n"
        "  F / F11                Toggle fullscreen\n"
        "  I                      Switch audio source (WASAPI loopback / mic)\n"
        "  Up/Down Arrow          Adjust beat sensitivity\n"
        "  Escape / Ctrl+Q        Quit\n",
        programName);
}

static std::string parseOption(int argc, char* argv[], const char* longOpt, const char* shortOpt)
{
    for (int i = 1; i < argc - 1; i++) {
        if (std::strcmp(argv[i], longOpt) == 0 || (shortOpt && std::strcmp(argv[i], shortOpt) == 0)) {
            return argv[i + 1];
        }
    }
    return "";
}

// Returns the explicitly requested preset directory (via -p/--presets or
// PROJECTM_PRESETS_PATH), or empty if the caller should fall back to the
// presets embedded in this executable (see preset_extractor.h).
static std::string parseExplicitPresetPath(int argc, char* argv[])
{
    std::string val = parseOption(argc, argv, "--presets", "-p");
    if (!val.empty())
        return val;

    const char* envPath = std::getenv("PROJECTM_PRESETS_PATH");
    if (envPath && *envPath) {
        return envPath;
    }

    return "";
}

static bool parseFlag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char* argv[])
{
    if (parseFlag(argc, argv, "-h") || parseFlag(argc, argv, "--help")) {
        printHelp(argv[0]);
        return 0;
    }

    std::string presetPath = parseExplicitPresetPath(argc, argv);
    std::string audioSource = parseOption(argc, argv, "--audio", "-a");
    bool startFullscreen = parseFlag(argc, argv, "--fullscreen") ||
                           parseFlag(argc, argv, "-f");
    bool listAudio = parseFlag(argc, argv, "--list-audio");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    if (listAudio) {
        AudioManager::printAudioDevices();
        SDL_Quit();
        return 0;
    }

    if (presetPath.empty()) {
        presetPath = extractEmbeddedPresets();
    }
    if (presetPath.empty()) {
        presetPath = "presets";
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    int winWidth = 1280;
    int winHeight = 720;
    SDL_Rect displayBounds;
    if (SDL_GetDisplayUsableBounds(0, &displayBounds) == 0) {
        winWidth = displayBounds.w * 3 / 4;
        winHeight = displayBounds.h * 3 / 4;
    }

    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    SDL_Window* window = SDL_CreateWindow(
        "MilkDrop Visualizer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        winWidth, winHeight, windowFlags);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext glCtx = SDL_GL_CreateContext(window);
    if (!glCtx) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        SDL_Log("glewInit failed: %s", glewGetErrorString(glewErr));
        SDL_GL_DeleteContext(glCtx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (SDL_GL_SetSwapInterval(-1) == -1) {
        SDL_GL_SetSwapInterval(1);
    }

    projectm_handle pm = projectm_create();
    if (!pm) {
        SDL_Log("projectm_create() failed - check OpenGL context");
        SDL_GL_DeleteContext(glCtx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int drawW = 0, drawH = 0;
    SDL_GL_GetDrawableSize(window, &drawW, &drawH);

    PresetSwitcher switcher;
    if (!switcher.init(window, pm, drawW, drawH, presetPath)) {
        SDL_Log("Failed to initialize preset switcher");
        projectm_destroy(pm);
        SDL_GL_DeleteContext(glCtx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_Log("Loaded %u presets from: %s",
            projectm_playlist_size(switcher.front.playlist), presetPath.c_str());
    if (projectm_playlist_size(switcher.front.playlist) == 0) {
        SDL_Log("Warning: No .milk presets found in %s", presetPath.c_str());
    }

    AudioManager audio;
    audio.init(switcher.front.pm, audioSource);

    HelpOverlay helpOverlay;
    helpOverlay.init();

    AppContext ctx;
    ctx.window = window;
    ctx.switcher = &switcher;
    ctx.audio = &audio;
    ctx.helpOverlay = &helpOverlay;
    ctx.presetPath = presetPath;
    ctx.lastDrawW = drawW;
    ctx.lastDrawH = drawH;

    switcher.pickInitialPreset();
    updateWindowTitle(ctx);

    if (startFullscreen) {
        toggleFullscreen(ctx);
    }

    bool running = true;
    const Uint32 frameDelay = 1000 / 60;
    const Uint32 autoAdvanceDelay = 30000;
    ctx.lastAutoAdvance = SDL_GetTicks();

    const bool profile = parseFlag(argc, argv, "--profile");
    std::FILE* profLog = nullptr;
    if (profile) {
        profLog = std::fopen("profile.log", "w");
    }
    Uint64 perfFreq = SDL_GetPerformanceFrequency();
    double accEvent = 0, accAudio = 0, accRender = 0, accSwap = 0;
    int profFrames = 0;
    int maxPixel = 0;
    Uint32 profLastReport = SDL_GetTicks();

    while (running) {
        Uint32 frameStart = SDL_GetTicks();
        Uint64 t0 = SDL_GetPerformanceCounter();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    syncWindowSize(ctx);
                }
                break;
            case SDL_KEYDOWN:
                handleKeyDown(event, ctx, running);
                break;
            }
        }

        Uint64 t1 = SDL_GetPerformanceCounter();
        audio.processFrame();
        Uint64 t2 = SDL_GetPerformanceCounter();

        if (!ctx.presetLocked && switcher.state() == SwitchState::Idle &&
            SDL_GetTicks() - ctx.lastAutoAdvance >= autoAdvanceDelay) {
            if (switcher.requestSwitch(SwitchDirection::Next)) {
                ctx.lastAutoAdvance = SDL_GetTicks();
            }
        }

        SDL_GL_GetDrawableSize(window, &drawW, &drawH);
        switcher.renderFrame(audio, static_cast<double>(frameDelay) / 1000.0, drawW, drawH);
        if (switcher.consumeSwapEvent()) {
            updateWindowTitle(ctx);
        }

        // Correctness probe: sample a pixel at 1/3 across (off the centered
        // help overlay) to confirm the visualization is actually rendering.
        int probeMax = -1;
        if (profile) {
            unsigned char px[4] = {0, 0, 0, 0};
            glReadPixels(drawW / 3, drawH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            probeMax = px[0];
            if (px[1] > probeMax) probeMax = px[1];
            if (px[2] > probeMax) probeMax = px[2];
        }

        helpOverlay.render(drawW, drawH);
        Uint64 t3 = SDL_GetPerformanceCounter();

        SDL_GL_SwapWindow(window);
        Uint64 t4 = SDL_GetPerformanceCounter();

        if (profile) {
            accEvent  += (double)(t1 - t0) / perfFreq * 1000.0;
            accAudio  += (double)(t2 - t1) / perfFreq * 1000.0;
            accRender += (double)(t3 - t2) / perfFreq * 1000.0;
            accSwap   += (double)(t4 - t3) / perfFreq * 1000.0;
            if (probeMax > maxPixel) maxPixel = probeMax;
            profFrames++;
            if (SDL_GetTicks() - profLastReport >= 1000) {
                char line[256];
                std::snprintf(line, sizeof(line),
                        "[%dx%d] %d fps | event %.2f ms | render %.2f ms | swap %.2f ms | maxpixel %d",
                        drawW, drawH, profFrames,
                        accEvent / profFrames,
                        accRender / profFrames, accSwap / profFrames, maxPixel);
                SDL_Log("%s", line);
                if (profLog) {
                    std::fprintf(profLog, "%s\n", line);
                    std::fflush(profLog);
                }
                accEvent = accAudio = accRender = accSwap = 0;
                profFrames = 0;
                maxPixel = 0;
                profLastReport = SDL_GetTicks();
            }
        }

        Uint32 elapsed = SDL_GetTicks() - frameStart;
        if (elapsed < frameDelay) {
            SDL_Delay(frameDelay - elapsed);
        }
    }

    if (profLog) {
        std::fclose(profLog);
    }
    helpOverlay.cleanup();
    audio.shutdown();
    switcher.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
