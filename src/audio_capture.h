#pragma once

#include <projectM-4/projectM.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

#include <SDL2/SDL.h>

enum class AudioSource {
    WASAPI_LOOPBACK,
    SDL_CAPTURE
};

#ifdef _WIN32
class WasapiLoopback {
public:
    WasapiLoopback();
    ~WasapiLoopback();

    bool init();
    void shutdown();
    void captureFrame(projectm_handle pm);
    bool isInitialized() const { return initialized_; }

private:
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    bool initialized_ = false;
    bool comInitialized_ = false;
};
#endif

class SdlAudioCapture {
public:
    SdlAudioCapture();
    ~SdlAudioCapture();

    bool init(projectm_handle pm, int deviceIndex = -1);
    void shutdown();
    void start();
    void stop();
    void updateHandle(projectm_handle pm);
    int getDeviceCount() const;
    const char* getDeviceName(int index) const;
    int getCurrentDeviceIndex() const { return currentDevice_; }
    bool isActive() const { return active_; }

private:
    static void audioCallback(void* userdata, Uint8* stream, int len);

    SDL_AudioDeviceID deviceId_ = 0;
    projectm_handle pm_ = nullptr;
    int currentDevice_ = -1;
    bool active_ = false;
};

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    bool init(projectm_handle pm, const std::string& requestedSource = "");
    void shutdown();
    void processFrame();
    void toggleSource();
    void updateHandle(projectm_handle pm);
    std::string getCurrentSourceName() const;
    AudioSource getCurrentSource() const { return currentSource_; }
    static void printAudioDevices();

private:
    projectm_handle pm_ = nullptr;
#ifdef _WIN32
    WasapiLoopback wasapi_;
#endif
    SdlAudioCapture sdlCapture_;
    AudioSource currentSource_ = AudioSource::WASAPI_LOOPBACK;
};
