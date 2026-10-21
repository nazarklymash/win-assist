#pragma once

#include <vector>

namespace wa {

struct SpeechSpan {
    bool has_speech = false;
    size_t begin = 0;  // sample index
    size_t end = 0;
    size_t speech_ms = 0;
};

// Runs WebRTC VAD (libfvad) over 16 kHz mono float audio and returns the span
// from the first to the last voiced frame, padded by `pad_ms` on both sides.
SpeechSpan detect_speech(const std::vector<float>& pcm16k, int aggressiveness = 2, int pad_ms = 250);

}  // namespace wa
