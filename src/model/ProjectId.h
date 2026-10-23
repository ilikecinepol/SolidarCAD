#pragma once

#include <array>
#include <compare>
#include <cstdint>

namespace solidar {

// Stable identity of an identity-bearing product document.  The all-zero
// value is reserved as an invalid sentinel; standalone documents carry no
// ProjectId at all (std::nullopt in ProductDocument).
class ProjectId final {
 public:
  using Bytes = std::array<std::uint8_t, 16>;

  constexpr ProjectId() noexcept = default;
  explicit constexpr ProjectId(Bytes bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] constexpr const Bytes& bytes() const noexcept {
    return bytes_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept {
    for (const auto byte : bytes_) {
      if (byte != 0) return true;
    }
    return false;
  }

  friend constexpr bool operator==(ProjectId, ProjectId) noexcept = default;
  friend constexpr auto operator<=>(ProjectId, ProjectId) noexcept = default;

 private:
  Bytes bytes_{};
};

inline constexpr ProjectId kInvalidProjectId{};

}  // namespace solidar
