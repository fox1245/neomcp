// The authorization server's public keys: a JWK Set read into keys of OpenSSL's libcrypto, each bound to the one
// algorithm it serves, and the check of a signature with one of them. Internal to neomcp::server_auth_jwt;
// include/neomcp/http/jwt.h says which keys are used.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/http/jwt.h"

namespace neomcp::http::detail {

// One key of the set, read and checked; immutable, and safe to use from several threads at once.
class PublicKey {
public:
    ~PublicKey();
    PublicKey(const PublicKey&) = delete;
    PublicKey& operator=(const PublicKey&) = delete;

    // Whether `signature` (as the JWS carries it, RFC 7518, section 3) signs `signing_input` with this key and its
    // algorithm.
    bool verifies(std::string_view signing_input, std::string_view signature) const;

    JwtAlgorithm algorithm() const noexcept { return algorithm_; }

private:
    struct Handle;
    friend struct KeyMaker;
    PublicKey(JwtAlgorithm algorithm, std::unique_ptr<Handle> handle);

    JwtAlgorithm algorithm_;
    std::unique_ptr<Handle> handle_;
};

struct KeyEntry {
    std::optional<std::string> kid;
    std::shared_ptr<const PublicKey> key;
};

// The keys of one fetched set that fit the algorithms the validator accepts.
class KeySet {
public:
    explicit KeySet(std::vector<KeyEntry> entries) : entries_(std::move(entries)) {}

    // The keys for a token with `algorithm` and `kid` (every key of that algorithm when the token names no `kid`).
    std::vector<std::shared_ptr<const PublicKey>> candidates(JwtAlgorithm algorithm,
                                                             const std::optional<std::string>& kid) const;

    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<KeyEntry> entries_;
};

// The key set a JWK Set document holds: nothing when the body is not one JSON object with a `keys` array (or names a
// member twice). Of the first `max_keys` entries of the array, those that cannot be used are left out (a set may
// hold no key the validator can use).
std::optional<KeySet> read_key_set(std::string_view body, std::span<const JwtAlgorithm> allowed, std::size_t max_keys);

}  // namespace neomcp::http::detail
