#include "daemon/assistant.h"

#include <objbase.h>

#include <algorithm>
#include <format>

#include "common/log.h"
#include "common/strings.h"
#include "daemon/ai/model_router.h"
#include "daemon/audio/capture.h"
#include "daemon/audio/vad.h"
#include "daemon/system/inject.h"
#include "daemon/system/ocr.h"
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
                "(look at|check) [my|the] screen [and] {question}",
                "що на (екрані|екране)", "опиши [мій|цей] (екран|вікно)"},
               {param("question", "What the user wants to know about the screen", false)}),
        intent("read_screen_text",
               "Extract the visible text from the user's active window with offline OCR (no vision model needed)",
               {"read [the] text (on|from) [my|the] (screen|window)", "ocr [my|the] (screen|window)",
                "copy [the] text (on|from) [my|the] screen", "what does [the] (screen|window) say",
                "прочитай текст (на|з) (екрана|екрану|вікна)", "розпізнай текст [на екрані]"},
               {}),
        intent("summarize_selection",
               "Summarize the text currently selected in the user's active window (do not type into the window)",
               {"summarize [this|that|it|the selection|the text]", "sum up [this|that|it]",
                "shorten [this|that|the selection]", "підсумуй [це|це виділення|текст]", "коротко [про] [це]"},
               {}),
        intent("translate_selection",
               "Translate the selected text and replace the selection with the translation",
               {"translate [this|that|it|the selection|the text] [to|into] {language}",
                "translate [this|that|it|the selection|the text]",
                "переклади [це|це виділення|текст] [на|у] {language}", "переклади [це|це виділення|текст]"},
               {param("language", "Target language, e.g. Ukrainian, English, Polish", false)}),
        intent("fix_selection",
               "Fix grammar and clarity of the selected text and replace the selection with the corrected version",
               {"fix [this|that|it|the selection|the text]", "correct [the] (grammar|text|selection)",
                "proofread [this|that|it]", "виправ [це|це виділення|текст]", "виправ (граматику|текст)"},
               {}),
        intent("reply_to_selection",
               "Draft a reply to the selected message/email and insert it into the active window",
               {"reply [to this|to that|to the selection]", "answer [this|that|the selection]",
                "draft [a] reply [to this|to that]", "відповіди [на це|на це повідомлення]",
                "напиши відповідь [на це]"},
               {}),
        intent("explain_selection",
               "Explain the currently selected code or text without modifying it",
               {"explain [this|that|it|the selection|the code|the error]",
                "what does [this|that] mean", "поясни [це|помилку|код]"},
               {}),
        intent("commit_message_from_selection",
               "Write a concise git commit message for the selected diff or description and copy/insert it",
               {"[write|generate|make] [a] commit message [for this|from this|from the selection]",
                "commit message [for this]", "напиши [повідомлення] коміту [для цього]"},
               {}),
        intent("type_text", "Type the given text verbatim into the user's active window",
               {"type {text}", "dictate {text}", "надрукуй {text}", "введи {text}"},
               {param("text", "Exact text to type")}, false),
        intent("compose_text",
               "Write new text (email, message, reply, code) and insert it into the active window. Only when the user "
               "explicitly asks to write or draft something, never to answer a question",
               {"(write|draft|compose) {instruction}", "(напиши|скласти|набери) {instruction}"},
               {param("instruction", "What to write, e.g. 'a polite reply declining the meeting'")}),
        intent("remember_fact",
               "Save a durable note about the user (preferences, names, paths, decisions) for future sessions",
               {"remember [that] {text}", "note [that] {text}", "keep in mind [that] {text}",
                "save [this] [to] memory {text}", "запам'ятай [що] {text}", "запиши [що] {text}"},
               {param("text", "The fact to store, as a short self-contained sentence")}),
        intent("recall_memory", "Look up what was previously remembered about a topic",
               {"what do you (know|remember) about {query}", "recall {query}",
                "what did i (say|tell you) about {query}", "do you remember {query}",
                "що ти (знаєш|пам'ятаєш) про {query}", "пригадай {query}"},
               {param("query", "Topic to look up", false)}),
        intent("forget_memory", "Delete stored notes matching a topic, or all of them",
               {"forget [what you know] about {query}", "delete [the] (note|memory) about {query}",
                "forget everything [you know] about me", "забудь [про] {query}", "видали нотатки [про] {query}"},
               {param("query", "Topic to delete; empty deletes every note", false)}),
        intent("clear_conversation", "Forget the conversation so far",
               {"(new|reset [the]|clear [the]) (conversation|chat)", "forget (that|everything)",
                "(нова|очисти|скинь) (розмова|розмову|чат)"},
               {}, false),
        intent("help", "List what the assistant can do",
               {"help", "what can you do", "допомога", "що ти вмієш"}, {}, false),
    };
    return {m};
}

