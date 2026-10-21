#include "daemon/ai/ollama_models.h"

#include <windows.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/strings.h"
#include "core/config.h"

namespace wa {
namespace {

std::filesystem::path models_root() {
    wchar_t buffer[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"OLLAMA_MODELS", buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buffer;
    n = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
    return std::filesystem::path(n > 0 && n < MAX_PATH ? buffer : L"") / L".ollama" / L"models";
}

}  // namespace

std::optional<std::filesystem::path> resolve_ollama_blob(std::string_view model, std::string* error) {
    std::string name(model);
    std::string tag = "latest";
    size_t slash = name.rfind('/');
    size_t colon = name.rfind(':');
    if (colon != std::string::npos && (slash == std::string::npos || colon > slash)) {
        tag = name.substr(colon + 1);
        name = name.substr(0, colon);
    }

    std::string host = "registry.ollama.ai";
    std::string ns = "library";
    std::string repo = name;
    if (size_t first = name.find('/'); first != std::string::npos) {
        std::string head = name.substr(0, first);
        std::string rest = name.substr(first + 1);
        if (head.find('.') != std::string::npos) {
            host = head;
            size_t second = rest.find('/');
            if (second != std::string::npos) {
                ns = rest.substr(0, second);
                repo = rest.substr(second + 1);
            } else {
                repo = rest;
            }
        } else {
            ns = head;
            repo = rest;
        }
    }

    const auto root = models_root();
    const auto manifest = root / L"manifests" / to_wide(host) / to_wide(ns) / to_wide(repo) / to_wide(tag);
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        if (error) *error = "Ollama model '" + std::string(model) + "' not found (run: ollama pull " + std::string(model) + ")";
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto j = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (j.is_discarded()) {
        if (error) *error = "unreadable Ollama manifest";
        return std::nullopt;
    }
    for (const auto& layer : j.value("layers", nlohmann::json::array())) {
        if (layer.value("mediaType", "") != "application/vnd.ollama.image.model") continue;
        std::string digest = layer.value("digest", "");
        if (auto pos = digest.find(':'); pos != std::string::npos) digest[pos] = '-';
        auto blob = root / L"blobs" / to_wide(digest);
        std::error_code ec;
        if (std::filesystem::exists(blob, ec)) return blob;
        if (error) *error = "Ollama blob missing: " + to_utf8(blob.wstring());
        return std::nullopt;
    }
    if (error) *error = "Ollama manifest has no model layer";
    return std::nullopt;
}

std::optional<std::filesystem::path> resolve_gguf(const std::string& spec, std::string* error) {
    if (spec.rfind("ollama:", 0) == 0) return resolve_ollama_blob(spec.substr(7), error);
    auto path = resolve_app_path(spec);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return path;
    if (error) *error = "GGUF model not found: " + to_utf8(path.wstring());
    return std::nullopt;
}

}  // namespace wa
