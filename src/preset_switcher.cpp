#include "preset_switcher.h"

#include <filesystem>

#include <projectM-4/core.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_items.h>
#include <projectM-4/playlist_playback.h>
#include <projectM-4/playlist_memory.h>

namespace {

void configureInstance(projectm_handle pm, int w, int h)
{
    projectm_set_window_size(pm, w, h);
    // The app drives all preset switching itself (see PresetSwitcher), so
    // projectM must never auto-advance or hard-cut on its own -- both would
    // bypass the background-warm pipeline and stall the visible instance.
    projectm_set_preset_duration(pm, 1e9);
    projectm_set_hard_cut_enabled(pm, false);
    projectm_set_beat_sensitivity(pm, 1.0f);
    projectm_set_mesh_size(pm, 48, 36);
    projectm_set_fps(pm, 60);
    projectm_set_aspect_correction(pm, true);
}

GLuint compileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

// fbo/colorTex is a pure blit-capture target: projectM never draws into it
// directly (see PresetInstance), so no depth attachment is needed -- only
// glBlitFramebuffer's GL_COLOR_BUFFER_BIT is ever used against it.
void createOrResizeFbo(PresetInstance& inst, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (inst.fbo != 0 && inst.fboW == w && inst.fboH == h) {
        return;
    }

    if (inst.fbo == 0) {
        glGenFramebuffers(1, &inst.fbo);
        glGenTextures(1, &inst.colorTex);
    }

    glBindTexture(GL_TEXTURE_2D, inst.colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, inst.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst.colorTex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        SDL_Log("Warning: preset instance FBO incomplete (0x%x)", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    inst.fboW = w;
    inst.fboH = h;
}

void destroyFbo(PresetInstance& inst)
{
    if (inst.fbo) { glDeleteFramebuffers(1, &inst.fbo); inst.fbo = 0; }
    if (inst.colorTex) { glDeleteTextures(1, &inst.colorTex); inst.colorTex = 0; }
    inst.fboW = inst.fboH = 0;
}

// Copies whatever was just rendered into framebuffer 0 of the CURRENTLY
// current context into inst.colorTex, so it can be sampled later regardless
// of which context is current then.
void captureFrame(PresetInstance& inst, int drawW, int drawH)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, inst.fbo);
    glBlitFramebuffer(0, 0, drawW, drawH, 0, 0, inst.fboW, inst.fboH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace

bool PresetSwitcher::init(SDL_Window* window, projectm_handle frontPm, int drawW, int drawH,
                           const std::string& presetPath)
{
    window_ = window;

    // --- front: uses the already-current main context ---
    front.glContext = SDL_GL_GetCurrentContext();
    front.pm = frontPm;
    configureInstance(front.pm, drawW, drawH);
    front.playlist = projectm_playlist_create(front.pm);
    if (!front.playlist) {
        SDL_Log("Failed to create front playlist");
        return false;
    }
    if (std::filesystem::is_directory(presetPath)) {
        projectm_playlist_add_path(front.playlist, presetPath.c_str(), true, false);
    }
    projectm_playlist_set_shuffle(front.playlist, true);
    createOrResizeFbo(front, drawW, drawH);

    // --- back: a second, shared context on its own hidden window ---
    // A private window is required, not just a private context: projectM
    // always force-renders to framebuffer 0 of whatever context is current
    // (see PresetInstance), so if back.glContext shared the *same* window as
    // front, its background warm-up and blend renders would land directly in
    // the visible window's backbuffer, racing with front's own rendering.
    backWindow_ = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                    drawW, drawH, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!backWindow_) {
        SDL_Log("Failed to create hidden window for background preset instance: %s", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    back.glContext = SDL_GL_CreateContext(backWindow_);
    if (!back.glContext) {
        SDL_Log("Failed to create shared context for background preset instance: %s", SDL_GetError());
        return false;
    }
    // SDL_GL_CreateContext makes the new context current on this thread. Re-resolve
    // GL entry points while it's current so extension pointers are valid for it too
    // -- safe here since the worker thread hasn't started yet, so nothing else is
    // touching GL concurrently.
    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        SDL_Log("glewInit failed for background context: %s", glewGetErrorString(glewErr));
        return false;
    }

    back.pm = projectm_create();
    if (!back.pm) {
        SDL_Log("Failed to create background projectM instance");
        return false;
    }
    configureInstance(back.pm, drawW, drawH);
    back.playlist = projectm_playlist_create(back.pm);
    if (!back.playlist) {
        SDL_Log("Failed to create back playlist");
        return false;
    }
    if (std::filesystem::is_directory(presetPath)) {
        projectm_playlist_add_path(back.playlist, presetPath.c_str(), true, false);
    }
    projectm_playlist_set_shuffle(back.playlist, true);
    createOrResizeFbo(back, drawW, drawH);

    // Release the back context so the worker thread can claim it, then restore
    // the front context on this (main) thread.
    SDL_GL_MakeCurrent(backWindow_, nullptr);
    SDL_GL_MakeCurrent(window_, front.glContext);

    initCompositor();

    worker_ = std::thread(&PresetSwitcher::workerLoop, this);
    return true;
}

void PresetSwitcher::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        stopWorker_ = true;
    }
    jobCv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }

