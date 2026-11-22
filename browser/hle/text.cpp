#include "hle/text.h"

#include "hle/cp932_table.h"

#include <array>
#include <memory>

namespace tmweb::hle::text
{
namespace
{

constexpr char16_t kShiftJisInvalid = 0x30FB; // KATAKANA MIDDLE DOT
constexpr char16_t kUtf8Invalid     = 0xFFFD;

auto doubleIndex(const std::uint8_t lead, const std::uint8_t trail) -> int
{
    if (trail < cp932::kTrailFirst || trail > cp932::kTrailLast)
    {
        return -1;
    }
    const auto row = lead <= 0x9F ? lead - 0x81 : lead - 0xE0 + (0xA0 - 0x81);
    return row * (cp932::kTrailLast - cp932::kTrailFirst + 1) + (trail - cp932::kTrailFirst);
}

// UTF-16 to Shift-JIS. Where several codes decode to one character, Windows encodes to JIS X 0208 first, then NEC
// row 13 (0x87), then the IBM extensions (0xFA-0xFC), and the NEC-selected IBM extensions (0xED-0xEE) last.
auto encoder() -> const std::array<std::uint16_t, 0x10000>&
{
    static const auto table = []()
    {
        auto out = std::make_unique<std::array<std::uint16_t, 0x10000>>();
        out->fill(cp932::kNone);
        for (auto byte = 0u; byte < 0x100; ++byte)
        {
            if (cp932::kSingle[byte] != cp932::kNone)
            {
                (*out)[cp932::kSingle[byte]] = static_cast<std::uint16_t>(byte);
            }
        }
        // The bytes the table leaves undefined round-trip through the characters decode gives them
        (*out)[0x80]   = 0x80;
        (*out)[0xF8F0] = 0xA0;
        for (auto byte = 0xFDu; byte <= 0xFF; ++byte)
        {
            (*out)[0xF8F1 + byte - 0xFD] = static_cast<std::uint16_t>(byte);
        }
        const auto claim = [&](const unsigned lead)
        {
            for (auto trail = 0u; trail < 0x100; ++trail)
            {
                const auto index = doubleIndex(static_cast<std::uint8_t>(lead), static_cast<std::uint8_t>(trail));
                const auto value = index < 0 ? cp932::kNone : cp932::kDouble[static_cast<std::size_t>(index)];
                if (value != cp932::kNone && (*out)[value] == cp932::kNone)
                {
                    (*out)[value] = static_cast<std::uint16_t>((lead << 8) | trail);
                }
            }
        };
        for (auto lead = 0x81u; lead <= 0xFC; ++lead)
        {
            if (isLeadByte(kShiftJis, static_cast<std::uint8_t>(lead)) && lead != 0xED && lead != 0xEE)
            {
                claim(lead);
            }
        }
        claim(0xED);
        claim(0xEE);
        return out;
    }();
    return *table;
}

void appendUtf8(std::string& out, const char32_t c)
{
    if (c < 0x80)
    {
        out.push_back(static_cast<char>(c));
    }
    else if (c < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else if (c < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

auto decodeUtf8(const std::string_view bytes, bool& invalid) -> std::u16string
{
    auto out = std::u16string{};
    for (auto i = std::size_t{}; i < bytes.size();)
    {
        const auto first = static_cast<std::uint8_t>(bytes[i]);
        const auto count = first < 0x80 ? 1u : (first >> 5) == 6 ? 2u
                                           : (first >> 4) == 14  ? 3u
                                           : (first >> 3) == 30  ? 4u
                                                                 : 0u;
        auto       c     = static_cast<char32_t>(count == 1 ? first : first & (0x7F >> count));
        auto       valid = count != 0 && i + count <= bytes.size();
        for (auto k = 1u; valid && k < count; ++k)
        {
            const auto next = static_cast<std::uint8_t>(bytes[i + k]);
            valid           = (next & 0xC0) == 0x80;
            c               = (c << 6) | (next & 0x3F);
        }
        if (!valid || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000))
        {
            invalid = true;
            out.push_back(kUtf8Invalid);
            ++i;
            continue;
        }
        if (c >= 0x10000)
        {
            out.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        }
        else
        {
            out.push_back(static_cast<char16_t>(c));
        }
        i += count;
    }
    return out;
}

} // namespace

auto codePage(const std::uint32_t requested) -> std::uint32_t
{
    return requested <= 3 ? kShiftJis : requested;
}

auto isLeadByte(const std::uint32_t codePage, const std::uint8_t byte) -> bool
{
    return codePage == kShiftJis && ((byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC));
}

auto decode(const std::uint32_t codePage, const std::string_view bytes, bool& invalid) -> std::u16string
{
    if (codePage == kUtf8)
    {
        return decodeUtf8(bytes, invalid);
    }
    auto out = std::u16string{};
    out.reserve(bytes.size());
    for (auto i = std::size_t{}; i < bytes.size(); ++i)
    {
        const auto byte = static_cast<std::uint8_t>(bytes[i]);
        if (codePage != kShiftJis)
        {
            out.push_back(byte);
            continue;
        }
        if (!isLeadByte(kShiftJis, byte))
        {
            // 0x80, 0xA0 and 0xFD-0xFF are undefined in the table; Windows passes 0x80 through and maps the others
            // to the private use area
            const auto single = cp932::kSingle[byte];
            out.push_back(single != cp932::kNone ? single : byte == 0x80 ? char16_t{ 0x80 }
                                                        : byte == 0xA0   ? char16_t{ 0xF8F0 }
                                                                         : static_cast<char16_t>(0xF8F1 + byte - 0xFD));
            continue;
        }
        const auto index = i + 1 < bytes.size() ? doubleIndex(byte, static_cast<std::uint8_t>(bytes[i + 1])) : -1;
        const auto value = index < 0 ? cp932::kNone : cp932::kDouble[static_cast<std::size_t>(index)];
        if (value == cp932::kNone)
        {
            invalid = true;
            out.push_back(kShiftJisInvalid);
            continue; // the trail byte is read again on its own, as Windows does
        }
        out.push_back(value);
        ++i;
    }
    return out;
}

auto encode(const std::uint32_t codePage, const std::u16string_view characters, const char fallback, bool& usedDefault) -> std::string
{
    auto out = std::string{};
    out.reserve(characters.size());
    for (auto i = std::size_t{}; i < characters.size(); ++i)
    {
        const auto c = characters[i];
        if (codePage == kUtf8)
        {
            auto point = static_cast<char32_t>(c);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < characters.size() && characters[i + 1] >= 0xDC00 && characters[i + 1] < 0xE000)
            {
                point = 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) + (characters[i + 1] - 0xDC00);
                ++i;
            }
            appendUtf8(out, point);
            continue;
        }
        if (codePage != kShiftJis)
        {
            usedDefault |= c >= 0x100;
            out.push_back(c < 0x100 ? static_cast<char>(c) : fallback);
            continue;
        }
        const auto code = encoder()[c];
        if (code == cp932::kNone)
        {
            usedDefault = true;
            out.push_back(fallback);
        }
        else if (code < 0x100)
        {
            out.push_back(static_cast<char>(code));
        }
        else
        {
            out.push_back(static_cast<char>(code >> 8));
            out.push_back(static_cast<char>(code & 0xFF));
        }
    }
    return out;
}

} // namespace tmweb::hle::text
