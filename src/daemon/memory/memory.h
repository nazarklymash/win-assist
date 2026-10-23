#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wa {

struct MemoryEntry {
    uint64_t id = 0;
    std::string text;
    // Free-form tag such as "user" or "selection"; shown when recalling.
    std::string source;
    // Local time string, for display only.
    std::string created;
    std::vector<float> embedding;
};

struct MemoryHit {
    MemoryEntry entry;
    float score = 0.0f;
};

// Append-only semantic note store backed by `memory.jsonl` in %APPDATA%.
// Embeddings come from the provider through the `embed` callback, so the store
// itself has no dependency on the LLM stack. Entries without an embedding are
// still searchable by substring.
class MemoryStore {
public:
    // Returns one vector per input, or nullopt when embedding is unavailable.
    using EmbedFn = std::function<std::optional<std::vector<std::vector<float>>>(const std::vector<std::string>&)>;

    void open(std::filesystem::path file, EmbedFn embed);

    // Stores `text`; returns false with `error` when it cannot be persisted.
    bool remember(const std::string& text, const std::string& source, std::string& error);
    // Best matches for `query`, strongest first. Falls back to substring
    // matching when embeddings are not available.
    std::vector<MemoryHit> recall(const std::string& query, size_t limit, float min_score);
    // Drops entries matching `query` (substring, case-insensitive). Empty query
    // clears everything. Returns how many were removed.
    size_t forget(const std::string& query, std::string& error);
    size_t size();

private:
    void load_locked();
    bool rewrite_locked(std::string& error);

    std::mutex mutex_;
    std::filesystem::path file_;
    EmbedFn embed_;
    std::vector<MemoryEntry> entries_;
    uint64_t next_id_ = 1;
    bool loaded_ = false;
};

}  // namespace wa
