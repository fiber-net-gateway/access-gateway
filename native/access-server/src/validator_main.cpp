#include "validation/NativeValidatorProtocol.h"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--describe-config-limits") {
        std::cout << fiber::access_server::native_validator_config_limits_response() << '\n';
        return std::cout.good() ? 0 : 1;
    }
    if (argc != 1) {
        return 2;
    }
    std::string input;
    std::array<char, 4096> buffer{};
    bool too_large = false;
    while (std::cin) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (count == 0) {
            break;
        }
        if (input.size() > fiber::access_server::kNativeValidatorMaxProtocolInputBytes - count) {
            too_large = true;
            continue;
        }
        input.append(buffer.data(), count);
    }

    const std::string response = too_large ? fiber::access_server::native_validator_input_too_large_response()
                                           : fiber::access_server::process_native_validator_request(input);
    std::cout << response << '\n';
    return std::cout.good() ? 0 : 1;
}