Assistant::Assistant(HWND notify, UINT message, PluginHost& plugins, ProviderHub& providers, SttClient& stt,
                     MemoryStore& memory)
    : notify_(notify), message_(message), plugins_(plugins), providers_(providers), stt_(stt), memory_(memory),
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
    ctx += "\nYou can control this Windows PC via tools: list/kill processes, run programs or shell "
           "commands, open paths/URLs, read/write/list/copy/move/delete files, system info, "
           "volume/brightness/media, lock/sleep/shutdown/restart, clipboard, windows, search, browser, "
           "reminders, and focus assist. Prefer a dedicated tool when one fits; use run_shell for "
           "anything else. Confirm before destructive power actions when the user is ambiguous.";
    ctx += "\nCurrent time: " + now_string() + ".";
    if (!request.target.title.empty() || !request.target.process.empty()) {
        ctx += "\nThe user's active window is \"" + to_utf8(request.target.title) + "\" (" +
               to_utf8(request.target.process) + ").";
    }
    return ctx;
}

std::string Assistant::recalled_context(const std::string& utterance) {
    auto hits = memory_.recall(utterance, 3, 0.45f);
    if (hits.empty()) return {};
    std::string out = "\nNotes you previously saved about this user (use them if relevant, never mention this list):";
    for (const auto& h : hits) out += "\n- " + h.entry.text;
    log::info("memory: injected {} note(s)", hits.size());
    return out;
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
        // Brief LLM follow-up for successful non-injection actions (volume, open, …).
        if (result.ok && result.inject_text.empty() && !result.display.empty() && !cancel_ &&
            match->intent != "help" && match->intent != "clear_conversation" &&
            match->intent.find("selection") == std::string::npos &&
            match->intent != "describe_screen") {
            std::string follow;
            ActionResult narrated = narrate_tool_result(utterance, match->intent, result.display, request, follow);
            if (narrated.ok && !follow.empty()) {
                result.display = result.display + "\n\n" + follow;
                result.speech = speakable(follow);
                answer = follow;
            }
        }
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
    if (intent == "read_screen_text") {
        post(SessionEvent::Kind::Status, "Reading your screen\u2026");
        std::string error;
        auto text = ocr_window(request.target.window, error);
        if (!text) return ActionResult::error("Could not read the screen: " + error);
        if (text->empty()) return ActionResult::error("No text found on the screen.");
        ActionResult r;
        r.display = *text;
        r.speech = speakable(text->size() > 400 ? text->substr(0, 400) : *text);
        return r;
    }
    if (intent == "summarize_selection") return process_selection("summarize", {}, request);
    if (intent == "translate_selection") return process_selection("translate", str("language"), request);
    if (intent == "fix_selection") return process_selection("fix", {}, request);
    if (intent == "reply_to_selection") return process_selection("reply", {}, request);
    if (intent == "explain_selection") return process_selection("explain", {}, request);
    if (intent == "commit_message_from_selection") return process_selection("commit", {}, request);
    if (intent == "remember_fact") {
        const std::string text = str("text").empty() ? utterance : str("text");
        std::string error;
        if (!memory_.remember(text, request.voice ? "voice" : "typed", error)) return ActionResult::error(error);
        ActionResult r;
        r.display = "Remembered: " + trim(text);
        r.speech = "Got it, I'll remember that.";
        return r;
    }
    if (intent == "recall_memory") {
        const std::string query = str("query").empty() ? utterance : str("query");
        auto hits = memory_.recall(query, 5, 0.25f);
        ActionResult r;
        if (hits.empty()) {
            r.display = memory_.size() == 0 ? "I haven't stored any notes yet."
                                            : "Nothing stored about that.";
            r.speech = r.display;
            return r;
        }
        std::string text = "What I remember:";
        for (const auto& h : hits) text += "\n\u2022 " + h.entry.text;
        r.display = text;
        r.speech = speakable(text);
        return r;
    }
    if (intent == "forget_memory") {
        const std::string query = str("query");
        std::string error;
        const size_t removed = memory_.forget(query, error);
        if (!error.empty()) return ActionResult::error(error);
        ActionResult r;
        r.display = removed == 0 ? "No matching notes to forget."
                                 : "Forgot " + std::to_string(removed) + " note(s).";
        r.speech = r.display;
        return r;
    }
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
    chat.messages.push_back({"system", system_context(request) + recalled_context(utterance), {}});
    {
        std::lock_guard lock(history_mutex_);
        if (std::chrono::steady_clock::now() - last_turn_ > kHistoryTtl) history_.clear();
        const size_t keep = static_cast<size_t>(std::max(0, request.config.llm.history_turns)) * 2;
        size_t start = history_.size() > keep ? history_.size() - keep : 0;
        for (size_t i = start; i < history_.size(); ++i) chat.messages.push_back(history_[i]);
    }
    chat.messages.push_back({"user", utterance, {}});
    chat.tools = plugins_.tools().tools;

    std::string route_why;
    chat.model = select_model(classify_utterance(utterance, !chat.tools.empty()), request.config, &route_why);
    if (!chat.model.empty()) log::info("llm: model '{}' ({})", chat.model, route_why);

    // Streams deltas to the overlay while hiding any <think> block a thinking
    // model emits despite think=false.
    struct ThinkFilter {
        Assistant* self;
        std::string pending;
        bool in_think = false;

        void feed(std::string_view delta) {
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
                    if (flush > 0) self->post(SessionEvent::Kind::Delta, pending.substr(0, flush));
                    pending.erase(0, flush);
                    return;
                }
                if (open > 0) self->post(SessionEvent::Kind::Delta, pending.substr(0, open));
                pending.erase(0, open + 7);
                in_think = true;
            }
        }
        void flush() {
            if (!pending.empty() && !in_think) self->post(SessionEvent::Kind::Delta, pending);
            pending.clear();
        }
        void reset() {
            pending.clear();
            in_think = false;
        }
    } filter{this};

    const auto started = std::chrono::steady_clock::now();
    ChatResponse response =
        provider->chat(chat, [&](std::string_view d) { filter.feed(d); }, cancel_);
    log::info("llm: {} answered in {} ms ({} tool calls)", provider->name(),
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
              response.tool_calls.size());
    filter.flush();
    if (!response.ok) return ActionResult::error(response.error == "cancelled" ? "Cancelled." : response.error);

    // Agent loop: run the tool the model asked for, feed the result back, and
    // let it decide whether another step is needed.
    const int max_steps = std::clamp(request.config.llm.max_tool_steps, 1, 12);
    std::vector<std::string> steps_done;
    std::vector<std::string> call_signatures;
    ActionResult last_tool;
    bool ran_any_tool = false;

    for (int step = 0; step < max_steps && !response.tool_calls.empty(); ++step) {
        if (cancel_) return ActionResult::error("Cancelled.");
        const ToolCall call = response.tool_calls.front();
        auto target = plugins_.tools().by_name.find(call.name);
        if (target == plugins_.tools().by_name.end()) {
            if (!ran_any_tool) return ActionResult::error("The model asked for an unknown tool '" + call.name + "'.");
            break;
        }

        // Guard against the model repeating the same call forever.
        const std::string signature = call.name + "|" + call.arguments.dump();
        if (std::count(call_signatures.begin(), call_signatures.end(), signature) >= 2) {
            log::warn("agent: '{}' repeated, stopping the loop", call.name);
            break;
        }
        call_signatures.push_back(signature);

        log::info("agent step {}/{}: {} {}", step + 1, max_steps, call.name, call.arguments.dump());
        post(SessionEvent::Kind::Status,
             "Step " + std::to_string(step + 1) + "/" + std::to_string(max_steps) + ": " + call.name + "\u2026");
        post(SessionEvent::Kind::Reset);
        ActionResult tool_result =
            dispatch(target->second.plugin_id, target->second.intent, call.arguments, request, utterance);

        if (!tool_result.ok) {
            // A refusal (denied permission) ends the run; other failures are
            // reported back so the model can adapt or explain.
            if (tool_result.display.find("Cancelled") == 0 || tool_result.display.find("blocked by") != std::string::npos) {
                return tool_result;
            }
            if (!ran_any_tool && step + 1 >= max_steps) return tool_result;
            steps_done.push_back(call.name + ": failed \u2014 " + tool_result.display);
        } else {
            ran_any_tool = true;
            last_tool = tool_result;
            steps_done.push_back(call.name + (tool_result.display.empty() ? "" : ": " + tool_result.display));
            // Pure injection tools (compose/type) must not get a chatty follow-up.
            if (!tool_result.inject_text.empty() && tool_result.display == tool_result.inject_text) return tool_result;
        }

        const bool last_step = step + 1 >= max_steps;
        chat.messages.push_back({"assistant", std::string("[tool_call] ") + call.name + " " + call.arguments.dump(), {}});
        chat.messages.push_back(
            {"user",
             std::string("[tool_result] status=") + (tool_result.ok ? "ok" : "error") + " display=" +
                 (tool_result.display.empty() ? "(done)" : tool_result.display) + "\n" +
                 (last_step ? "This was the final step. Summarize the outcome for the user in one or two short "
                              "sentences and call no further tools."
                            : "If the user's request is now fully handled, reply with a brief spoken confirmation "
                              "(one or two short sentences) and call no more tools. If more work is genuinely "
                              "needed, call the next tool."),
             {}});
        post(SessionEvent::Kind::Status, "Thinking\u2026");
        post(SessionEvent::Kind::Reset);
        filter.reset();
        response = provider->chat(chat, [&](std::string_view d) { filter.feed(d); }, cancel_);
        filter.flush();
        if (!response.ok) {
            // The tools already ran; report what happened.
            if (ran_any_tool) break;
            return ActionResult::error(response.error == "cancelled" ? "Cancelled." : response.error);
        }
    }

    if (ran_any_tool) {
        answer = strip_thinking(response.content);
        ActionResult r = last_tool;
        // With several steps, show what was done above the model's summary.
        std::string trail;
        if (steps_done.size() > 1) {
            for (const auto& s : steps_done) trail += "\u2022 " + s + "\n";
        } else if (!steps_done.empty()) {
            trail = last_tool.display;
        }
        r.display = trail;
        if (!answer.empty()) {
            if (!r.display.empty()) r.display += "\n";
            r.display += answer;
            r.speech = speakable(answer);
        }
        if (r.display.empty()) r.display = "Done.";
        {
            std::lock_guard lock(history_mutex_);
            history_.push_back({"user", utterance, {}});
            history_.push_back({"assistant", r.display, {}});
            last_turn_ = std::chrono::steady_clock::now();
        }
        return r;
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

ActionResult Assistant::narrate_tool_result(const std::string& utterance, const std::string& intent,
                                            const std::string& tool_display, const SessionRequest& request,
                                            std::string& follow_up) {
    std::string error;
    LlmProvider* provider = providers_.select(false, error);
    if (!provider) return ActionResult::error(error);
    ChatRequest chat;
    chat.model = select_model(TaskClass::Quick, request.config);
    chat.max_tokens = 80;
    chat.messages.push_back(
        {"system",
         "The desktop assistant just ran action '" + intent + "' for the user. "
         "Result: " + tool_display + ". Reply with one short friendly sentence confirming it "
         "(same language as the user). No tools. No quotes.",
         {}});
    chat.messages.push_back({"user", utterance, {}});
    ChatResponse response = provider->chat(chat, nullptr, cancel_);
    if (!response.ok) return ActionResult::error(response.error);
    follow_up = strip_thinking(response.content);
    ActionResult r;
    r.display = follow_up;
    return r;
}

ActionResult Assistant::compose(const std::string& instruction, const SessionRequest& request) {
    std::string error;
    LlmProvider* provider = providers_.select(false, error);
    if (!provider) return ActionResult::error(error);
    post(SessionEvent::Kind::Status, "Writing\u2026");
    post(SessionEvent::Kind::Reset);

    ChatRequest chat;
    chat.model = select_model(TaskClass::Chat, request.config);
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

ActionResult Assistant::process_selection(const std::string& mode, const std::string& language,
                                          const SessionRequest& request) {
    std::wstring selected_w;
    std::string error;
    post(SessionEvent::Kind::Status, "Reading selection\u2026");
    if (!capture_selection(request.target, selected_w, error)) return ActionResult::error(error);
    const std::string selected = to_utf8(selected_w);
    if (selected.size() > 12000) {
        return ActionResult::error("The selection is too long (" + std::to_string(selected.size()) +
                                   " characters). Select a shorter passage.");
    }

    LlmProvider* provider = providers_.select(false, error);
    if (!provider) return ActionResult::error(error);

    const bool replace = mode == "translate" || mode == "fix" || mode == "reply" || mode == "commit";
    std::string system;
    std::string user;
    std::string status;
    if (mode == "summarize") {
        status = "Summarizing\u2026";
        system = "Summarize the user's selected text in a few short sentences. "
                 "Reply in the same language as the selection unless asked otherwise. "
                 "Output only the summary.";
        user = selected;
    } else if (mode == "translate") {
        status = "Translating\u2026";
        const std::string target = language.empty() ? "Ukrainian" : language;
        system = "Translate the selected text into " + target +
                 ". Output only the translation, with no preface or quotes. "
                 "Preserve meaning, tone, and basic formatting.";
        user = selected;
    } else if (mode == "fix") {
        status = "Fixing\u2026";
        system = "Fix grammar, spelling, and clarity of the selected text. "
                 "Keep the same language and meaning. Output only the corrected text.";
        user = selected;
    } else if (mode == "reply") {
        status = "Drafting reply\u2026";
        system =
            "Draft a concise reply to the selected message as if typing into the user's active window (" +
            to_utf8(request.target.process) + "). Match the language of the selection. "
            "Output only the reply text: no subject line, no preface, no quotes.";
        user = selected;
    } else if (mode == "explain") {
        status = "Explaining\u2026";
        system = "Explain the selected text or code clearly and briefly. "
                 "Match the user's language. Do not modify the selection.";
        user = selected;
    } else if (mode == "commit") {
        status = "Writing commit message\u2026";
        system = "Write a concise conventional git commit message for the selected diff or change "
                 "description. Output only the commit message (subject line, optional body). "
                 "No quotes, no markdown fences.";
        user = selected;
    } else {
        return ActionResult::error("Unknown selection action '" + mode + "'.");
    }

    post(SessionEvent::Kind::Status, status);
    post(SessionEvent::Kind::Reset);
    ChatRequest chat;
    // Selection rewrites are mechanical; the quick model keeps them snappy.
    chat.model = select_model(selected.size() > 2000 ? TaskClass::Chat : TaskClass::Quick, request.config);
    chat.max_tokens = std::max(request.config.llm.max_tokens, replace ? 800 : 400);
    chat.messages.push_back({"system", system, {}});
    chat.messages.push_back({"user", user, {}});
    ChatResponse response = provider->chat(
        chat, [&](std::string_view d) { post(SessionEvent::Kind::Delta, std::string(d)); }, cancel_);
    if (!response.ok) return ActionResult::error(response.error == "cancelled" ? "Cancelled." : response.error);

    ActionResult r;
    r.display = strip_thinking(response.content);
    if (r.display.empty()) return ActionResult::error("The model returned no text.");
    if (replace) r.inject_text = r.display;
    else r.speech = speakable(r.display);
    return r;
}

ActionResult Assistant::answer_from_screen_text(const std::string& question, const std::string& screen_text,
                                                const SessionRequest& request) {
    std::string error;
    LlmProvider* provider = providers_.select(false, error);
    if (!provider) return ActionResult::error(error);
    std::string text = screen_text;
    if (text.size() > 8000) text = text.substr(0, 8000) + "\n...[truncated]";

    post(SessionEvent::Kind::Reset);
    ChatRequest chat;
    chat.model = select_model(TaskClass::Chat, request.config);
    chat.max_tokens = request.config.llm.max_tokens;
    chat.messages.push_back(
        {"system",
         "The text below was read off the user's screen with OCR, so it may be noisy and lacks layout. "
         "Answer the user's question using only this text. If it does not contain the answer, say so.\n\n" + text,
         {}});
    chat.messages.push_back({"user", question.empty() ? "What is on my screen?" : question, {}});
    ChatResponse response = provider->chat(
        chat, [&](std::string_view d) { post(SessionEvent::Kind::Delta, std::string(d)); }, cancel_);
    if (!response.ok) return ActionResult::error(response.error == "cancelled" ? "Cancelled." : response.error);
    ActionResult r;
    r.display = strip_thinking(response.content);
    if (r.display.empty()) return ActionResult::error("The model returned an empty answer.");
    r.speech = speakable(r.display);
    return r;
}

ActionResult Assistant::describe_screen(const std::string& question, const SessionRequest& request) {
    std::string error;
    LlmProvider* provider = providers_.select(true, error);
    if (!provider) {
        // No vision model: read the window with OCR and answer from the text.
        std::string ocr_error;
        post(SessionEvent::Kind::Status, "Reading your screen\u2026");
        auto text = ocr_window(request.target.window, ocr_error);
        if (!text || text->empty()) return ActionResult::error(error);
        log::info("describe_screen: no vision provider, answering from {} OCR characters", text->size());
        return answer_from_screen_text(question, *text, request);
    }
    post(SessionEvent::Kind::Status, "Looking at your screen\u2026");
    auto png = capture_window_png_base64(request.target.window, 1280, error);
    if (!png) return ActionResult::error("Screen capture failed: " + error);
    post(SessionEvent::Kind::Reset);

    ChatRequest chat;
    chat.vision = true;
    chat.model = select_model(TaskClass::Vision, request.config);
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
