#include "daemon/memory/memory.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "common/strings.h"

namespace wa {
namespace {

std::string fold_case(const std::string& text) {
    std::wstring w = to_wide(text);
    if (!w.empty()) CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    return to_utf8(w);
}

std::string local_time_string() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    if (na <= 0 || nb <= 0) return 0.0f;
    return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
}

}  // namespace

void MemoryStore::open(std::filesystem::path file, EmbedFn embed) {
    std::lock_guard lock(mutex_);
    file_ = std::move(file);
    embed_ = std::move(embed);
    loaded_ = false;
    entries_.clear();
    next_id_ = 1;
}

void MemoryStore::load_locked() {
    if (loaded_) return;
    loaded_ = true;
    std::error_code ec;
    if (file_.empty() || !std::filesystem::exists(file_, ec)) return;
    std::ifstream in(file_, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        MemoryEntry e;
        e.id = j.value("id", 0ull);
        e.text = j.value("text", "");
        e.source = j.value("source", "");
        e.created = j.value("created", "");
        e.embedding = j.value("embedding", std::vector<float>{});
        if (e.text.empty()) continue;
        next_id_ = std::max(next_id_, e.id + 1);
        entries_.push_back(std::move(e));
    }
    log::info("memory: {} entries loaded", entries_.size());
}

bool MemoryStore::remember(const std::string& text, const std::string& source, std::string& error) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        error = "Nothing to remember.";
        return false;
    }
    std::lock_guard lock(mutex_);
    load_locked();
    if (file_.empty()) {
        error = "Memory file is not configured.";
        return false;
    }

    MemoryEntry e;
    e.id = next_id_++;
    e.text = trimmed;
    e.source = source;
    e.created = local_time_string();
    if (embed_) {
        if (auto vectors = embed_({trimmed}); vectors && !vectors->empty()) e.embedding = (*vectors)[0];
        else log::warn("memory: embedding unavailable, entry stored for substring search only");
    }

    nlohmann::json j = {{"id", e.id}, {"text", e.text}, {"source", e.source}, {"created", e.created}};
    if (!e.embedding.empty()) j["embedding"] = e.embedding;

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::ofstream out(file_, std::ios::binary | std::ios::app);
    if (!out) {
        error = "Could not write the memory file.";
        return false;
    }
    out << j.dump() << '\n';
    entries_.push_back(std::move(e));
    return true;
}

std::vector<MemoryHit> MemoryStore::recall(const std::string& query, size_t limit, float min_score) {
    std::lock_guard lock(mutex_);
    load_locked();
    if (entries_.empty() || limit == 0) return {};

    std::vector<float> q;
    if (embed_ && !query.empty()) {
        if (auto vectors = embed_({query}); vectors && !vectors->empty()) q = (*vectors)[0];
    }

    std::vector<MemoryHit> hits;
    if (!q.empty()) {
        for (const auto& e : entries_) {
            if (e.embedding.empty()) continue;
            const float score = cosine(q, e.embedding);
            if (score >= min_score) hits.push_back({e, score});
        }
    }
    if (hits.empty()) {
        // No embeddings (or nothing similar): fall back to substring matching.
        const std::string needle = fold_case(query);
        for (const auto& e : entries_) {
            if (needle.empty() || fold_case(e.text).find(needle) != std::string::npos) hits.push_back({e, 0.0f});
        }
        // Newest first when we have no similarity signal.
        std::reverse(hits.begin(), hits.end());
        if (hits.size() > limit) hits.resize(limit);
        return hits;
    }

    std::sort(hits.begin(), hits.end(), [](const MemoryHit& a, const MemoryHit& b) { return a.score > b.score; });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

bool MemoryStore::rewrite_locked(std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const auto tmp = file_.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not rewrite the memory file.";
            return false;
        }
        for (const auto& e : entries_) {
            nlohmann::json j = {{"id", e.id}, {"text", e.text}, {"source", e.source}, {"created", e.created}};
            if (!e.embedding.empty()) j["embedding"] = e.embedding;
            out << j.dump() << '\n';
        }
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        error = "Could not replace the memory file.";
        return false;
    }
    return true;
}

size_t MemoryStore::forget(const std::string& query, std::string& error) {
    std::lock_guard lock(mutex_);
    load_locked();
    const size_t before = entries_.size();
    if (query.empty()) {
        entries_.clear();
    } else {
        const std::string needle = fold_case(query);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const MemoryEntry& e) {
                                          return fold_case(e.text).find(needle) != std::string::npos;
                                      }),
                       entries_.end());
    }
    const size_t removed = before - entries_.size();
    if (removed && !rewrite_locked(error)) return 0;
    return removed;
}

size_t MemoryStore::size() {
    std::lock_guard lock(mutex_);
    load_locked();
    return entries_.size();
}

}  // namespace wa
