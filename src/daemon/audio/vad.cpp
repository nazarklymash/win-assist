#include "daemon/audio/vad.h"

extern "C" {
#include <fvad.h>
}

#include <algorithm>
#include <cstdint>

namespace wa {

SpeechSpan detect_speech(const std::vector<float>& pcm, int aggressiveness, int pad_ms) {
    constexpr int kRate = 16000;
    constexpr size_t kFrame = kRate * 30 / 1000;  // 30 ms
    SpeechSpan span;
    Fvad* vad = fvad_new();
    if (!vad) return span;
    fvad_set_sample_rate(vad, kRate);
    fvad_set_mode(vad, std::clamp(aggressiveness, 0, 3));

    int16_t frame[kFrame];
    size_t first = SIZE_MAX;
    size_t last = 0;
    size_t voiced = 0;
    for (size_t offset = 0; offset + kFrame <= pcm.size(); offset += kFrame) {
        for (size_t i = 0; i < kFrame; ++i) {
            float s = std::clamp(pcm[offset + i], -1.0f, 1.0f);
            frame[i] = static_cast<int16_t>(s * 32767.0f);
        }
        if (fvad_process(vad, frame, kFrame) == 1) {
            ++voiced;
            if (first == SIZE_MAX) first = offset;
            last = offset + kFrame;
        }
    }
    fvad_free(vad);

    span.speech_ms = voiced * 30;
    // Require ~150 ms of voiced frames to filter clicks and breaths.
    if (voiced < 5) return span;
    const size_t pad = static_cast<size_t>(kRate) * static_cast<size_t>(pad_ms) / 1000;
    span.has_speech = true;
    span.begin = first > pad ? first - pad : 0;
    span.end = std::min(pcm.size(), last + pad);
    return span;
}

}  // namespace wa
