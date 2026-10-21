#pragma once

#include <string>
#include <string_view>

#include "core/config.h"

struct ISpVoice;

namespace wa {

// SAPI5 text-to-speech. Lives on the UI (STA) thread; the voice is created on
// first use and released by release() once speech has finished.
class Tts {
public:
    ~Tts();
    void speak(std::wstring_view text, const TtsConfig& config);
    void stop();
    bool speaking();
    void release();

private:
    bool ensure_voice(const TtsConfig& config);

    ISpVoice* voice_ = nullptr;
    std::string voice_name_;
};

}  // namespace wa
