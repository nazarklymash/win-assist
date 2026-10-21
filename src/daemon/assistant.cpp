#include "daemon/assistant.h"

#include <objbase.h>

#include <format>

#include "common/log.h"
#include "common/strings.h"
#include "daemon/audio/capture.h"
#include "daemon/audio/vad.h"
#include "daemon/system/inject.h"
#include "daemon/system/screenshot.h"

namespace wa {

using nlohmann::json;

namespace {

constexpr auto kHistoryTtl = std::chrono::minutes(5);

IntentDef intent(std::string name, std::string description, std::vector<std::string> patterns,
                 std::vector<IntentParam> params = {}, bool expose = true) {
    IntentDef d;
    d.name = std::move(name);
    d.description = std::move(description);
    d.patterns = std::move(patterns);
    d.parameters = std::move(params);
    d.expose_to_llm = expose;
    return d;
}

IntentParam param(std::string name, std::string description, bool required = true) {
    IntentParam p;
    p.name = std::move(name);
    p.description = std::move(description);
    p.required = required;
    return p;
}

std::string strip_thinking(std::string text) {
    for (size_t open; (open = text.find("<think>")) != std::string::npos;) {
        size_t close = text.find("</think>", open);
        text.erase(open, close == std::string::npos ? std::string::npos : close + 8 - open);
    }
    return trim(text);
}

// Markdown symbols read badly through TTS.
std::string speakable(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '*' || c == '#' || c == '`' || c == '_') continue;
        out += c;
    }
    return out;
}

std::string now_string() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return std::format("{} {:04}-{:02}-{:02} {:02}:{:02}", days[st.wDayOfWeek % 7], st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute);
}

}  // namespace

std::vector<PluginManifest> builtin_manifests() {
    PluginManifest m;
    m.id = kBuiltinPluginId;
    m.name = "Built-in skills";
    m.version = "1.0.0";
    m.kind = PluginKind::Builtin;
    m.intents = {
        intent("describe_screen", "Look at the user's active window and answer a question about what is visible",
               {"what's on my screen", "what is on my screen", "describe [my|the] screen", "what am i looking at",
                "(read|explain|summarize) [my|the|this] (screen|window|page)",
                "(look at|check) [my|the] screen [and] {question}"},
               {param("question", "What the user wants to know about the screen", false)}),
        intent("type_text", "Type the given text verbatim into the user's active window",
               {"type {text}", "dictate {text}"}, {param("text", "Exact text to type")}, false),
        intent("compose_text",
               "Write new text (email, message, reply, code) and insert it into the active window. Only when the user "
               "explicitly asks to write or draft something, never to answer a question",
               {"(write|draft|compose) {instruction}"},
               {param("instruction", "What to write, e.g. 'a polite reply declining the meeting'")}),
        intent("clear_conversation", "Forget the conversation so far",
               {"(new|reset [the]|clear [the]) (conversation|chat)", "forget (that|everything)"}, {}, false),
        intent("help", "List what the assistant can do", {"help", "what can you do"}, {}, false),
    };
    return {m};
}

Assistant::Assistant(HWND notify, UINT message, PluginHost& plugins, ProviderHub& providers, SttClient& stt)
    : notify_(notify), message_(message), plugins_(plugins), providers_(providers), stt_(stt),
      cancel_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

Assistant::~Assistant() {
    cancel();
    join();
    CloseHandle(cancel_event_);
}

void Assistant::start(SessionRequest request) {
    join();
    cancel_ = false;
    ResetEvent(cancel_event_);
    busy_ = true;
    session_ = request.id;
    thread_ = std::thread(&Assistant::run, this, std::move(request));
}

void Assistant::cancel() {
    if (!busy_) return;
    cancel_ = true;
    SetEvent(cancel_event_);
    providers_.cancel_all();
}

void Assistant::join() {
    if (thread_.joinable()) thread_.join();
}

void Assistant::clear_history() {
    std::lock_guard lock(history_mutex_);
    history_.clear();
}

void Assistant::post(SessionEvent::Kind kind, std::string text) {
    auto* e = new SessionEvent;
    e->session = session_;
    e->kind = kind;
    e->text = std::move(text);
    if (!PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(e))) delete e;
}

void Assistant::finish(ActionResult result, bool injected, std::string answer) {
    std::string summary = result.display.size() > 160 ? result.display.substr(0, 160) + "..." : result.display;
    for (char& c : summary) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    const uint64_t session = session_;
    if (result.ok) log::info("session {}: ok{} \"{}\"", session, injected ? " (injected)" : "", summary);
    else log::warn("session {}: {}", session, summary);

    auto* e = new SessionEvent;
    e->session = session_;
    e->kind = result.ok ? SessionEvent::Kind::Done : SessionEvent::Kind::Error;
    e->result = std::move(result);
    e->injected = injected;
    e->answer = std::move(answer);
    busy_ = false;
    if (!PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(e))) delete e;
}

