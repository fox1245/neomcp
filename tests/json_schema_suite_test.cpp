// Runs the official JSON Schema validation test suite for the 2020-12 dialect (the files of validation/tests, and
// from optional/ the two files about regular expressions; not the rest of optional/ or proposals/) against the
// validator. The suite is data only: its files are parsed here and nothing of it is built.
//
// Which cases apply to a dialect is decided by the suite's own `compatibility` and `tags` properties, following the
// algorithm in the suite's README. Of the cases that apply, those the validator does not support by design are
// listed in the skips file next to this test, each with its reason. A skipped case is still compiled and validated,
// so that it must be refused or handled without a crash; only its verdict is not compared. The test fails when a
// skip entry matches nothing, and when fewer cases ran than the floor in the skips file.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"

namespace {

using neomcp::json;
using neomcp::json_schema::Issue;
using neomcp::json_schema::Validator;

namespace fs = std::filesystem;

json read_json(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream text;
    text << stream.rdbuf();
    return json::parse(text.str());
}

std::string string_member(const json& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

struct Skip {
    std::string file;
    std::string group;  // empty: every group of the file
    std::string test;   // empty: every test of the group
    std::string reason;
    bool used = false;
};

struct SkipList {
    std::size_t minimum_executed_tests = 0;
    std::vector<Skip> skips;
};

// The release the validator is tested as, in the numbering of the suite's `compatibility` property.
constexpr long dialect_release = 2020;

// The behaviours behind the suite's `tags` that the validator provides: `format` is an annotation.
bool provides_tag(const std::string& tag) { return tag == "format-annotation"; }

// The compatibility algorithm of the suite's README.
bool is_compatible(const std::string& compatibility, long release_under_test) {
    bool valid = true;
    std::size_t start = 0;
    while (start <= compatibility.size()) {
        auto end = compatibility.find(',', start);
        if (end == std::string::npos) end = compatibility.size();
        std::string term = compatibility.substr(start, end - start);
        start = end + 1;
        std::string op;
        if (term.rfind("<=", 0) == 0) {
            op = "<=";
            term.erase(0, 2);
        } else if (term.rfind('=', 0) == 0) {
            op = "=";
            term.erase(0, 1);
        }
        const long release = std::stol(term);
        if (op.empty()) valid = release_under_test >= release;
        if (op == "<=") valid = release_under_test <= release;
        if (op == "=") valid = release_under_test == release;
        if (release_under_test <= release) break;
    }
    return valid;
}

SkipList read_skips() {
    const json document = read_json(NEOMCP_JSON_SCHEMA_SKIPS_FILE);
    SkipList list;
    list.minimum_executed_tests = document.at("minimum_executed_tests").get<std::size_t>();
    for (const json& entry : document.at("skips")) {
        Skip skip;
        skip.file = string_member(entry, "file");
        skip.group = string_member(entry, "group");
        skip.test = string_member(entry, "test");
        skip.reason = string_member(entry, "reason");
        list.skips.push_back(std::move(skip));
    }
    return list;
}

struct Counts {
    std::size_t files = 0;
    std::size_t groups_not_applicable = 0;  // by compatibility or tags
    std::size_t groups_executed = 0;
    std::size_t groups_skipped = 0;
    std::size_t tests_executed = 0;
    std::size_t tests_skipped = 0;
};

// The skip entry that covers a group or a test, if any. `test` is empty when asking about the whole group.
Skip* find_skip(SkipList& list, const std::string& file, const std::string& group, const std::string* test) {
    for (Skip& skip : list.skips) {
        if (skip.file != file) continue;
        if (!skip.group.empty() && skip.group != group) continue;
        if (skip.test.empty()) return &skip;
        if (test && skip.test == *test) return &skip;
    }
    return nullptr;
}

TEST(JsonSchemaSuite, SkipFileIsWellFormed) {
    const SkipList list = read_skips();
    EXPECT_GT(list.minimum_executed_tests, 0U);
    for (const Skip& skip : list.skips) {
        EXPECT_FALSE(skip.file.empty()) << "a skip entry without a file";
        EXPECT_FALSE(skip.reason.empty()) << "the skip of " << skip.file << " / " << skip.group << " has no reason";
        EXPECT_TRUE(skip.test.empty() || !skip.group.empty())
            << "the skip of test " << skip.test << " in " << skip.file << " names no group";
    }
}

TEST(JsonSchemaSuite, Draft2020_12) {
    SkipList skips = read_skips();
    Counts counts;

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(NEOMCP_JSON_SCHEMA_SUITE_DIR)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "no test files in " << NEOMCP_JSON_SCHEMA_SUITE_DIR;
    // The optional files that test the dialect of the regular expressions of `pattern` and `patternProperties`.
    for (const char* name : {"ecmascript-regex.json", "non-bmp-regex.json"}) {
        const fs::path path = fs::path(NEOMCP_JSON_SCHEMA_SUITE_DIR) / "optional" / name;
        ASSERT_TRUE(fs::is_regular_file(path)) << path.string();
        files.push_back(path);
    }

    for (const fs::path& path : files) {
        const std::string file = path.filename().string();
        const json document = read_json(path);
        ASSERT_TRUE(document.is_object()) << file;
        ++counts.files;
        for (const json& group : document.at("tests")) {
            const std::string group_name = string_member(group, "description");
            const std::string compatibility = string_member(group, "compatibility");
            bool applies = compatibility.empty() || is_compatible(compatibility, dialect_release);
            if (const auto tags = group.find("tags"); tags != group.end()) {
                for (const json& tag : *tags) applies = applies && provides_tag(tag.get<std::string>());
            }
            if (!applies) {
                ++counts.groups_not_applicable;
                continue;
            }
            Skip* group_skip = find_skip(skips, file, group_name, nullptr);
            if (group_skip) group_skip->used = true;
            const bool group_skipped = group_skip != nullptr;
            (group_skipped ? counts.groups_skipped : counts.groups_executed) += 1;

            const auto compiled = Validator::compile(group.at("schema"));
            const Validator* validator = std::get_if<Validator>(&compiled);
            if (!validator && !group_skipped) {
                const Issue& issue = std::get<Issue>(compiled);
                ADD_FAILURE() << file << " / " << group_name << ": the schema is refused at " << issue.schema_pointer
                              << ": " << issue.message;
            }

            for (const json& test : group.at("tests")) {
                const std::string test_name = string_member(test, "description");
                Skip* test_skip = group_skipped ? group_skip : find_skip(skips, file, group_name, &test_name);
                if (test_skip) test_skip->used = true;
                // A skipped case still runs: it must not crash, whatever it answers.
                std::optional<Issue> issue;
                if (validator) issue = validator->validate(test.at("data"));
                if (group_skipped || test_skip) {
                    ++counts.tests_skipped;
                    continue;
                }
                ++counts.tests_executed;
                if (!validator) continue;
                const bool expected = test.at("valid").get<bool>();
                if (expected == !issue.has_value()) continue;
                ADD_FAILURE() << file << " / " << group_name << " / " << test_name << ": expected "
                              << (expected ? "valid" : "invalid") << ", got "
                              << (issue ? "invalid at " + issue->instance_pointer + " (" + issue->schema_pointer +
                                              ": " + issue->message + ")"
                                        : std::string("valid"));
            }
        }
    }

    for (const Skip& skip : skips.skips) {
        EXPECT_TRUE(skip.used) << "stale skip: " << skip.file << " / " << skip.group << " / " << skip.test
                               << " matches nothing in the suite";
    }
    EXPECT_GE(counts.tests_executed, skips.minimum_executed_tests)
        << "fewer tests ran than the floor in the skips file";

    std::cout << "JSON Schema suite: " << counts.files << " files, " << counts.groups_not_applicable
              << " groups not applicable to 2020-12 by compatibility or tags, " << counts.groups_executed
              << " groups executed, " << counts.groups_skipped << " groups skipped, " << counts.tests_executed << " tests executed, "
              << counts.tests_skipped << " tests skipped\n";
}

}  // namespace