    // front's context is assumed current on this thread on entry.
    destroyCompositor();
    destroyFbo(front);
    if (front.playlist) { projectm_playlist_destroy(front.playlist); front.playlist = nullptr; }
    projectm_destroy(front.pm);
    front.pm = nullptr;

    SDL_GL_MakeCurrent(backWindow_, back.glContext);
    destroyFbo(back);
    if (back.playlist) { projectm_playlist_destroy(back.playlist); back.playlist = nullptr; }
    projectm_destroy(back.pm);
    back.pm = nullptr;

    SDL_GL_MakeCurrent(backWindow_, nullptr);
    SDL_GL_DeleteContext(back.glContext);
    back.glContext = nullptr;
    SDL_GL_DeleteContext(front.glContext);
    front.glContext = nullptr;

    SDL_DestroyWindow(backWindow_);
    backWindow_ = nullptr;
}

void PresetSwitcher::pickInitialPreset()
{
    if (projectm_playlist_size(front.playlist) == 0) {
        return;
    }
    projectm_playlist_play_next(front.playlist, true);
    uint32_t idx = projectm_playlist_get_position(front.playlist);
    SDL_GL_MakeCurrent(backWindow_, back.glContext);
    projectm_playlist_set_position(back.playlist, idx, true);
    SDL_GL_MakeCurrent(window_, front.glContext);
}

bool PresetSwitcher::requestSwitch(SwitchDirection dir)
{
    if (state_ != SwitchState::Idle) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        pendingDirection_ = dir;
        hasJob_ = true;
    }
    warmReady_ = false;
    state_ = SwitchState::Warming;
    jobCv_.notify_one();
    return true;
}

void PresetSwitcher::resize(int drawW, int drawH)
{
    SDL_GL_MakeCurrent(window_, front.glContext);
    projectm_set_window_size(front.pm, drawW, drawH);
    createOrResizeFbo(front, drawW, drawH);

    if (state_ == SwitchState::Idle) {
        SDL_SetWindowSize(backWindow_, drawW, drawH);
        SDL_GL_MakeCurrent(backWindow_, back.glContext);
        projectm_set_window_size(back.pm, drawW, drawH);
        createOrResizeFbo(back, drawW, drawH);
        SDL_GL_MakeCurrent(window_, front.glContext);
    }
    else {
        // The worker thread may currently own back.glContext -- defer until idle.
        backResizePending_ = true;
        pendingResizeW_ = drawW;
        pendingResizeH_ = drawH;
    }
}

