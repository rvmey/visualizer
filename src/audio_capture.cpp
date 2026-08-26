#include "audio_capture.h"
#include <projectM-4/audio.h>

#ifdef _WIN32
#include <functiondiscoverykeys_devpkey.h>

WasapiLoopback::WasapiLoopback() = default;

WasapiLoopback::~WasapiLoopback()
{
    shutdown();
}

bool WasapiLoopback::init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
        comInitialized_ = (hr != RPC_E_CHANGED_MODE);
    }
    else {
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    enumerator->Release();
    if (FAILED(hr)) {
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audioClient_));
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->GetMixFormat(&mixFormat_);
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  0, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&captureClient_));
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->Start();
    if (FAILED(hr)) {
        return false;
    }

    initialized_ = true;
    return true;
}

void WasapiLoopback::shutdown()
{
    if (audioClient_) {
        audioClient_->Stop();
    }
    if (captureClient_) {
        captureClient_->Release();
        captureClient_ = nullptr;
    }
    if (audioClient_) {
        audioClient_->Release();
        audioClient_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    initialized_ = false;
}

void WasapiLoopback::captureFrame(projectm_handle pm, projectm_handle secondary)
{
    if (!initialized_ || !captureClient_) {
        return;
    }

    UINT32 nextPacketSize = 0;
    HRESULT hr = captureClient_->GetNextPacketSize(&nextPacketSize);

    while (SUCCEEDED(hr) && nextPacketSize > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;

        hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            break;
        }

        if (data && numFrames > 0) {
            projectm_pcm_add_float(pm, reinterpret_cast<float*>(data),
                                   numFrames, PROJECTM_STEREO);
            if (secondary) {
                projectm_pcm_add_float(secondary, reinterpret_cast<float*>(data),
                                       numFrames, PROJECTM_STEREO);
            }
        }

        captureClient_->ReleaseBuffer(numFrames);
        hr = captureClient_->GetNextPacketSize(&nextPacketSize);
    }
}
#endif // _WIN32

// --- SDL Audio Capture ---

SdlAudioCapture::SdlAudioCapture() = default;

SdlAudioCapture::~SdlAudioCapture()
{
    shutdown();
}

bool SdlAudioCapture::init(projectm_handle pm, int deviceIndex)
{
    shutdown();

    pm_ = pm;
    currentDevice_ = deviceIndex;

    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 44100 / 60;
    want.callback = audioCallback;
    want.userdata = this;

    const char* deviceName = nullptr;
    if (deviceIndex >= 0 && deviceIndex < getDeviceCount()) {
        deviceName = SDL_GetAudioDeviceName(deviceIndex, SDL_TRUE);
    }

    SDL_AudioSpec have{};
    deviceId_ = SDL_OpenAudioDevice(deviceName, SDL_TRUE, &want, &have, 0);
    if (deviceId_ == 0) {
        SDL_Log("Failed to open audio capture device: %s", SDL_GetError());
        return false;
    }

    return true;
}

void SdlAudioCapture::shutdown()
{
    if (deviceId_ != 0) {
        SDL_CloseAudioDevice(deviceId_);
        deviceId_ = 0;
    }
    active_ = false;
    pm_ = nullptr;
    secondary_ = nullptr;
}

void SdlAudioCapture::start()
{
    if (deviceId_ != 0) {
        SDL_PauseAudioDevice(deviceId_, 0);
        active_ = true;
    }
}

void SdlAudioCapture::stop()
{
    if (deviceId_ != 0) {
        SDL_PauseAudioDevice(deviceId_, 1);
        active_ = false;
    }
}

void SdlAudioCapture::updateHandle(projectm_handle pm)
{
    // The audio callback runs on a separate thread; lock to avoid using a
    // stale handle while it's being swapped.
    if (deviceId_ != 0) {
        SDL_LockAudioDevice(deviceId_);
    }
    pm_ = pm;
    if (deviceId_ != 0) {
        SDL_UnlockAudioDevice(deviceId_);
    }
}

void SdlAudioCapture::setSecondary(projectm_handle pm)
{
    if (deviceId_ != 0) {
        SDL_LockAudioDevice(deviceId_);
    }
    secondary_ = pm;
    if (deviceId_ != 0) {
        SDL_UnlockAudioDevice(deviceId_);
    }
}

int SdlAudioCapture::getDeviceCount() const
{
    return SDL_GetNumAudioDevices(SDL_TRUE);
}

const char* SdlAudioCapture::getDeviceName(int index) const
{
    return SDL_GetAudioDeviceName(index, SDL_TRUE);
}

void SdlAudioCapture::audioCallback(void* userdata, Uint8* stream, int len)
{
    auto* self = static_cast<SdlAudioCapture*>(userdata);
    if (!self->pm_) {
        return;
    }
    unsigned int sampleFrames = static_cast<unsigned int>(len) / sizeof(float) / 2;
    projectm_pcm_add_float(self->pm_, reinterpret_cast<float*>(stream),
                           sampleFrames, PROJECTM_STEREO);
    if (self->secondary_) {
        projectm_pcm_add_float(self->secondary_, reinterpret_cast<float*>(stream),
                               sampleFrames, PROJECTM_STEREO);
    }
}

