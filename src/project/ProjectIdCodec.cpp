#include "project/internal/ProjectIdCodec.h"

#include <array>

namespace solidar::project::detail {

std::optional<ProjectId> parseCanonicalProjectId(
    std::string_view text) noexcept {
  if (text.size() != 36) return std::nullopt;
  constexpr std::array<std::size_t, 4> kHyphens{8, 13, 18, 23};
  for (const std::size_t position : kHyphens) {
    if (text[position] != '-') return std::nullopt;
  }

  ProjectId::Bytes bytes{};
  std::size_t nibble = 0;
  const auto decodeHex = [](char character) noexcept -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f')
      return 10 + character - 'a';
    return -1;
  };
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const int value = decodeHex(text[index]);
    if (value < 0) return std::nullopt;
    const std::size_t byteIndex = nibble / 2;
    if ((nibble % 2) == 0)
      bytes[byteIndex] = static_cast<std::uint8_t>(value << 4);
    else
      bytes[byteIndex] |= static_cast<std::uint8_t>(value);
    ++nibble;
  }
  if (nibble != 32) return std::nullopt;
  ProjectId id{bytes};
  return id.isValid() ? std::optional<ProjectId>{id} : std::nullopt;
}

std::optional<std::string> formatCanonicalProjectId(ProjectId id) {
  if (!id.isValid()) return std::nullopt;
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index < id.bytes().size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10)
      text.push_back('-');
    const std::uint8_t byte = id.bytes()[index];
    text.push_back(kHex[(byte >> 4) & 0x0f]);
    text.push_back(kHex[byte & 0x0f]);
  }
  return text;
}

}  // namespace solidar::project::detail
