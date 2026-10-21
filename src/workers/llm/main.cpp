// wa-llm: llama.cpp text-generation worker.
//
// Protocol (frames over the daemon's named pipe):
//   worker -> {"event":"ready","ok":true,"model":"..."}
//   daemon -> {"op":"chat","messages":[{"role":"user","content":"..."}],
//              "max_tokens":512,"json":false,"stream":true}
//   worker -> {"delta":"..."}*  then  {"done":true,"ok":true,"text":"..."}
//   daemon -> {"op":"cancel"}   (may arrive while generating)
//   daemon -> {"op":"shutdown"}
// The worker exits when the pipe closes.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include <llama.h>
#include <nlohmann/json.hpp>

#include "common/pipe.h"
#include "workers/worker_common.h"

namespace {

using nlohmann::json;

constexpr const char* kJsonGrammar = R"GBNF(
root   ::= object
value  ::= object | array | string | number | ("true" | "false" | "null") ws
object ::= "{" ws ( string ":" ws value ("," ws string ":" ws value)* )? "}" ws
array  ::= "[" ws ( value ("," ws value)* )? "]" ws
string ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4}) )* "\"" ws
number ::= ("-"? ([0-9] | [1-9] [0-9]{0,15})) ("." [0-9]+)? ([eE] [-+]? [0-9] [1-9]{0,15})? ws
ws     ::= | " " | "\n" [ \t]{0,20}
)GBNF";

void quiet_log(enum ggml_log_level level, const char* text, void*) {
    if (level >= GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

bool send_json(HANDLE pipe, const json& j) { return wa::ipc::write_frame(pipe, j.dump()); }

// Length of the longest prefix of `s` that ends on a UTF-8 character boundary.
size_t complete_utf8_prefix(const std::string& s) {
    size_t i = s.size();
    size_t back = 0;
    while (i > 0 && back < 4) {
        unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0xC0) != 0x80) {
            size_t need = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            return (back + 1 >= need) ? s.size() : i - 1;
        }
        --i;
        ++back;
    }
    return s.size();
}

bool cancel_requested(HANDLE pipe) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return true;
    if (available == 0) return false;
    std::string frame;
    if (!wa::ipc::read_frame(pipe, frame)) return true;
    json j = json::parse(frame, nullptr, false);
    return !j.is_discarded() && (j.value("op", "") == "cancel" || j.value("op", "") == "shutdown");
}

