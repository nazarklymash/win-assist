#include "daemon/audio/capture.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cmath>

#include "common/log.h"
#include "common/strings.h"

using Microsoft::WRL::ComPtr;

namespace wa {

AudioCapture::AudioCapture()
    : stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), ready_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

AudioCapture::~AudioCapture() {
    abort();
    CloseHandle(stop_event_);
    CloseHandle(ready_event_);
}

bool AudioCapture::start(int max_seconds, std::string& error, std::string device_id) {
    if (thread_.joinable()) abort();
    {
        std::lock_guard lock(mutex_);
        samples_.clear();
        samples_.reserve(static_cast<size_t>(kSampleRate) * 8);
        start_error_.clear();
        device_id_ = std::move(device_id);
    }
    max_samples_ = static_cast<size_t>(kSampleRate) * static_cast<size_t>(max_seconds > 0 ? max_seconds : 30);
    level_ = 0.0f;
    ResetEvent(stop_event_);
    ResetEvent(ready_event_);
    thread_ = std::thread(&AudioCapture::run, this);
    if (WaitForSingleObject(ready_event_, 3000) != WAIT_OBJECT_0) error = "microphone did not start";
    {
        std::lock_guard lock(mutex_);
        if (!start_error_.empty()) error = start_error_;
    }
    if (error.empty()) return true;
    SetEvent(stop_event_);
    thread_.join();
    return false;
}

std::vector<float> AudioCapture::stop() {
    if (!thread_.joinable()) return {};
    SetEvent(stop_event_);
    thread_.join();
    level_ = 0.0f;
    std::lock_guard lock(mutex_);
    return std::move(samples_);
}

void AudioCapture::abort() { (void)stop(); }

void AudioCapture::run() {
    auto fail = [this](std::string message) {
        log::warn("audio capture: {}", message);
        {
            std::lock_guard lock(mutex_);
            start_error_ = std::move(message);
        }
        SetEvent(ready_event_);
    };

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com = SUCCEEDED(hr);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        HANDLE buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            std::string id;
            {
                std::lock_guard lock(mutex_);
                id = device_id_;
            }
            if (!id.empty()) {
                hr = enumerator->GetDevice(to_wide(id).c_str(), &device);
                if (FAILED(hr)) log::warn("audio: configured device missing, falling back to default");
            }
            if (!device) hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
        if (FAILED(hr) || !device) {
            fail("no microphone found");
        } else {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            format.nChannels = 1;
            format.nSamplesPerSec = kSampleRate;
            format.wBitsPerSample = 32;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = kSampleRate * 4;
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2'000'000, 0, &format, nullptr);
            if (SUCCEEDED(hr)) hr = client->SetEventHandle(buffer_event);
            if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
            if (SUCCEEDED(hr)) hr = client->Start();
            if (FAILED(hr)) {
                fail(hr == E_ACCESSDENIED ? "microphone access is blocked in Windows privacy settings"
                                          : "could not open the microphone");
            } else {
                SetEvent(ready_event_);
                HANDLE waits[2] = {stop_event_, buffer_event};
                float smoothed = 0.0f;
                for (;;) {
                    DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
                    if (wait == WAIT_OBJECT_0) break;
                    UINT32 packet = 0;
                    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD buffer_flags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &buffer_flags, nullptr, nullptr))) break;
                        const float* in = reinterpret_cast<const float*>(data);
                        double energy = 0.0;
                        {
                            std::lock_guard lock(mutex_);
                            for (UINT32 i = 0; i < frames && samples_.size() < max_samples_; ++i) {
                                float s = (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : in[i];
                                samples_.push_back(s);
                                energy += static_cast<double>(s) * s;
                            }
                        }
                        capture->ReleaseBuffer(frames);
                        if (frames > 0) {
                            float rms = static_cast<float>(std::sqrt(energy / frames));
                            float target = std::min(1.0f, rms * 8.0f);
                            smoothed = target > smoothed ? target : smoothed * 0.85f + target * 0.15f;
                            level_ = smoothed;
                        }
                    }
                }
                client->Stop();
            }
        }
        CloseHandle(buffer_event);
    }
    if (com) CoUninitialize();
}

}  // namespace wa
