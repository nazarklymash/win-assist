#include "daemon/tts/sapi.h"

#include <windows.h>
#include <sapi.h>

#include "common/log.h"
#include "common/strings.h"

namespace wa {

Tts::~Tts() { release(); }

bool Tts::ensure_voice(const TtsConfig& config) {
    if (voice_ && voice_name_ == config.voice) return true;
    release();
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice_)))) {
        log::warn("SAPI voice unavailable");
        voice_ = nullptr;
        return false;
    }
    voice_name_ = config.voice;
    if (!config.voice.empty()) {
        ISpObjectTokenCategory* category = nullptr;
        IEnumSpObjectTokens* tokens = nullptr;
        ISpObjectToken* token = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                       reinterpret_cast<void**>(&category))) &&
            SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE))) {
            const std::wstring filter = L"Name=" + to_wide(config.voice);
            if (SUCCEEDED(category->EnumTokens(filter.c_str(), nullptr, &tokens)) && tokens->Next(1, &token, nullptr) == S_OK) {
                voice_->SetVoice(token);
            } else {
                log::warn("TTS voice '{}' not found, using default", config.voice);
            }
        }
        if (token) token->Release();
        if (tokens) tokens->Release();
        if (category) category->Release();
    }
    return true;
}

void Tts::speak(std::wstring_view text, const TtsConfig& config) {
    if (text.empty() || !ensure_voice(config)) return;
    voice_->SetRate(config.rate < -10 ? -10 : config.rate > 10 ? 10 : config.rate);
    std::wstring copy(text);
    voice_->Speak(copy.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
}

void Tts::stop() {
    if (voice_) voice_->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
}

bool Tts::speaking() {
    if (!voice_) return false;
    SPVOICESTATUS status{};
    if (FAILED(voice_->GetStatus(&status, nullptr))) return false;
    return status.dwRunningState == SPRS_IS_SPEAKING;
}

void Tts::release() {
    if (voice_) {
        voice_->Release();
        voice_ = nullptr;
    }
    voice_name_.clear();
}

}  // namespace wa
