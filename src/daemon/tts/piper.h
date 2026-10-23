#pragma once

#include <string>
#include <string_view>

#include "core/config.h"

namespace wa {

// Speaks via piper.exe when configured and present; returns false to fall back
// to SAPI. Runs asynchronously (detached process with --quiet).
bool piper_speak(std::wstring_view text, const TtsConfig& config);

bool piper_available(const TtsConfig& config);

}  // namespace wa
