#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <neomcp/http_server.h>
#include <neomcp/server.h>

struct AddArgs {
    int a;
    int b;
    std::optional<std::string> note;
};
NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend", note, "a note to echo back")

int main(int argc, char** argv) {
    neomcp::Server server({.name = "calc", .version = "1.0.0"});
    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });

    neomcp::HttpServerOptions options;
    options.port = 8000;
    if (argc > 1) {
        const std::string_view text = argv[1];
        const char* const end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, options.port);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            std::cerr << "calc_http: \"" << text << "\" is not a port (0 to 65535)\n";
            return EXIT_FAILURE;
        }
    }
    return neomcp::run_http(server, options);
}
