// The checker of the differential test of the pattern translator. It reads a file of cases (tests/pattern_oracle/
// generate.js writes them; tests/data/pattern_cases.json is the hand-written file in the same format), compiles
// {"type":"string","pattern":p} through the public interface of the validator, and compares it with the answers of the
// reference engine that the file holds:
//
//   - the reference says the pattern is a syntax error: the validator must refuse the schema;
//   - the validator accepts the pattern: every text must give the answer of the reference;
//   - the validator refuses a pattern that the reference accepts: allowed, and counted by the construct that its
//     message names, so that one can see that the run was not mostly refusals. The run fails when the share of the
//     patterns that reached a comparison is below a floor.
//
// Usage: neomcp_pattern_oracle <cases.json> [--min-compared-share <fraction>]
// Exit status: 0 all agree, 1 a difference or too few comparisons, 2 the file could not be read.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"

namespace {

using neomcp::json;
using neomcp::json_schema::Issue;
using neomcp::json_schema::Validator;

// Of the patterns the reference accepts, the share that must reach a comparison. A run of the generator with the
// grammar of generate.js reaches about 0.88; the floor leaves room for a change of the grammar and not for a
// translator that refuses most of what it is given.
constexpr double default_min_compared_share = 0.75;
// How many differences are printed in full; all are counted.
constexpr std::size_t max_reported = 40;

// What the validator says when a string does not match, and the parts of the messages of a refusal.
constexpr std::string_view no_match_prefix = "does not match pattern ";
constexpr std::string_view refusal_marker = " uses ";
constexpr std::string_view refusal_position = " at byte ";
constexpr std::string_view refusal_end = ", which cannot be matched exactly";
constexpr std::string_view engine_marker = "is not accepted by the regular expression engine: ";
constexpr std::string_view memory_marker = "max_pattern_memory";
constexpr std::string_view repeat_product_marker = "nested repeat counts multiply";

json read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + path);
    std::ostringstream text;
    text << stream.rdbuf();
    return json::parse(text.str());
}

// Printable ASCII as it is, every other code point as <U+XXXX>.
std::string escaped(const std::string& text) {
    std::string out;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        std::uint32_t rune = lead;
        if (lead >= 0xF0) {
            length = 4;
            rune = lead & 0x07u;
        } else if (lead >= 0xE0) {
            length = 3;
            rune = lead & 0x0Fu;
        } else if (lead >= 0xC0) {
            length = 2;
            rune = lead & 0x1Fu;
        }
        for (std::size_t i = 1; i < length && index + i < text.size(); ++i) {
            rune = (rune << 6) | (static_cast<unsigned char>(text[index + i]) & 0x3Fu);
        }
        index += length;
        if (rune >= 0x20 && rune < 0x7F) {
            out += static_cast<char>(rune);
        } else {
            std::ostringstream code;
            code << "<U+" << std::hex << std::uppercase << rune << ">";
            out += code.str();
        }
    }
    return out;
}

// The construct a refusal names, shortened to what does not depend on the pattern: the text between "uses" and "at
// byte", up to its first parenthesis. The message is "pattern <quoted source> uses <construct> at byte <n>, which
// cannot be matched exactly in time proportional to the string".
std::string refusal_key(const std::string& message) {
    const std::size_t end = message.rfind(refusal_end);
    if (end != std::string::npos) {
        const std::size_t position = message.rfind(refusal_position, end);
        const std::size_t marker = position == std::string::npos ? std::string::npos : message.rfind(refusal_marker, position);
        if (marker != std::string::npos) {
            std::string construct = message.substr(marker + refusal_marker.size(), position - marker - refusal_marker.size());
            const std::size_t paren = construct.find(" (");
            if (paren != std::string::npos) construct.resize(paren);
            return construct;
        }
    }
    return message.substr(0, 80);
}

enum class Outcome { compared, refused_by_translator, refused_by_memory_limit, refused_by_repeat_product, unexpected };

struct Classified {
    Outcome outcome;
    std::string key;
};

Classified classify_refusal(const Issue& issue) {
    const std::string& message = issue.message;
    if (message.find(refusal_marker) != std::string::npos && message.rfind(refusal_end) != std::string::npos) {
        return {Outcome::refused_by_translator, refusal_key(message)};
    }
    if (message.find(memory_marker) != std::string::npos) return {Outcome::refused_by_memory_limit, "the memory limit of a pattern"};
    if (message.find(engine_marker) != std::string::npos && message.find(repeat_product_marker) != std::string::npos) {
        return {Outcome::refused_by_repeat_product, "repeat counts that multiply past the engine limit"};
    }
    // Any other refusal is not a designed one: the translator wrote an expression that the engine does not accept, or
    // a limit that the generator keeps clear of was reached.
    return {Outcome::unexpected, message};
}