std::string Assistant::system_context(const SessionRequest& request) const {
    std::string ctx = request.config.llm.system_prompt;
    ctx += "\nAnswer questions directly in your reply. Do not insert text into the user's window unless they "
           "explicitly ask you to write, draft or type something.";
    ctx += "\nCurrent time: " + now_string() + ".";
    if (!request.target.title.empty() || !request.target.process.empty()) {
        ctx += "\nThe user's active window is \"" + to_utf8(request.target.title) + "\" (" +
               to_utf8(request.target.process) + ").";
    }
    return ctx;
}

void Assistant::run(SessionRequest request) {
    struct ComScope {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } com;
    std::string utterance = trim(request.text);

    if (request.voice) {
        SpeechSpan span = detect_speech(request.audio);
        if (!span.has_speech) {
            finish(ActionResult::error("I didn't hear anything."), false);
            return;
        }
        std::vector<float> speech(request.audio.begin() + static_cast<ptrdiff_t>(span.begin),
                                  request.audio.begin() + static_cast<ptrdiff_t>(span.end));
        post(SessionEvent::Kind::Status, "Transcribing\u2026");
        std::string error;
        auto transcript = stt_.transcribe(speech, error, cancel_event_);
        if (cancel_) {
            finish(ActionResult::error("Cancelled."), false);
            return;
        }
        if (!transcript) {
            finish(ActionResult::error("Speech recognition failed: " + error), false);
            return;
        }
        utterance = transcript->text;
        // Whisper's usual hallucinations on near-silence.
        if (utterance.empty() || utterance == "[BLANK_AUDIO]" || utterance == "(silence)") {
            finish(ActionResult::error("I didn't catch that."), false);
            return;
        }
        post(SessionEvent::Kind::Transcript, utterance);
    }

    if (request.mode == SessionMode::Dictation) {
        std::string error;
        bool ok = inject_text(request.target, to_wide(utterance), error);
        ActionResult r;
        r.ok = ok;
        r.display = ok ? utterance : error;
        finish(r, ok);
        return;
    }

    ActionResult result;
    std::string answer;
    if (auto match = plugins_.router().match(utterance)) {
        log::info("router: '{}' -> {}/{}", utterance, match->plugin_id, match->intent);
        result = dispatch(match->plugin_id, match->intent, match->parameters, request, utterance);
    } else {
        result = ask_llm(utterance, request, answer);
    }

    bool injected = false;
    if (result.ok && !result.inject_text.empty() && !cancel_) {
        std::string error;
        injected = inject_text(request.target, to_wide(result.inject_text), error);
        if (!injected) {
            result.ok = false;
            result.display = error;
        }
    }
    if (cancel_) result = ActionResult::error("Cancelled.");
    finish(std::move(result), injected, std::move(answer));
}

ActionResult Assistant::dispatch(const std::string& plugin_id, const std::string& intent, const json& params,
                                 const SessionRequest& request, const std::string& utterance) {
    if (plugin_id == kBuiltinPluginId) return run_builtin(intent, params, request, utterance);
    post(SessionEvent::Kind::Status, "Running " + intent + "\u2026");
    InvokeContext ctx{utterance, request.target};
    ActionResult r = plugins_.invoke(plugin_id, intent, params, ctx, cancel_);
    if (r.ok && r.display.empty() && r.inject_text.empty()) r.display = "Done.";
    return r;
}

