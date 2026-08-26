#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <GL/glew.h>
#include <SDL2/SDL.h>

#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>

#include "audio_capture.h"

// A background-loaded projectM preset instance: its own projectM handle,
// playlist, and GL context.
//
// projectM always renders its final output to draw-framebuffer 0 of whatever
// context is current (confirmed in its source -- ProjectM.cpp explicitly does
// `glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0)` before its last blit, with a
// "ToDo: Allow external apps to provide a custom target framebuffer" comment
// -- there's no supported way to render it directly into an FBO). So instead
// `fbo`/`colorTex` are a pure capture target: after letting projectM draw to
// framebuffer 0 as normal, that frame is copied into `colorTex` via
// glBlitFramebuffer so it can be sampled as a texture for crossfade
// compositing.
struct PresetInstance {
    projectm_handle pm = nullptr;
    projectm_playlist_handle playlist = nullptr;
    SDL_GLContext glContext = nullptr;
    GLuint fbo = 0;
    GLuint colorTex = 0;
    int fboW = 0;
    int fboH = 0;
};

enum class SwitchDirection {
    Next,
    Previous,
    Last
};

enum class SwitchState {
    Idle,
    Warming,
    Blending
};

// Eliminates the preset-switch stall (projectM synchronously compiles a
// preset's shaders on whatever thread loads it) by keeping a second,
// off-screen projectM instance ("back") that loads/compiles the next preset
// on a worker thread while the visible instance ("front") keeps rendering.
// Once the background load is warmed up, the app crossfades between the two
// instances' rendered output itself, then the two swap roles.
class PresetSwitcher {
public:
    PresetSwitcher() = default;
    ~PresetSwitcher() = default;
    PresetSwitcher(const PresetSwitcher&) = delete;
    PresetSwitcher& operator=(const PresetSwitcher&) = delete;

    // `window`'s GL context must already be current on the calling thread,
    // and `frontPm` must have been created against it.
    bool init(SDL_Window* window, projectm_handle frontPm, int drawW, int drawH,
              const std::string& presetPath);
    void shutdown();

    // Picks the first preset synchronously (fine at startup -- nothing has
    // rendered yet for a stall to interrupt) and mirrors it onto the back
    // instance's playlist so both playlists' shuffle histories start aligned.
    // No-op if the playlist is empty.
    void pickInitialPreset();

    // Returns false (no-op) if a switch is already in flight.
    bool requestSwitch(SwitchDirection dir);

    void resize(int drawW, int drawH);

    // Renders the current frame. Leaves `front.glContext` current and the
    // default framebuffer bound with the composited image drawn into it.
    void renderFrame(AudioManager& audio, double dtSeconds, int drawW, int drawH);

    // True exactly once, on the frame a role-swap (i.e. a switch) completes.
    bool consumeSwapEvent();

    SwitchState state() const { return state_; }

    PresetInstance front;
    PresetInstance back;

private:
    void workerLoop();
    void initCompositor();
    void destroyCompositor();

    // The main, visible window (owns front.glContext's on-screen output).
    SDL_Window* window_ = nullptr;
    // A hidden window that exists purely to give back.glContext a private
    // framebuffer-0 to force-render into (see PresetInstance comment) --
    // without this, the worker thread's background warm-up renders and the
    // per-frame blend renders would land directly in the visible window's
    // backbuffer (both contexts share the same window otherwise), racing
    // with and corrupting whatever the main thread just drew there.
    SDL_Window* backWindow_ = nullptr;

    std::thread worker_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    bool hasJob_ = false;
    bool stopWorker_ = false;
    SwitchDirection pendingDirection_ = SwitchDirection::Next;
    std::atomic<bool> warmReady_{false};

    SwitchState state_ = SwitchState::Idle;
    double blendElapsed_ = 0.0;
    double blendDuration_ = 3.0;
    bool swapEventPending_ = false;

    bool backResizePending_ = false;
    int pendingResizeW_ = 0;
    int pendingResizeH_ = 0;

    // Program and VBO are shareable GL objects, valid under either context.
    // The VAO is NOT shareable, so it's created fresh at draw time instead of
    // cached here (see renderFrame) -- it must be valid in whichever of the
    // two contexts is current, which alternates every role-swap.
    GLuint compositeProgram_ = 0;
    GLuint compositeVbo_ = 0;
};
