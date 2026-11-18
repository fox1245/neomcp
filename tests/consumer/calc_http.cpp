#include <cstdint>
#include <optional>
#include <string>

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
        options.port = static_cast<std::uint16_t>(std::stoi(argv[1]));
    }
    return neomcp::run_http(server, options);
}
