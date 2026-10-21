#include "daemon/app.h"

#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>

#include "common/log.h"
#include "common/paths.h"
#include "common/strings.h"
#include "daemon/system/autostart.h"
#include "daemon/system/inject.h"

namespace wa {
namespace {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgSession = WM_APP + 2;

constexpr int kHotkeyAssistant = 1;
constexpr int kHotkeyDictation = 2;
constexpr int kHotkeyEscape = 3;
constexpr int kHotkeyInsert = 4;

constexpr UINT_PTR kTimerPoll = 10;
constexpr UINT_PTR kTimerDismiss = 11;
constexpr UINT_PTR kTimerMaintenance = 12;

enum MenuId : UINT {
    kMenuStatus = 100,
    kMenuBackend,
    kMenuProviderAuto = 110,
    kMenuProviderOllama,
    kMenuProviderLlama,
    kMenuSpeak = 120,
    kMenuAutostart,
    kMenuAsk,
    kMenuOpenConfig = 130,
    kMenuReload,
    kMenuOpenPlugins,
    kMenuOpenLogs,
    kMenuQuit = 199,
    kMenuModelBase = 200,
};

void open_path(const std::filesystem::path& p) {
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

App::App() = default;

App::~App() {
    if (assistant_) {
        assistant_->cancel();
        assistant_->join();
    }
    if (warm_thread_.joinable()) warm_thread_.join();
    if (inject_thread_.joinable()) inject_thread_.join();
}

std::wstring App::hotkey_label(const std::string& text) const {
    auto spec = parse_hotkey(text);
    return spec ? to_wide(format_hotkey(*spec)) : to_wide(text);
}

void App::load_config() {
    std::string error;
    config_ = load_or_create_config(config_path_, &error);
    if (!error.empty()) {
        log::error("config: {}", error);
        tray_.notify(L"win-assist config error", to_wide(error));
    }
    std::lock_guard lock(config_json_mutex_);
    config_json_ = nlohmann::json(config_).dump();
}

void App::save_config() {
    std::ofstream out(config_path_, std::ios::binary | std::ios::trunc);
    out << nlohmann::json(config_).dump(2) << '\n';
    std::lock_guard lock(config_json_mutex_);
    config_json_ = nlohmann::json(config_).dump();
}

void App::apply_config() {
    providers_->apply_config(config_);
    stt_->set_config(config_.stt);
    overlay_.configure(config_.overlay.width, config_.overlay.position == "top");
    register_hotkeys();
    tray_.set_tooltip(L"win-assist \u2014 hold " + hotkey_label(config_.hotkeys.assistant) + L" to talk, tap to type");
}

void App::reload_plugins() {
    std::vector<std::filesystem::path> dirs = {paths::exe_dir() / L"plugins", paths::app_data() / L"plugins"};
    for (const auto& d : config_.plugin_dirs) dirs.push_back(resolve_app_path(d));
    std::vector<std::string> errors;
    plugins_->load(dirs, builtin_manifests(), errors);
    if (!errors.empty()) {
        tray_.notify(L"win-assist plugins", to_wide(std::to_string(errors.size()) + " plugin problem(s); see the log."));
    }
}

void App::register_hotkeys() {
    UnregisterHotKey(hwnd_, kHotkeyAssistant);
    UnregisterHotKey(hwnd_, kHotkeyDictation);
    std::wstring failures;
    auto reg = [&](int id, const std::string& text) {
        auto spec = parse_hotkey(text);
        if (!spec) {
            failures += L"Invalid hotkey '" + to_wide(text) + L"'. ";
            return;
        }
        if (!RegisterHotKey(hwnd_, id, spec->modifiers | MOD_NOREPEAT, spec->vk)) {
            failures += hotkey_label(text) + L" is already used by another app. ";
        }
    };
    reg(kHotkeyAssistant, config_.hotkeys.assistant);
    if (!config_.hotkeys.dictation.empty()) reg(kHotkeyDictation, config_.hotkeys.dictation);
    if (!failures.empty()) {
        log::warn("hotkeys: {}", to_utf8(failures));
        tray_.notify(L"win-assist hotkeys", failures + L"Change it in the config.");
    }
}

void App::set_session_hotkeys(bool escape, bool insert) {
    if (escape != escape_registered_) {
        if (escape) escape_registered_ = RegisterHotKey(hwnd_, kHotkeyEscape, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
        else {
            UnregisterHotKey(hwnd_, kHotkeyEscape);
            escape_registered_ = false;
        }
    }
    if (insert != insert_registered_) {
        if (insert) insert_registered_ = RegisterHotKey(hwnd_, kHotkeyInsert, MOD_ALT | MOD_NOREPEAT, VK_RETURN) != FALSE;
        else {
            UnregisterHotKey(hwnd_, kHotkeyInsert);
            insert_registered_ = false;
        }
    }
}

void App::set_state(State state) {
    static constexpr const char* kNames[] = {"idle", "ptt-pending", "listening", "input", "processing", "responding"};
    if (state != state_) log::info("state: {} -> {}", kNames[static_cast<int>(state_)], kNames[static_cast<int>(state)]);
    state_ = state;
    switch (state) {
        case State::Idle: tray_.set_state(TrayState::Idle); break;
        case State::PttPending:
        case State::Listening: tray_.set_state(TrayState::Listening); break;
        case State::Input:
        case State::Responding: tray_.set_state(TrayState::Idle); break;
        case State::Processing: tray_.set_state(TrayState::Busy); break;
    }
    const bool overlay_up = state == State::Listening || state == State::Processing || state == State::Responding;
    set_session_hotkeys(overlay_up, state == State::Responding && !last_answer_.empty());
}

void App::on_hotkey(int id) {
    if (id == kHotkeyEscape) {
        dismiss();
        return;
    }
    if (id == kHotkeyInsert) {
        insert_last_answer();
        return;
    }
    // While capturing, the trigger is still physically held, so any further
    // hotkey message is keyboard auto-repeat (registering the Esc hotkey
    // resets MOD_NOREPEAT tracking).
    if (state_ == State::PttPending || state_ == State::Listening) return;
    if (state_ != State::Idle) {
        // Pressing the trigger again while the assistant is up closes it.
        dismiss();
        return;
    }
    auto spec = parse_hotkey(id == kHotkeyAssistant ? config_.hotkeys.assistant : config_.hotkeys.dictation);
    if (!spec) return;
    begin_capture(id == kHotkeyAssistant ? SessionMode::Assistant : SessionMode::Dictation, spec->vk, spec->modifiers);
}

void App::begin_capture(SessionMode mode, unsigned vk, unsigned modifiers) {
    target_ = capture_target();
    if (target_.window == overlay_.hwnd()) target_ = {};
    mode_ = mode;
    ptt_vk_ = vk;
    press_time_ = std::chrono::steady_clock::now();
    last_answer_.clear();
    if (modifiers & (MOD_ALT | MOD_WIN)) send_menu_mask_key();

    std::string error;
    if (!audio_.start(config_.stt.max_record_s, error)) {
        if (mode == SessionMode::Assistant) {
            open_input(L"Microphone unavailable (" + to_wide(error) + L"). Type instead.");
        } else {
            view_ = {OverlayView::Mode::Error, L"Dictation", L"Microphone unavailable: " + to_wide(error), L""};
            overlay_.show(view_, target_.window);
            set_state(State::Responding);
            schedule_dismiss(3500);
        }
        return;
    }
    set_state(State::PttPending);
    SetTimer(hwnd_, kTimerPoll, 30, nullptr);
}

void App::on_ptt_poll() {
    const bool down = (GetAsyncKeyState(static_cast<int>(ptt_vk_)) & 0x8000) != 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - press_time_);
    overlay_.set_level(audio_.level());

    if (state_ == State::PttPending) {
        if (down && elapsed.count() >= config_.hotkeys.tap_threshold_ms) {
            const bool dictation = mode_ == SessionMode::Dictation;
            view_ = {OverlayView::Mode::Listening, dictation ? L"Dictating" : L"Listening",
                     L"", L"Release " + hotkey_label(dictation ? config_.hotkeys.dictation : config_.hotkeys.assistant) +
                              L" when you're done"};
            overlay_.show(view_, target_.window);
            set_state(State::Listening);
            // Load whisper while the user is still talking.
            if (warm_thread_.joinable()) warm_thread_.join();
            warm_thread_ = std::thread([stt = stt_.get()] { stt->warm_up(); });
        } else if (!down) {
            KillTimer(hwnd_, kTimerPoll);
            audio_.abort();
            if (mode_ == SessionMode::Assistant) {
                open_input();
            } else {
                view_ = {OverlayView::Mode::Error, L"Dictation",
                         L"Hold " + hotkey_label(config_.hotkeys.dictation) + L" while you speak.", L""};
                overlay_.show(view_, target_.window);
                set_state(State::Responding);
                schedule_dismiss(2500);
            }
        }
        return;
    }
    if (state_ == State::Listening) {
        if (!down || elapsed.count() >= config_.stt.max_record_s * 1000LL) finish_capture();
        return;
    }
    KillTimer(hwnd_, kTimerPoll);
}

void App::finish_capture() {
    KillTimer(hwnd_, kTimerPoll);
    std::vector<float> samples = audio_.stop();
    view_.mode = OverlayView::Mode::Processing;
    view_.heading = mode_ == SessionMode::Dictation ? L"Dictating" : L"Listening";
    view_.body.clear();
    view_.hint = L"Transcribing\u2026";
    overlay_.update(view_);
    set_state(State::Processing);

    SessionRequest request;
    request.mode = mode_;
    request.voice = true;
    request.audio = std::move(samples);
    start_session(std::move(request));
}

void App::open_input(const std::wstring& hint) {
    mode_ = SessionMode::Assistant;
    view_ = {OverlayView::Mode::Input, L"Ask win-assist", L"",
             hint.empty() ? L"Enter to send \u00B7 Esc to close \u00B7 hold " + hotkey_label(config_.hotkeys.assistant) +
                                L" to talk"
                          : hint};
    set_state(State::Input);
    overlay_.show(view_, target_.window);
}

void App::submit_text(std::wstring text) {
    if (state_ != State::Input) return;
    view_ = {OverlayView::Mode::Processing, L"\u201C" + text + L"\u201D", L"", L"Thinking\u2026"};
    overlay_.update(view_);
    set_state(State::Processing);
    SessionRequest request;
    request.mode = SessionMode::Assistant;
    request.text = to_utf8(text);
    start_session(std::move(request));
}

void App::start_session(SessionRequest request) {
    session_voice_ = request.voice;
    request.id = ++session_id_;
    request.target = target_;
    request.config = config_;
    assistant_->start(std::move(request));
}

void App::on_session_event(SessionEvent* raw) {
    std::unique_ptr<SessionEvent> event(raw);
    const bool terminal = event->kind == SessionEvent::Kind::Done || event->kind == SessionEvent::Kind::Error;
    if (terminal) {
        assistant_->join();
        ensure_maintenance();
    }
    if (event->session != session_id_ || state_ != State::Processing) return;

    switch (event->kind) {
        case SessionEvent::Kind::Transcript:
            view_.heading = L"\u201C" + to_wide(event->text) + L"\u201D";
            break;
        case SessionEvent::Kind::Status:
            view_.hint = to_wide(event->text);
            break;
        case SessionEvent::Kind::Reset:
            view_.body.clear();
            break;
        case SessionEvent::Kind::Delta:
            view_.body += to_wide(event->text);
            break;
        case SessionEvent::Kind::Done:
        case SessionEvent::Kind::Error: {
            const ActionResult& r = event->result;
            last_answer_ = event->answer;
            view_.mode = r.ok ? OverlayView::Mode::Response : OverlayView::Mode::Error;
            if (!r.display.empty()) view_.body = to_wide(r.display);
            if (event->injected) {
                view_.hint = L"Inserted into " + (target_.process.empty() ? L"the active window" : target_.process);
            } else if (!last_answer_.empty()) {
                view_.hint = L"Alt+Enter to insert \u00B7 Esc to close";
            } else {
                view_.hint = L"Esc to close";
            }
            overlay_.update(view_);
            set_state(State::Responding);

            const bool speak = r.ok && !r.speech.empty() && config_.tts.enabled &&
                               (session_voice_ || config_.tts.speak_typed_answers);
            if (speak) tts_.speak(to_wide(r.speech), config_.tts);
            int ms;
            if (event->injected && mode_ == SessionMode::Dictation) ms = 1200;
            else if (!r.ok) ms = 4500;
            else ms = std::clamp(2500 + static_cast<int>(view_.body.size()) * 45, config_.overlay.min_display_ms, 30000);
            schedule_dismiss(ms);
            return;
        }
    }
    overlay_.update(view_);
}

void App::insert_last_answer() {
    if (last_answer_.empty()) return;
    std::wstring text = to_wide(last_answer_);
    TargetInfo target = target_;
    send_menu_mask_key();
    dismiss();
    if (inject_thread_.joinable()) inject_thread_.join();
    inject_thread_ = std::thread([target, text] {
        std::string error;
        if (!inject_text(target, text, error)) log::warn("insert failed: {}", error);
    });
}

void App::schedule_dismiss(int ms) { SetTimer(hwnd_, kTimerDismiss, static_cast<UINT>(ms), nullptr); }

void App::dismiss() {
    KillTimer(hwnd_, kTimerPoll);
    KillTimer(hwnd_, kTimerDismiss);
    if (audio_.active()) audio_.abort();
    if (assistant_->busy()) assistant_->cancel();
    tts_.stop();
    overlay_.hide();
    last_answer_.clear();
    set_state(State::Idle);
    ensure_maintenance();
}

void App::ensure_maintenance() {
    if (maintenance_running_) return;
    maintenance_running_ = true;
    SetTimer(hwnd_, kTimerMaintenance, 10000, nullptr);
}

void App::maintenance() {
    bool alive = stt_->idle_tick();
    alive |= providers_->idle_tick();
    if (!overlay_.visible()) {
        if (overlay_.hidden_for(std::chrono::seconds(20))) overlay_.release_resources();
        else alive = true;
    }
    if (state_ == State::Idle && !tts_.speaking()) tts_.release();
    if (state_ != State::Idle || assistant_->busy()) alive = true;
    if (!alive) {
        KillTimer(hwnd_, kTimerMaintenance);
        maintenance_running_ = false;
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }
}

void App::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    std::wstring status = L"Hold " + hotkey_label(config_.hotkeys.assistant) + L" to talk, tap to type";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, status.c_str());

