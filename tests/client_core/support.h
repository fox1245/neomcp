// Shared by the tests of the client half of the protocol core: a small deterministic generator of the model's
// values, and field-complete equality for them.
//
// The generator is seeded and reads no clock. Its numbers come from the raw output of one fixed generator
// (SplitMix64) reduced by hand, never from std::uniform_*_distribution, whose output differs between standard
// libraries; so the values are the same on every platform.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/wire.h"

namespace client_core_test {

using neomcp::json;

class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // 0 to n - 1; n is positive.
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
    bool chance() { return (next() & 1U) != 0; }

private:
    std::uint64_t state_;
};

// How much of a value is filled in.
enum class Fill {
    none,  // every optional member absent, every list empty, every string empty
    all,   // every optional member present, every list holds items
    any    // chosen at random, member by member
};

class Generator {
public:
    // `project`: generate only the members the profile's type of each value has, so that a value is equal to what a
    // reader returns; without it every member of the model is generated, also the ones a revision does not write.
    Generator(const neomcp::profile::Profile& profile, std::uint64_t seed, Fill fill, bool project)
        : profile_(profile), random_(seed), fill_(fill), project_(project) {}

    neomcp::Icon icon();
    neomcp::Implementation implementation();
    neomcp::ToolAnnotations tool_annotations();
    neomcp::ToolDefinition tool();
    neomcp::ContentAnnotations content_annotations();
    neomcp::ContentBlock block();
    neomcp::ContentBlock block(neomcp::ContentKind kind);
    neomcp::ResourceContents resource_contents();
    neomcp::ResourceDefinition resource();
    neomcp::ResourceTemplateDefinition resource_template();
    neomcp::ReadResult read_result();
    neomcp::PromptDefinition prompt();
    neomcp::PromptMessage prompt_message();
    neomcp::PromptResult prompt_result();
    neomcp::CompletionResult completion();
    neomcp::ToolResult tool_result();

    // Pieces the input tests use too.
    std::string text();
    json object(int depth = 2);
    std::uint64_t count_value();
    double real();
    Random& random() { return random_; }
    bool some();
    std::size_t items(std::size_t most);

private:
    // Whether the value of the type may have the member: the profile's type has it (a profile without the type
    // holds nothing back, like the writers).
    bool has(std::string_view type_name, std::string_view member) const;
    bool on(std::string_view type_name, std::string_view member) { return (!project_ || has(type_name, member)) && some(); }
    json scalar();
    json value(int depth);
    std::optional<json> meta(std::string_view type_name);
    json extra();
    neomcp::Role role();
    std::vector<neomcp::Icon> icons(std::string_view type_name);
    std::optional<neomcp::ContentAnnotations> annotations(std::string_view type_name);
    std::optional<std::string> optional_text(std::string_view type_name, std::string_view member);
    void fill_common(std::optional<neomcp::ContentAnnotations>& annotation_slot, std::optional<json>& meta_slot,
                     json& extra_slot, std::string_view type_name);

    const neomcp::profile::Profile& profile_;
    Random random_;
    Fill fill_;
    bool project_;
};

// ---- Equality, field by field

bool same(const json& a, const json& b);
bool same(const neomcp::Icon& a, const neomcp::Icon& b);
bool same(const neomcp::Implementation& a, const neomcp::Implementation& b);
bool same(const neomcp::ToolAnnotations& a, const neomcp::ToolAnnotations& b);
bool same(const neomcp::ToolDefinition& a, const neomcp::ToolDefinition& b);
bool same(const neomcp::ContentAnnotations& a, const neomcp::ContentAnnotations& b);
bool same(const neomcp::ContentBlock& a, const neomcp::ContentBlock& b);
bool same(const neomcp::ResourceContents& a, const neomcp::ResourceContents& b);
bool same(const neomcp::ResourceDefinition& a, const neomcp::ResourceDefinition& b);
bool same(const neomcp::ResourceTemplateDefinition& a, const neomcp::ResourceTemplateDefinition& b);
bool same(const neomcp::ReadResult& a, const neomcp::ReadResult& b);
bool same(const neomcp::PromptDefinition& a, const neomcp::PromptDefinition& b);
bool same(const neomcp::PromptMessage& a, const neomcp::PromptMessage& b);
bool same(const neomcp::PromptResult& a, const neomcp::PromptResult& b);
bool same(const neomcp::CompletionResult& a, const neomcp::CompletionResult& b);
bool same(const neomcp::ToolResult& a, const neomcp::ToolResult& b);

// The members of the types above that are not themselves compared by a function above.
template <typename T>
bool same(const T& a, const T& b) {
    return a == b;
}

template <typename T>
bool same(const std::optional<T>& a, const std::optional<T>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || same(*a, *b);
}

template <typename T>
bool same(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same(a[i], b[i])) return false;
    }
    return true;
}

}  // namespace client_core_test
