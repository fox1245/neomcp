// Scope values (OAuth 2.1, section 1.4.1): which ones a flow asks for, and how a step-up extends them.
//
// A scope value is one scope-token: one or more of the characters %x21, %x23-5B and %x5D-7E. Values compare exactly
// (they are case-sensitive). A list keeps the order in which values were first seen and holds each value once.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "client_auth/bounds.h"
#include "client_auth/types.h"
#include "neomcp/authorization.h"

namespace neomcp::detail::oauth {

inline constexpr std::string_view offline_access_scope = "offline_access";

// The one check of the library (neomcp/authorization.h).
using neomcp::is_scope_token;

// The values that are scope-tokens, each once, in first-seen order, within `max_scopes` values and `max_scope_bytes`
// bytes when joined with spaces. What is left out is reported.
std::vector<std::string> bounded_scopes(std::span<const std::string> values, const OAuthBounds& bounds,
                                        std::vector<FlowDiagnostic>& diagnostics);

// The values of `first`, then those of `second` not already in it.
std::vector<std::string> scope_union(std::span<const std::string> first, std::span<const std::string> second);

// Every value of `smaller` is in `larger`, and `larger` has at least one more.
bool is_strict_superset(std::span<const std::string> larger, std::span<const std::string> smaller);

bool contains_scope(std::span<const std::string> values, std::string_view value);

// The values joined with single spaces.
std::string join_scopes(std::span<const std::string> values);

// The values of a space-delimited list, empty pieces dropped.
std::vector<std::string> split_scopes(std::string_view text);

}  // namespace neomcp::detail::oauth