// --- Audio Manager ---

AudioManager::AudioManager() = default;

AudioManager::~AudioManager()
{
    shutdown();
}

static int findSdlDeviceByName(const SdlAudioCapture& sdl, const std::string& name)
{
    int count = sdl.getDeviceCount();
    for (int i = 0; i < count; i++) {
        const char* devName = sdl.getDeviceName(i);
        if (devName && name == devName)
            return i;
    }
    for (int i = 0; i < count; i++) {
        const char* devName = sdl.getDeviceName(i);
        if (devName) {
            std::string dn(devName);
            if (dn.find(name) != std::string::npos)
                return i;
        }
    }
    return -1;
}

bool AudioManager::init(projectm_handle pm, const std::string& requestedSource)
{
    pm_ = pm;

    if (requestedSource == "mic") {
        if (sdlCapture_.init(pm, -1)) {
            sdlCapture_.start();
            currentSource_ = AudioSource::SDL_CAPTURE;
            return true;
        }
        SDL_Log("Requested mic capture failed");
        return false;
    }

    if (!requestedSource.empty() && requestedSource != "loopback") {
        int idx = -1;
        try { idx = std::stoi(requestedSource); } catch (...) {}
        if (idx < 0)
            idx = findSdlDeviceByName(sdlCapture_, requestedSource);
        if (idx >= 0 && sdlCapture_.init(pm, idx)) {
            sdlCapture_.start();
            currentSource_ = AudioSource::SDL_CAPTURE;
            return true;
        }
        SDL_Log("Requested audio device \"%s\" not found or failed to open", requestedSource.c_str());
        return false;
    }

#ifdef _WIN32
    if (wasapi_.init()) {
        currentSource_ = AudioSource::WASAPI_LOOPBACK;
        return true;
    }
    SDL_Log("WASAPI loopback init failed, falling back to SDL audio capture");
#endif

    if (sdlCapture_.init(pm, -1)) {
        sdlCapture_.start();
        currentSource_ = AudioSource::SDL_CAPTURE;
        return true;
    }

    SDL_Log("No audio capture source available");
    return false;
}

void AudioManager::printAudioDevices()
{
    std::printf("Available audio sources:\n");
#ifdef _WIN32
    std::printf("  %-6s %s\n", "", "loopback  - System Audio (WASAPI Loopback) [default]");
#endif
    std::printf("  %-6s %s\n", "", "mic       - Default microphone input");
    int count = SDL_GetNumAudioDevices(SDL_TRUE);
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        if (name)
            std::printf("  %-6d %s\n", i, name);
    }
}

void AudioManager::shutdown()
{
#ifdef _WIN32
    wasapi_.shutdown();
#endif
    sdlCapture_.shutdown();
}

void AudioManager::processFrame()
{
#ifdef _WIN32
    if (currentSource_ == AudioSource::WASAPI_LOOPBACK) {
        wasapi_.captureFrame(pm_, secondary_);
    }
#endif
}

void AudioManager::toggleSource()
{
#ifdef _WIN32
    if (currentSource_ == AudioSource::WASAPI_LOOPBACK) {
        wasapi_.shutdown();
        if (sdlCapture_.init(pm_, -1)) {
            sdlCapture_.start();
            currentSource_ = AudioSource::SDL_CAPTURE;
        }
        else {
            wasapi_.init();
        }
    }
    else {
        sdlCapture_.shutdown();
        if (wasapi_.init()) {
            currentSource_ = AudioSource::WASAPI_LOOPBACK;
        }
        else {
            sdlCapture_.init(pm_, -1);
            sdlCapture_.start();
        }
    }
#else
    int count = sdlCapture_.getDeviceCount();
    int next = (sdlCapture_.getCurrentDeviceIndex() + 1) % (count > 0 ? count : 1);
    sdlCapture_.shutdown();
    sdlCapture_.init(pm_, next);
    sdlCapture_.start();
#endif
}

void AudioManager::updateHandle(projectm_handle pm)
{
    pm_ = pm;
    sdlCapture_.updateHandle(pm);
}

void AudioManager::setSecondaryTarget(projectm_handle pm)
{
    secondary_ = pm;
    sdlCapture_.setSecondary(pm);
}

void AudioManager::clearSecondaryTarget()
{
    secondary_ = nullptr;
    sdlCapture_.setSecondary(nullptr);
}

std::string AudioManager::getCurrentSourceName() const
{
    if (currentSource_ == AudioSource::WASAPI_LOOPBACK) {
        return "System Audio (WASAPI Loopback)";
    }
    int idx = sdlCapture_.getCurrentDeviceIndex();
    const char* name = (idx >= 0) ? sdlCapture_.getDeviceName(idx) : nullptr;
    if (name) {
        return std::string("Mic: ") + name;
    }
    return "Mic: Default";
}