    std::string error;
    LlmProvider* provider = providers_->select(false, error);
    std::wstring backend = provider ? L"LLM: " + to_wide(provider->name()) +
                                          (provider == &providers_->ollama() ? L" (" + to_wide(config_.ollama.model) + L")" : L"")
                                    : L"LLM: unavailable";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuBackend, backend.c_str());
    AppendMenuW(menu, MF_STRING, kMenuAsk, L"Ask\u2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU backend_menu = CreatePopupMenu();
    AppendMenuW(backend_menu, MF_STRING | (config_.llm.provider == "auto" ? MF_CHECKED : 0), kMenuProviderAuto,
                L"Auto (Ollama, then llama.cpp)");
    AppendMenuW(backend_menu, MF_STRING | (config_.llm.provider == "ollama" ? MF_CHECKED : 0), kMenuProviderOllama,
                L"Ollama");
    AppendMenuW(backend_menu, MF_STRING | (config_.llm.provider == "llamacpp" ? MF_CHECKED : 0), kMenuProviderLlama,
                L"llama.cpp (embedded)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(backend_menu), L"LLM backend");

    HMENU models_menu = CreatePopupMenu();
    menu_models_ = providers_->ollama().list_models();
    if (menu_models_.empty()) {
        AppendMenuW(models_menu, MF_STRING | MF_GRAYED, 0, L"Ollama not reachable");
    }
    for (size_t i = 0; i < menu_models_.size() && i < 100; ++i) {
        const bool current = menu_models_[i] == config_.ollama.model;
        AppendMenuW(models_menu, MF_STRING | (current ? MF_CHECKED : 0), kMenuModelBase + static_cast<UINT>(i),
                    to_wide(menu_models_[i]).c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(models_menu), L"Ollama model");

    AppendMenuW(menu, MF_STRING | (config_.tts.enabled ? MF_CHECKED : 0), kMenuSpeak, L"Speak answers");
    AppendMenuW(menu, MF_STRING | (autostart::enabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"Edit config");
    AppendMenuW(menu, MF_STRING, kMenuReload, L"Reload config && plugins");
    AppendMenuW(menu, MF_STRING, kMenuOpenPlugins, L"Open plugins folder");
    AppendMenuW(menu, MF_STRING, kMenuOpenLogs, L"Open logs");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd) on_menu(cmd);
}

void App::on_menu(UINT id) {
    if (id >= kMenuModelBase && id < kMenuModelBase + menu_models_.size()) {
        config_.ollama.model = menu_models_[id - kMenuModelBase];
        save_config();
        apply_config();
        return;
    }
    switch (id) {
        case kMenuProviderAuto: config_.llm.provider = "auto"; break;
        case kMenuProviderOllama: config_.llm.provider = "ollama"; break;
        case kMenuProviderLlama: config_.llm.provider = "llamacpp"; break;
        case kMenuSpeak: config_.tts.enabled = !config_.tts.enabled; break;
        case kMenuAutostart: autostart::set_enabled(!autostart::enabled()); return;
        case kMenuAsk:
            if (state_ == State::Idle) {
                target_ = {};
                open_input();
            }
            return;
        case kMenuOpenConfig: open_path(config_path_); return;
        case kMenuReload:
            if (state_ != State::Idle) dismiss();
            load_config();
            apply_config();
            reload_plugins();
            tray_.notify(L"win-assist", L"Config and plugins reloaded.");
            return;
        case kMenuOpenPlugins: {
            auto dir = paths::app_data() / L"plugins";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            open_path(dir);
            return;
        }
        case kMenuOpenLogs: open_path(paths::local_data() / L"logs"); return;
        case kMenuQuit: DestroyWindow(hwnd_); return;
        default: return;
    }
    save_config();
    apply_config();
}

LRESULT CALLBACK App::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<App*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbar_created_ && taskbar_created_ != 0) {
        tray_.recreate();
        return 0;
    }
    switch (msg) {
        case WM_HOTKEY:
            on_hotkey(static_cast<int>(wp));
            return 0;
        case WM_TIMER:
            if (wp == kTimerPoll) on_ptt_poll();
            else if (wp == kTimerDismiss) {
                if (tts_.speaking()) {
                    schedule_dismiss(500);
                } else {
                    KillTimer(hwnd_, kTimerDismiss);
                    dismiss();
                }
            } else if (wp == kTimerMaintenance) {
                maintenance();
            }
            return 0;
        case kMsgSession:
            on_session_event(reinterpret_cast<SessionEvent*>(lp));
            return 0;
        case kMsgTray:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP: show_tray_menu(); break;
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (state_ == State::Idle) {
                        target_ = {};
                        open_input();
                    }
                    break;
            }
            return 0;
        case kMsgAnotherInstance:
            tray_.notify(L"win-assist", L"Already running \u2014 hold " + hotkey_label(config_.hotkeys.assistant) +
                                            L" to talk.");
            return 0;
        case WM_DESTROY:
            tray_.destroy();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

int App::run(HINSTANCE instance) {
    instance_ = instance;
    log::init(paths::local_data() / L"logs" / L"daemon.log");
    log::info("win-assist starting (pid {})", GetCurrentProcessId());
    config_path_ = paths::app_data() / L"config.json";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = kMainWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives TaskbarCreated.
    CreateWindowExW(WS_EX_TOOLWINDOW, kMainWindowClass, L"win-assist", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                    this);
    if (!hwnd_) return 1;
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    tray_.create(hwnd_, kMsgTray);

    load_config();
    providers_ = std::make_unique<ProviderHub>(config_);
    stt_ = std::make_unique<SttClient>(config_.stt);
    PluginServices services;
    services.ollama_post = [this](const std::string& path, const std::string& body, bool& ok) {
        HttpResponse r = providers_->ollama().post(path, body, 60000);
        ok = r.ok();
        return r.body;
    };
    services.config_json = [this] {
        std::lock_guard lock(config_json_mutex_);
        return config_json_;
    };
    plugins_ = std::make_unique<PluginHost>(std::move(services));
    assistant_ = std::make_unique<Assistant>(hwnd_, kMsgSession, *plugins_, *providers_, *stt_);

    Overlay::Callbacks callbacks;
    callbacks.on_submit = [this](std::wstring text) { submit_text(std::move(text)); };
    callbacks.on_cancel = [this] {
        if (state_ == State::Input) dismiss();
    };
    overlay_.create(instance, std::move(callbacks));

    apply_config();
    reload_plugins();
    log::info("ready: assistant={}, dictation={}, provider={}", config_.hotkeys.assistant, config_.hotkeys.dictation,
              config_.llm.provider);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    assistant_->cancel();
    assistant_->join();
    audio_.abort();
    overlay_.destroy();
    plugins_->unload();
    stt_->shutdown();
    providers_->shutdown();
    log::info("win-assist stopped");
    return 0;
}

}  // namespace wa