ActionResult Assistant::run_builtin(const std::string& intent, const json& params, const SessionRequest& request,
                                    const std::string& utterance) {
    auto str = [&](const char* key) {
        auto it = params.find(key);
        return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (intent == "type_text") {
        ActionResult r;
        r.inject_text = str("text");
        r.display = r.inject_text;
        return r;
    }
    if (intent == "compose_text") return compose(str("instruction").empty() ? utterance : str("instruction"), request);
    if (intent == "describe_screen") return describe_screen(str("question").empty() ? utterance : str("question"), request);
    if (intent == "clear_conversation") {
        clear_history();
        ActionResult r;
        r.display = "Starting a fresh conversation.";
        r.speech = r.display;
        return r;
    }
    if (intent == "help") {
        std::string text = "Ask me anything, or try:";
        for (const auto& m : plugins_.manifests()) {
            for (const auto& i : m.intents) {
                if (!i.patterns.empty() && i.expose_to_llm) text += "\n\u2022 " + i.patterns.front();
            }
        }
        ActionResult r;
        r.display = text;
        return r;
    }
    return ActionResult::error("Unknown built-in skill '" + intent + "'.");
}

ActionResult Assistant::ask_llm(const std::string& utterance, const SessionRequest& request, std::string& answer) {
    std::string error;
    LlmProvider* provider = providers_.select(false, error);
    if (!provider) return ActionResult::error(error);
    post(SessionEvent::Kind::Status, "Thinking\u2026");

    ChatRequest chat;
    chat.max_tokens = request.config.llm.max_tokens;
    chat.messages.push_back({"system", system_context(request), {}});
    {
        std::lock_guard lock(history_mutex_);
        if (std::chrono::steady_clock::now() - last_turn_ > kHistoryTtl) history_.clear();
        const size_t keep = static_cast<size_t>(std::max(0, request.config.llm.history_turns)) * 2;
        size_t start = history_.size() > keep ? history_.size() - keep : 0;
        for (size_t i = start; i < history_.size(); ++i) chat.messages.push_back(history_[i]);
    }
    chat.messages.push_back({"user", utterance, {}});
    chat.tools = plugins_.tools().tools;

    bool in_think = false;
    std::string pending;
    const auto started = std::chrono::steady_clock::now();
    ChatResponse response = provider->chat(
        chat,
        [&](std::string_view delta) {
            // Hide any reasoning block a thinking model emits despite think=false.
            pending.append(delta);
            for (;;) {
                if (in_think) {
                    size_t close = pending.find("</think>");
                    if (close == std::string::npos) return;
                    pending.erase(0, close + 8);
                    in_think = false;
                    continue;
                }
                size_t open = pending.find("<think>");
                if (open == std::string::npos) {
                    // Hold back a possible partial "<think>" tag.
                    size_t lt = pending.rfind('<');
                    size_t flush = (lt != std::string::npos && pending.size() - lt < 7) ? lt : pending.size();
                    if (flush > 0) post(SessionEvent::Kind::Delta, pending.substr(0, flush));
                    pending.erase(0, flush);
                    return;
                }
                if (open > 0) post(SessionEvent::Kind::Delta, pending.substr(0, open));
                pending.erase(0, open + 7);
                in_think = true;
            }
        },
        cancel_);
    log::info("llm: {} answered in {} ms ({} tool calls)", provider->name(),
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
              response.tool_calls.size());
    if (!pending.empty() && !in_think) post(SessionEvent::Kind::Delta, pending);
    if (!response.ok) return ActionResult::error(response.error == "cancelled" ? "Cancelled." : response.error);

    if (!response.tool_calls.empty()) {
        const ToolCall& call = response.tool_calls.front();
        auto target = plugins_.tools().by_name.find(call.name);
        if (target == plugins_.tools().by_name.end()) {
            return ActionResult::error("The model asked for an unknown tool '" + call.name + "'.");
        }
        log::info("llm tool call: {} {}", call.name, call.arguments.dump());
        post(SessionEvent::Kind::Reset);
        return dispatch(target->second.plugin_id, target->second.intent, call.arguments, request, utterance);
    }

    answer = strip_thinking(response.content);
    if (answer.empty()) return ActionResult::error("The model returned an empty answer.");
    {
        std::lock_guard lock(history_mutex_);
        history_.push_back({"user", utterance, {}});
        history_.push_back({"assistant", answer, {}});
        last_turn_ = std::chrono::steady_clock::now();
    }
    ActionResult r;
    r.display = answer;
    r.speech = speakable(answer);
    return r;
}

ActionResult Assistant::compose(const std::string& instruction, const SessionRequest& request) {
    std::string error;
    LlmProvider* provider = providers_.select(false, error);
    if (!provider) return ActionResult::error(error);
    post(SessionEvent::Kind::Status, "Writing\u2026");
    post(SessionEvent::Kind::Reset);

    ChatRequest chat;
    chat.max_tokens = std::max(request.config.llm.max_tokens, 800);
    chat.messages.push_back(
        {"system",
         "You write text that will be typed directly into the user's active window (" + to_utf8(request.target.process) +
             ": \"" + to_utf8(request.target.title) +
             "\"). Output only the text itself: no preface, no surrounding quotes, no explanations.",
         {}});
    chat.messages.push_back({"user", instruction, {}});
    ChatResponse response = provider->chat(
        chat, [&](std::string_view d) { post(SessionEvent::Kind::Delta, std::string(d)); }, cancel_);
    if (!response.ok) return ActionResult::error(response.error);
    ActionResult r;
    r.inject_text = strip_thinking(response.content);
    r.display = r.inject_text;
    if (r.inject_text.empty()) return ActionResult::error("The model returned no text.");
    return r;
}

ActionResult Assistant::describe_screen(const std::string& question, const SessionRequest& request) {
    std::string error;
    LlmProvider* provider = providers_.select(true, error);
    if (!provider) return ActionResult::error(error);
    post(SessionEvent::Kind::Status, "Looking at your screen\u2026");
    auto png = capture_window_png_base64(request.target.window, 1280, error);
    if (!png) return ActionResult::error("Screen capture failed: " + error);
    post(SessionEvent::Kind::Reset);

    ChatRequest chat;
    chat.vision = true;
    chat.max_tokens = request.config.llm.max_tokens;
    chat.messages.push_back({"system", system_context(request), {}});
    ChatMessage user{"user",
                     "This is a screenshot of my active window. " +
                         (question.empty() ? std::string("Describe what is on it.") : question),
                     {*png}};
    chat.messages.push_back(std::move(user));
    ChatResponse response = provider->chat(
        chat, [&](std::string_view d) { post(SessionEvent::Kind::Delta, std::string(d)); }, cancel_);
    if (!response.ok) return ActionResult::error(response.error);
    ActionResult r;
    r.display = strip_thinking(response.content);
    r.speech = speakable(r.display);
    return r;
}

}  // namespace wa
