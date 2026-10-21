#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wa {

// Push-to-talk microphone capture through WASAPI shared mode. The device is
// opened only between start() and stop(); samples are delivered as 16 kHz
// mono float (the audio engine resamples for us).
class AudioCapture {
public:
    static constexpr int kSampleRate = 16000;

    AudioCapture();
    ~AudioCapture();

    bool start(int max_seconds, std::string& error);
    std::vector<float> stop();
    void abort();
    bool active() const { return thread_.joinable(); }
    // Smoothed input level in [0, 1] for the overlay meter.
    float level() const { return level_.load(); }

private:
    void run();

    std::thread thread_;
    HANDLE stop_event_ = nullptr;
    HANDLE ready_event_ = nullptr;
    std::mutex mutex_;
    std::vector<float> samples_;
    std::string start_error_;
    std::atomic<float> level_{0.0f};
    size_t max_samples_ = 0;
};

}  // namespace wa
