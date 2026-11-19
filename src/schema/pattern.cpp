#include "schema/pattern.h"

#include <cstdint>
#include <limits>
#include <utility>

#include <re2/re2.h>

#include "schema/pattern_translate.h"

namespace neomcp::json_schema::detail {

struct Pattern::Engine {
    Engine(const std::string& expression, const RE2::Options& options) : regex(re2::StringPiece(expression), options) {}
    RE2 regex;
};

Pattern::Pattern() = default;
Pattern::~Pattern() = default;

bool Pattern::search(std::string_view text) const {
    // No groups are captured, so the match is a decision on the text alone.
    return engine->regex.Match(re2::StringPiece(text.data(), text.size()), 0, text.size(), RE2::UNANCHORED, nullptr, 0);
}

std::variant<std::shared_ptr<const Pattern>, std::string> compile_pattern_text(const std::string& source,
                                                                               const Limits& limits) {
    auto translated = translate_pattern(source);
    if (const auto* refusal = std::get_if<PatternRefusal>(&translated)) {
        return "uses " + refusal->construct + " at byte " + std::to_string(refusal->position) +
               ", which cannot be matched exactly in time proportional to the string";
    }
    const std::string& expression = std::get<std::string>(translated);

    RE2::Options options;
    options.set_log_errors(false);
    options.set_never_capture(true);
    const std::uint64_t budget = limits.max_pattern_memory;
    options.set_max_mem(budget > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                            ? std::numeric_limits<std::int64_t>::max()
                            : static_cast<std::int64_t>(budget));

    auto pattern = std::make_shared<Pattern>();
    pattern->source = source;
    pattern->engine = std::make_unique<Pattern::Engine>(expression, options);
    const RE2& regex = pattern->engine->regex;
    if (!regex.ok()) {
        if (regex.error_code() == RE2::ErrorPatternTooLarge) {
            return "needs more memory than the limit max_pattern_memory allows (" +
                   std::to_string(limits.max_pattern_memory) + " bytes)";
        }
        std::string reason = "is not accepted by the regular expression engine: " + regex.error();
        if (regex.error_code() == RE2::ErrorRepeatSize) reason += " (nested repeat counts multiply to more than 1000)";
        return reason;
    }
    return std::shared_ptr<const Pattern>(std::move(pattern));
}

}  // namespace neomcp::json_schema::detail
