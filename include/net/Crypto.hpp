#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace net {

// Minimal primitives needed for the RFC 6455 opening handshake.
std::array<std::uint8_t, 20> sha1(std::string_view data);
std::string base64Encode(const std::uint8_t* data, std::size_t len);
std::string webSocketAcceptKey(std::string_view clientKey);

}  // namespace net
