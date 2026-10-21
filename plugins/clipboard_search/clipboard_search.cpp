// Native plugin: in-memory clipboard history with keyword search and optional
// semantic search through Ollama embeddings. Nothing is written to disk.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <winassist/plugin_api.h>

using nlohmann::json;

namespace {

constexpr size_t kMaxEntries = 50;
constexpr size_t kMaxChars = 10000;
constexpr wchar_t kWindowClass[] = L"WinAssistClipboardListener";

const wa_host_api* g_host = nullptr;
HWND g_window = nullptr;
std::mutex g_mutex;
std::deque<std::wstring> g_history;  // newest first

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring preview(const std::wstring& s, size_t max = 140) {
    std::wstring out = s.substr(0, max);
    for (auto& c : out) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    }
    if (s.size() > max) out += L"\u2026";
    return out;
}

void capture_clipboard() {
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT ignore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    if (IsClipboardFormatAvailable(exclude) || IsClipboardFormatAvailable(ignore)) return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(g_window)) return;
    std::wstring text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
            text.assign(p, wcsnlen(p, kMaxChars));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
    std::lock_guard lock(g_mutex);
    g_history.erase(std::remove(g_history.begin(), g_history.end(), text), g_history.end());
    g_history.push_front(std::move(text));
    if (g_history.size() > kMaxEntries) g_history.pop_back();
}

LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        capture_clipboard();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool put_on_clipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

std::string reply(bool ok, const std::string& display) {
    return json{{"status", ok ? "ok" : "error"}, {"display", display}}.dump();
}

// Returns embeddings for each input via the host's Ollama bridge, or empty.
std::vector<std::vector<float>> embed(const std::vector<std::string>& inputs) {
    if (!g_host || !g_host->ollama_post || !g_host->config_json) return {};
    char* cfg_raw = g_host->config_json(g_host->host);
    json cfg = json::parse(cfg_raw ? cfg_raw : "{}", nullptr, false);
    if (cfg_raw) g_host->free_string(g_host->host, cfg_raw);
    std::string model = "nomic-embed-text";
    if (!cfg.is_discarded()) model = cfg.value("/ollama/embed_model"_json_pointer, model);
    const std::string body = json{{"model", model}, {"input", inputs}}.dump();
    char* raw = g_host->ollama_post(g_host->host, "/api/embed", body.c_str());
    if (!raw) return {};
    json j = json::parse(raw, nullptr, false);
    g_host->free_string(g_host->host, raw);
    if (j.is_discarded() || !j.contains("embeddings")) return {};
    return j["embeddings"].get<std::vector<std::vector<float>>>();
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    return na > 0 && nb > 0 ? static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb))) : 0.0f;
}

std::string search(const std::string& query_utf8) {
    std::vector<std::wstring> items;
    {
        std::lock_guard lock(g_mutex);
        items.assign(g_history.begin(), g_history.end());
    }
    if (items.empty()) return reply(false, "Clipboard history is empty (it only covers text copied since win-assist started).");
    const std::wstring query = lower(to_wide(query_utf8));

    int best = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        if (lower(items[i]).find(query) != std::wstring::npos) {
            best = static_cast<int>(i);
            break;
        }
    }
    std::string how = "match";
    if (best < 0) {
        std::vector<std::string> inputs = {query_utf8};
        for (const auto& item : items) inputs.push_back(to_utf8(item.substr(0, 2000)));
        auto vectors = embed(inputs);
        if (vectors.size() == inputs.size()) {
            float best_score = 0.35f;
            for (size_t i = 1; i < vectors.size(); ++i) {
                float score = cosine(vectors[0], vectors[i]);
                if (score > best_score) {
                    best_score = score;
                    best = static_cast<int>(i - 1);
                }
            }
            how = "closest match";
        }
    }
    if (best < 0) return reply(false, "Nothing in your clipboard history matches \"" + query_utf8 + "\".");
    put_on_clipboard(items[static_cast<size_t>(best)]);
    return reply(true, "Copied " + how + " to clipboard: " + to_utf8(preview(items[static_cast<size_t>(best)])));
}

std::string list_history() {
    std::lock_guard lock(g_mutex);
    if (g_history.empty()) return reply(true, "Clipboard history is empty.");
    std::string text = "Recent clipboard items:";
    for (size_t i = 0; i < g_history.size() && i < 5; ++i) text += "\n" + std::to_string(i + 1) + ". " + to_utf8(preview(g_history[i], 90));
    return reply(true, text);
}

std::string handle(const std::string& request_json) {
    json request = json::parse(request_json, nullptr, false);
    if (request.is_discarded()) return reply(false, "Bad request.");
    const std::string intent = request.value("intent", "");
    const json params = request.value("parameters", json::object());
    if (intent == "search_clipboard") return search(params.value("query", ""));
    if (intent == "clipboard_history") return list_history();
    if (intent == "clear_clipboard_history") {
        std::lock_guard lock(g_mutex);
        g_history.clear();
        return reply(true, "Clipboard history cleared.");
    }
    return reply(false, "Unknown intent '" + intent + "'.");
}

const wa_plugin_info kInfo = {WA_PLUGIN_API_VERSION, "clipboard_search", "1.0.0"};

}  // namespace

extern "C" {

WA_PLUGIN_EXPORT const wa_plugin_info* wa_plugin_get_info(void) { return &kInfo; }

WA_PLUGIN_EXPORT int wa_plugin_init(const wa_host_api* host) {
    g_host = host;
    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&listener_proc), &instance);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = listener_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    g_window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!g_window || !AddClipboardFormatListener(g_window)) return 1;
    return 0;
}

WA_PLUGIN_EXPORT void wa_plugin_shutdown(void) {
    if (g_window) {
        RemoveClipboardFormatListener(g_window);
        DestroyWindow(g_window);
        g_window = nullptr;
    }
    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&listener_proc), &instance);
    UnregisterClassW(kWindowClass, instance);
    std::lock_guard lock(g_mutex);
    g_history.clear();
}

WA_PLUGIN_EXPORT char* wa_plugin_invoke(const char* request_json) {
    std::string out = handle(request_json ? request_json : "");
    char* buffer = static_cast<char*>(malloc(out.size() + 1));
    if (buffer) memcpy(buffer, out.c_str(), out.size() + 1);
    return buffer;
}

WA_PLUGIN_EXPORT void wa_plugin_free(char* response_json) { free(response_json); }

}  // extern "C"
