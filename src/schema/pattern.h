// A compiled `pattern`: the regular expression of a schema, checked against strings in time proportional to the
// length of the string.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "neomcp/json_schema.h"

namespace neomcp::json_schema::detail {

struct Pattern {
    // The text the schema holds, for messages.
    std::string source;

    Pattern();
    ~Pattern();
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;

    // Whether `text` contains a match (JSON Schema patterns are not anchored). The work is proportional to the length
    // of `text` and does not depend on the pattern's shape. Safe to call from several threads.
    bool search(std::string_view text) const;

    struct Engine;
    std::unique_ptr<Engine> engine;
};

// Translates `source` from the dialect of JSON Schema (ECMA-262 with the `u` flag) to RE2 and compiles it within
// `limits.max_pattern_memory`. Anything that cannot be matched exactly gives the reason, as the text that follows
// "pattern <source> " in a message; the length limit is the caller's to check.
std::variant<std::shared_ptr<const Pattern>, std::string> compile_pattern_text(const std::string& source,
                                                                               const Limits& limits);

}  // namespace neomcp::json_schema::detail
