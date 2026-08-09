#include "validation/NativeValidatorProtocol.h"

#include <array>
#include <iostream>
#include <string>

int main() {
  std::string input;
  std::array<char, 4096> buffer{};
  bool too_large = false;
  while (std::cin) {
    std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(std::cin.gcount());
    if (count == 0) {
      break;
    }
    if (input.size() >
        fiber::access_server::kNativeValidatorMaxProtocolInputBytes - count) {
      too_large = true;
      continue;
    }
    input.append(buffer.data(), count);
  }

  const std::string response =
      too_large
          ? fiber::access_server::native_validator_input_too_large_response()
          : fiber::access_server::process_native_validator_request(input);
  std::cout << response << '\n';
  return std::cout.good() ? 0 : 1;
}
