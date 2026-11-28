#ifndef FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_YAML_H
#define FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_YAML_H

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace fiber::access_server {

struct LocalFileYamlError {
    std::size_t line = 0;
    std::string message;
};

// Converts a bounded YAML 1.2 subset (block/flow mappings & sequences, block
// scalars, quoted/plain scalars, comments) into canonical JSON text suitable
// for AccessConfigCodec. Never throws; enforces bounded nesting depth and
// output size.
[[nodiscard]] std::expected<std::string, LocalFileYamlError>
convert_yaml_to_json(std::string_view yaml, std::size_t max_output_bytes = 16U << 20U);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_LOCAL_FILE_CONFIG_YAML_H
