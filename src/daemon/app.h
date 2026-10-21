#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/hotkey.h"
#include "daemon/ai/providers.h"
#include "daemon/ai/stt.h"
#include "daemon/assistant.h"
#include "daemon/audio/capture.h"
#include "daemon/overlay/overlay.h"
#include "daemon/plugins/host.h"
#include "daemon/tray.h"
#include "daemon/tts/sapi.h"

namespace wa {

inline constexpr wchar_t kMainWindowClass[] = L"WinAssistDaemon";
inline constexpr UINT kMsgAnotherInstance = WM_APP + 3;

// Owns the hidden message window, the state machine and every subsystem.
// Everything here runs on the UI thread unless noted.
class App {
public:
    App();
    ~App();
    int run(HINSTANCE instance);

private:
    enum class State { Idle, PttPending, Listening, Input, Processing, Responding };

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void load_config();
    void save_config();
    void apply_config();
    void reload_plugins();
    void register_hotkeys();
    void set_session_hotkeys(bool escape, bool insert);

    void on_hotkey(int id);
    void on_ptt_poll();
    void begin_capture(SessionMode mode, unsigned vk, unsigned modifiers);
    void finish_capture();
    void open_input(const std::wstring& hint = {});
    void submit_text(std::wstring text);
    void start_session(SessionRequest request);
    void on_session_event(SessionEvent* event);
    void insert_last_answer();
    void dismiss();
    void schedule_dismiss(int ms);
    void set_state(State state);
    void ensure_maintenance();
    void maintenance();

    void show_tray_menu();
    void on_menu(UINT id);
    std::wstring hotkey_label(const std::string& text) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;

    std::filesystem::path config_path_;
    Config config_;
    std::mutex config_json_mutex_;
    std::string config_json_;

    Tray tray_;
    Overlay overlay_;
    AudioCapture audio_;
    Tts tts_;
    std::unique_ptr<ProviderHub> providers_;
    std::unique_ptr<SttClient> stt_;
    std::unique_ptr<PluginHost> plugins_;
    std::unique_ptr<Assistant> assistant_;
    std::thread warm_thread_;
    std::thread inject_thread_;

    State state_ = State::Idle;
    SessionMode mode_ = SessionMode::Assistant;
    TargetInfo target_;
    unsigned ptt_vk_ = 0;
    std::chrono::steady_clock::time_point press_time_{};
    OverlayView view_;
    bool session_voice_ = false;
    uint64_t session_id_ = 0;
    std::string last_answer_;
    bool escape_registered_ = false;
    bool insert_registered_ = false;
    bool maintenance_running_ = false;
    std::vector<std::string> menu_models_;
};

}  // namespace wa