struct Report {
    std::size_t cases = 0;
    std::size_t reference_syntax_errors = 0;
    std::size_t syntax_errors_refused = 0;
    std::size_t accepted_by_reference = 0;
    std::size_t compared_patterns = 0;
    std::size_t compared_texts = 0;
    std::size_t texts_matched = 0;
    std::size_t differences = 0;
    std::size_t printed = 0;
    std::size_t first_failure = 0;  // the number of the case (counted from 1) of the first failure, 0 when none
    std::map<std::string, std::size_t> refusals;
    std::size_t refused_total = 0;

    void fail(const std::string& what) {
        ++differences;
        if (first_failure == 0) first_failure = cases;
        if (printed < max_reported) {
            std::cout << "FAILURE " << what << "\n";
            ++printed;
        }
    }
};

void check_case(const json& entry, Report& report) {
    ++report.cases;
    const std::string pattern = entry.at("pattern").get<std::string>();
    const json schema = json{{"type", "string"}, {"pattern", pattern}};
    auto compiled = Validator::compile(schema);
    const auto* validator = std::get_if<Validator>(&compiled);

    if (entry.contains("syntax_error") && entry.at("syntax_error").get<bool>()) {
        ++report.reference_syntax_errors;
        if (validator) {
            report.fail("the validator accepts a pattern that is a syntax error in Unicode mode: \"" + escaped(pattern) + "\"");
        } else {
            ++report.syntax_errors_refused;
        }
        return;
    }

    ++report.accepted_by_reference;
    if (!validator) {
        const Classified refusal = classify_refusal(std::get<Issue>(compiled));
        if (refusal.outcome == Outcome::unexpected) {
            report.fail("the validator refuses \"" + escaped(pattern) + "\" in a way that no construct explains: " + refusal.key);
            return;
        }
        ++report.refusals[refusal.key];
        ++report.refused_total;
        return;
    }

    ++report.compared_patterns;
    for (const json& item : entry.at("texts")) {
        const std::string text = item.at("text").get<std::string>();
        const bool expected = item.at("match").get<bool>();
        const std::optional<Issue> issue = validator->validate(json(text));
        const bool actual = !issue.has_value();
        if (issue && issue->message.rfind(no_match_prefix, 0) != 0) {
            report.fail("pattern \"" + escaped(pattern) + "\", text \"" + escaped(text) + "\": the validation ended with: " + issue->message);
            continue;
        }
        ++report.compared_texts;
        if (expected) ++report.texts_matched;
        if (actual != expected) {
            report.fail("pattern \"" + escaped(pattern) + "\", text \"" + escaped(text) + "\": reference " +
                        (expected ? "matches" : "does not match") + ", validator " + (actual ? "matches" : "does not match"));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    double min_share = default_min_compared_share;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--min-compared-share" && i + 1 < argc) {
            min_share = std::strtod(argv[++i], nullptr);
        } else if (path.empty()) {
            path = std::string(argument);
        } else {
            std::cerr << "usage: neomcp_pattern_oracle <cases.json> [--min-compared-share <fraction>]\n";
            return 2;
        }
    }
    if (path.empty()) {
        std::cerr << "usage: neomcp_pattern_oracle <cases.json> [--min-compared-share <fraction>]\n";
        return 2;
    }

    json document;
    try {
        document = read_file(path);
    } catch (const std::exception& error) {
        std::cerr << "neomcp_pattern_oracle: " << error.what() << "\n";
        return 2;
    }

    Report report;
    try {
        for (const json& entry : document.at("cases")) check_case(entry, report);
    } catch (const std::exception& error) {
        std::cerr << "neomcp_pattern_oracle: the file is not a file of cases: " << error.what() << "\n";
        return 2;
    }

    std::cout << "cases: " << report.cases << "\n"
              << "  syntax errors in the reference: " << report.reference_syntax_errors << ", refused by the validator: "
              << report.syntax_errors_refused << "\n"
              << "  accepted by the reference: " << report.accepted_by_reference << "\n"
              << "    compared: " << report.compared_patterns << " patterns, " << report.compared_texts << " texts ("
              << report.texts_matched << " matched)\n"
              << "    refused by the validator: " << report.refused_total << "\n";
    std::vector<std::pair<std::string, std::size_t>> rows(report.refusals.begin(), report.refusals.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    for (const auto& [construct, count] : rows) std::cout << "      " << count << "  " << construct << "\n";

    const double share = report.accepted_by_reference == 0
                             ? 1.0
                             : static_cast<double>(report.compared_patterns) / static_cast<double>(report.accepted_by_reference);
    std::cout << "  share of the accepted patterns that reached a comparison: " << share << " (floor " << min_share << ")\n";
    // A file without cases, or one in which nothing was compared, says nothing about the translator: it is a failure of
    // the run and not a pass.
    if (report.cases == 0) report.fail("the file holds no case");
    if (report.compared_texts == 0) report.fail("no text was compared");
    if (share < min_share) report.fail("too few patterns reached a comparison");
    std::cout << (report.differences == 0 ? "PASS" : "FAIL") << ": " << report.differences << " failures";
    if (report.first_failure != 0) std::cout << ", the first at case " << report.first_failure;
    std::cout << "\n";
    return report.differences == 0 ? 0 : 1;
}
