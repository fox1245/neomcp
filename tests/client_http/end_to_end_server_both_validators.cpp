// An application that takes both kinds of access token links both validators: the one that asks the authorization server
// about each token, and the one that checks signed tokens with its key set. This program is that application with
// nothing to serve: it makes each validator from empty options, which both refuse, and exits 0 when they did.
//
// It is built with the tests, so two validators that define one name with external linkage fail the build; the test
// ServerEndToEndBothValidatorsLink runs it.
#include <utility>

#include "neomcp/http/introspection.h"
#include "neomcp/http/jwt.h"

int main() {
    auto introspection = neomcp::http::make_introspection_validator(neomcp::http::IntrospectionOptions{});
    auto signed_tokens = neomcp::http::make_jwt_validator(neomcp::http::JwtOptions{});
    return introspection || signed_tokens ? 1 : 0;
}