class Engine {
public:
    bool load(const std::string& path, int n_ctx, int n_gpu_layers, int threads, std::string& error) {
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = n_gpu_layers;
        mparams.progress_callback = [](float, void*) { return true; };
        model_ = llama_model_load_from_file(path.c_str(), mparams);
        if (!model_) {
            error = "failed to load model: " + path;
            return false;
        }
        vocab_ = llama_model_get_vocab(model_);
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = static_cast<uint32_t>(n_ctx);
        cparams.n_batch = 512;
        cparams.n_threads = threads;
        cparams.n_threads_batch = threads;
        cparams.no_perf = true;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            error = "failed to create llama context";
            return false;
        }
        const char* tmpl = llama_model_chat_template(model_, nullptr);
        template_ = tmpl ? tmpl : "";
        return true;
    }

    ~Engine() {
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
    }

    std::string build_prompt(const json& messages) const {
        std::vector<std::string> roles;
        std::vector<std::string> contents;
        for (const auto& m : messages) {
            roles.push_back(m.value("role", "user"));
            contents.push_back(m.value("content", ""));
        }
        std::vector<llama_chat_message> chat;
        for (size_t i = 0; i < roles.size(); ++i) chat.push_back({roles[i].c_str(), contents[i].c_str()});

        auto apply = [&](const char* tmpl) -> std::string {
            std::vector<char> buf(4096);
            int n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
            if (n < 0) return {};
            if (n > static_cast<int>(buf.size())) {
                buf.resize(static_cast<size_t>(n));
                n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
            }
            return std::string(buf.data(), static_cast<size_t>(n));
        };
        std::string prompt = template_.empty() ? std::string() : apply(template_.c_str());
        if (prompt.empty()) prompt = apply("chatml");
        // Thinking models (Qwen3 style): pre-fill an empty reasoning block to
        // skip slow chain-of-thought, matching enable_thinking=false.
        if (template_.find("<think>") != std::string::npos) prompt += "<think>\n\n</think>\n\n";
        return prompt;
    }

    json generate(HANDLE pipe, const json& request) {
        const bool want_json = request.value("json", false);
        const bool stream = request.value("stream", true);
        const int max_tokens = request.value("max_tokens", 512);
        const std::string prompt = build_prompt(request.value("messages", json::array()));

        llama_memory_clear(llama_get_memory(ctx_), true);

        std::vector<llama_token> tokens(prompt.size() + 16);
        int n = llama_tokenize(vocab_, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                               static_cast<int32_t>(tokens.size()), true, true);
        if (n < 0) {
            tokens.resize(static_cast<size_t>(-n));
            n = llama_tokenize(vocab_, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                               static_cast<int32_t>(tokens.size()), true, true);
        }
        tokens.resize(static_cast<size_t>(n));
        const int n_ctx = static_cast<int>(llama_n_ctx(ctx_));
        if (n + 8 >= n_ctx) return {{"done", true}, {"ok", false}, {"error", "prompt exceeds context window"}};

        const int n_batch = static_cast<int>(llama_n_batch(ctx_));
        for (int i = 0; i < n; i += n_batch) {
            int count = std::min(n_batch, n - i);
            if (llama_decode(ctx_, llama_batch_get_one(tokens.data() + i, count)) != 0) {
                return {{"done", true}, {"ok", false}, {"error", "llama_decode failed on prompt"}};
            }
        }

        llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        if (want_json) llama_sampler_chain_add(sampler, llama_sampler_init_grammar(vocab_, kJsonGrammar, "root"));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(want_json ? 0.2f : 0.7f));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

        std::string text;
        std::string pending;
        bool cancelled = false;
        int produced = 0;
        int pos = n;
        while (produced < max_tokens && pos < n_ctx - 1) {
            llama_token token = llama_sampler_sample(sampler, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, token)) break;
            char piece[256];
            int len = llama_token_to_piece(vocab_, token, piece, sizeof(piece), 0, false);
            if (len > 0) {
                text.append(piece, static_cast<size_t>(len));
                pending.append(piece, static_cast<size_t>(len));
                size_t ready = complete_utf8_prefix(pending);
                if (stream && ready > 0) {
                    if (!send_json(pipe, {{"delta", pending.substr(0, ready)}})) {
                        cancelled = true;
                        break;
                    }
                    pending.erase(0, ready);
                }
            }
            if (cancel_requested(pipe)) {
                cancelled = true;
                break;
            }
            if (llama_decode(ctx_, llama_batch_get_one(&token, 1)) != 0) break;
            ++produced;
            ++pos;
        }
        llama_sampler_free(sampler);
        if (cancelled) return {{"done", true}, {"ok", false}, {"error", "cancelled"}};
        return {{"done", true}, {"ok", true}, {"text", text}};
    }

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    std::string template_;
};

}  // namespace

int main() {
    auto args = wa::worker::parse_args();
    HANDLE pipe = wa::worker::connect_pipe(wa::worker::arg_str(args, "pipe"));
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "wa-llm: could not connect to pipe\n");
        return 2;
    }

    llama_log_set(quiet_log, nullptr);
    llama_backend_init();

    const int threads = wa::worker::arg_int(args, "threads", 0) > 0 ? wa::worker::arg_int(args, "threads", 0)
                                                                     : wa::worker::default_threads();
    const std::string model = wa::worker::arg_str(args, "model");
    std::string error;
    {
        Engine engine;
        if (!engine.load(model, wa::worker::arg_int(args, "ctx", 4096), wa::worker::arg_int(args, "gpu-layers", 0),
                         threads, error)) {
            send_json(pipe, {{"event", "ready"}, {"ok", false}, {"error", error}});
            CloseHandle(pipe);
            return 3;
        }
        send_json(pipe, {{"event", "ready"}, {"ok", true}, {"model", model}});

        std::string frame;
        while (wa::ipc::read_frame(pipe, frame)) {
            json request = json::parse(frame, nullptr, false);
            if (request.is_discarded()) break;
            const std::string op = request.value("op", "");
            if (op == "shutdown") break;
            if (op == "cancel") continue;
            if (op != "chat") {
                send_json(pipe, {{"done", true}, {"ok", false}, {"error", "unknown op"}});
                continue;
            }
            if (!send_json(pipe, engine.generate(pipe, request))) break;
        }
    }
    llama_backend_free();
    CloseHandle(pipe);
    return 0;
}
