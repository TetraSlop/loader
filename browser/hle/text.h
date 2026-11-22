#pragma once

// Narrow and wide text as MultiByteToWideChar and WideCharToMultiByte convert them. Code page 932 (Shift-JIS, the
// game's ANSI and OEM code page) uses Microsoft's table; 65001 is UTF-8; anything else maps bytes to the first 256
// characters.

#include <cstdint>
#include <string>
#include <string_view>

namespace tmweb::hle::text
{

constexpr std::uint32_t kShiftJis = 932;
constexpr std::uint32_t kUtf8     = 65001;

// The code page a conversion uses: CP_ACP, CP_OEMCP and CP_THREAD_ACP are 932
auto codePage(std::uint32_t requested) -> std::uint32_t;

auto isLeadByte(std::uint32_t codePage, std::uint8_t byte) -> bool;

// invalid: set when a byte sequence has no character; it becomes U+30FB for 932 and U+FFFD for UTF-8, as Windows
// does
auto decode(std::uint32_t codePage, std::string_view bytes, bool& invalid) -> std::u16string;

// usedDefault: set when a character has no encoding and became fallback
auto encode(std::uint32_t codePage, std::u16string_view characters, char fallback, bool& usedDefault) -> std::string;

} // namespace tmweb::hle::text
