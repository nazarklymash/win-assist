#include <doctest/doctest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>

#include "daemon/memory/memory.h"

using namespace wa;

namespace {

std::filesystem::path temp_memory_file(const wchar_t* tag) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    auto path = std::filesystem::path(dir) / (std::wstring(L"wa_test_memory_") + tag + L".jsonl");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// Deterministic stand-in for the embedding model: one dimension per keyword.
MemoryStore::EmbedFn keyword_embedder() {
    return [](const std::vector<std::string>& inputs) -> std::optional<std::vector<std::vector<float>>> {
        static const char* kWords[] = {"coffee", "deadline", "cat"};
        std::vector<std::vector<float>> out;
        for (const auto& text : inputs) {
            std::vector<float> v(3, 0.0f);
            for (size_t i = 0; i < 3; ++i) {
                if (text.find(kWords[i]) != std::string::npos) v[i] = 1.0f;
            }
            // Keep every vector non-zero so cosine stays defined.
            if (v[0] == 0 && v[1] == 0 && v[2] == 0) v = {0.1f, 0.1f, 0.1f};
            out.push_back(std::move(v));
        }
        return out;
    };
}

}  // namespace

TEST_CASE("remember and recall by similarity") {
    MemoryStore store;
    const auto file = temp_memory_file(L"similarity");
    store.open(file, keyword_embedder());

    std::string error;
    REQUIRE(store.remember("I drink coffee without sugar", "test", error));
    REQUIRE(store.remember("The deadline is on Friday", "test", error));
    CHECK(store.size() == 2);

    auto hits = store.recall("coffee", 5, 0.5f);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].entry.text == "I drink coffee without sugar");
    CHECK(hits[0].score > 0.5f);

    std::filesystem::remove(file);
}

TEST_CASE("entries survive reopening the store") {
    MemoryStore store;
    const auto file = temp_memory_file(L"persist");
    store.open(file, keyword_embedder());
    std::string error;
    REQUIRE(store.remember("my cat is called Mia", "test", error));

    MemoryStore reopened;
    reopened.open(file, keyword_embedder());
    CHECK(reopened.size() == 1);
    auto hits = reopened.recall("cat", 3, 0.5f);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].entry.text == "my cat is called Mia");

    std::filesystem::remove(file);
}

TEST_CASE("recall falls back to substring search without embeddings") {
    MemoryStore store;
    const auto file = temp_memory_file(L"substring");
    store.open(file, nullptr);
    std::string error;
    REQUIRE(store.remember("Project Zeta ships in March", "test", error));
    REQUIRE(store.remember("Unrelated note", "test", error));

    auto hits = store.recall("zeta", 5, 0.9f);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].entry.text == "Project Zeta ships in March");

    std::filesystem::remove(file);
}

TEST_CASE("forget removes matching entries and empty text is rejected") {
    MemoryStore store;
    const auto file = temp_memory_file(L"forget");
    store.open(file, keyword_embedder());

    std::string error;
    CHECK_FALSE(store.remember("   ", "test", error));
    CHECK_FALSE(error.empty());

    REQUIRE(store.remember("coffee beans from Kenya", "test", error));
    REQUIRE(store.remember("deadline moved to Monday", "test", error));
    CHECK(store.forget("coffee", error) == 1);
    CHECK(store.size() == 1);

    // The rewrite must survive a reload.
    MemoryStore reopened;
    reopened.open(file, keyword_embedder());
    CHECK(reopened.size() == 1);

    CHECK(reopened.forget("", error) == 1);
    CHECK(reopened.size() == 0);

    std::filesystem::remove(file);
}