void PresetSwitcher::renderFrame(AudioManager& audio, double dtSeconds, int drawW, int drawH)
{
    if (state_ == SwitchState::Warming && warmReady_.load()) {
        state_ = SwitchState::Blending;
        blendElapsed_ = 0.0;
        audio.setSecondaryTarget(back.pm);
    }

    if (state_ == SwitchState::Idle || state_ == SwitchState::Warming) {
        SDL_GL_MakeCurrent(window_, front.glContext);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, drawW, drawH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        projectm_opengl_render_frame(front.pm);
        return;
    }

    // Blending: let each instance render normally (to its own window's
    // framebuffer 0 -- projectM always targets that, see PresetInstance),
    // capture the result into a texture, then crossfade the two textures.
    blendElapsed_ += dtSeconds;
    float t = static_cast<float>(blendElapsed_ / blendDuration_);
    if (t > 1.0f) t = 1.0f;
    float smooth = t * t * (3.0f - 2.0f * t);

    SDL_GL_MakeCurrent(backWindow_, back.glContext);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawW, drawH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    projectm_opengl_render_frame(back.pm);
    captureFrame(back, drawW, drawH);

    SDL_GL_MakeCurrent(window_, front.glContext);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawW, drawH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    projectm_opengl_render_frame(front.pm);
    captureFrame(front, drawW, drawH);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawW, drawH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    GLboolean depthTest = GL_FALSE;
    glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(compositeProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, front.colorTex);
    glUniform1i(glGetUniformLocation(compositeProgram_, "uFront"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, back.colorTex);
    glUniform1i(glGetUniformLocation(compositeProgram_, "uBack"), 1);
    glUniform1f(glGetUniformLocation(compositeProgram_, "uT"), smooth);

    // The VAO isn't shareable across contexts and front.glContext alternates
    // between the two physical contexts every role-swap, so it's created
    // fresh here rather than cached -- the VBO/program it references are
    // shareable and remain valid regardless of which context is current.
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, compositeVbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (depthTest) {
        glEnable(GL_DEPTH_TEST);
    }

    if (t >= 1.0f) {
        audio.clearSecondaryTarget();
        std::swap(front, back);
        audio.updateHandle(front.pm);

        // Note: the two playlists' shuffle histories can drift apart since
        // each only knows about presets *it* played, not what the other
        // played while it sat in the background -- occasionally that means a
        // preset repeats sooner than shuffle would otherwise avoid. Fixing
        // that would mean loading a preset here to re-sync position, which
        // reintroduces the exact synchronous-load stall this whole feature
        // exists to eliminate, so it's left as a known, minor tradeoff.

        if (backResizePending_) {
            SDL_SetWindowSize(backWindow_, pendingResizeW_, pendingResizeH_);
            SDL_GL_MakeCurrent(backWindow_, back.glContext);
            createOrResizeFbo(back, pendingResizeW_, pendingResizeH_);
            projectm_set_window_size(back.pm, pendingResizeW_, pendingResizeH_);
            backResizePending_ = false;
        }

        // Restore front.glContext as current -- required regardless of the
        // branch above, since std::swap just changed which physical context
        // that field refers to (the one last current on this thread, from
        // compositing above, is now stored in `back`, tied to backWindow_).
        SDL_GL_MakeCurrent(window_, front.glContext);

        state_ = SwitchState::Idle;
        swapEventPending_ = true;
    }
}

bool PresetSwitcher::consumeSwapEvent()
{
    if (swapEventPending_) {
        swapEventPending_ = false;
        return true;
    }
    return false;
}

void PresetSwitcher::workerLoop()
{
    std::unique_lock<std::mutex> lock(jobMutex_);
    while (true) {
        jobCv_.wait(lock, [this] { return hasJob_ || stopWorker_; });
        if (stopWorker_) {
            break;
        }
        SwitchDirection dir = pendingDirection_;
        hasJob_ = false;
        lock.unlock();

        SDL_GL_MakeCurrent(backWindow_, back.glContext);
        switch (dir) {
        case SwitchDirection::Next:
            projectm_playlist_play_next(back.playlist, true);
            break;
        case SwitchDirection::Previous:
            projectm_playlist_play_previous(back.playlist, true);
            break;
        case SwitchDirection::Last:
            projectm_playlist_play_last(back.playlist, true);
            break;
        }

        // Force a full compile+link via an actual draw call -- some drivers
        // defer full shader validation until first use. This renders to
        // backWindow_'s own (never-presented) framebuffer, so it's invisible.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, back.fboW, back.fboH);
        for (int i = 0; i < 2; i++) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            projectm_opengl_render_frame(back.pm);
        }
        glFinish();
        SDL_GL_MakeCurrent(backWindow_, nullptr);

        warmReady_ = true;

        lock.lock();
    }
}

void PresetSwitcher::initCompositor()
{
    const char* vsSrc =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "out vec2 vUV;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "  vUV = aPos * 0.5 + 0.5;\n"
        "}\n";
    const char* fsSrc =
        "#version 330 core\n"
        "in vec2 vUV;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D uFront;\n"
        "uniform sampler2D uBack;\n"
        "uniform float uT;\n"
        "void main() {\n"
        "  vec4 a = texture(uFront, vUV);\n"
        "  vec4 b = texture(uBack, vUV);\n"
        "  FragColor = mix(a, b, uT);\n"
        "}\n";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    compositeProgram_ = glCreateProgram();
    glAttachShader(compositeProgram_, vs);
    glAttachShader(compositeProgram_, fs);
    glLinkProgram(compositeProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    float verts[] = {
        -1, -1,
         1, -1,
         1,  1,
        -1, -1,
         1,  1,
        -1,  1,
    };
    glGenBuffers(1, &compositeVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, compositeVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void PresetSwitcher::destroyCompositor()
{
    if (compositeVbo_) { glDeleteBuffers(1, &compositeVbo_); compositeVbo_ = 0; }
    if (compositeProgram_) { glDeleteProgram(compositeProgram_); compositeProgram_ = 0; }
}
