#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "model/ProductDocument.h"

namespace solidar {

namespace detail {
class AssemblyCommandHistoryTestAccess;
}

struct RootOccurrenceAddress final {
  ProductIncarnationId productIncarnation;
  DefinitionId rootDefinitionId;
  OccurrencePath path;
  DefinitionId expectedTargetDefinitionId;

  friend bool operator==(const RootOccurrenceAddress&,
                         const RootOccurrenceAddress&) = default;
};

struct RenameOccurrenceDelta final {
  std::string beforeUtf8;
  std::string afterUtf8;

  friend bool operator==(const RenameOccurrenceDelta&,
                         const RenameOccurrenceDelta&) = default;
};

struct VisibilityOccurrenceDelta final {
  bool beforeVisible{true};
  bool afterVisible{true};

  friend bool operator==(const VisibilityOccurrenceDelta&,
                         const VisibilityOccurrenceDelta&) = default;
};

struct GroundedOccurrenceDelta final {
  bool beforeGrounded{};
  bool afterGrounded{};

  friend bool operator==(const GroundedOccurrenceDelta&,
                         const GroundedOccurrenceDelta&) = default;
};

struct SuppressedOccurrenceDelta final {
  bool beforeSuppressed{};
  bool afterSuppressed{};

  friend bool operator==(const SuppressedOccurrenceDelta&,
                         const SuppressedOccurrenceDelta&) = default;
};

enum class OccurrenceDeltaCreateStatus {
  Created,
  InvalidBeforeName,
  InvalidAfterName,
  ResourceFailure,
};

struct OccurrenceDeltaCreateResult;

// Closed occurrence delta. A5c1-A5c4 add only authored occurrence rename,
// persistent visibility, grounding and suppression; all later semantics remain
// intentionally absent.
class OccurrenceDelta final {
 public:
  [[nodiscard]] static OccurrenceDelta empty(
      RootOccurrenceAddress address) {
    return OccurrenceDelta(std::move(address));
  }

  [[nodiscard]] static OccurrenceDeltaCreateResult rename(
      RootOccurrenceAddress address, std::string beforeUtf8,
      std::string afterUtf8) noexcept;
  [[nodiscard]] static OccurrenceDelta visibility(
      RootOccurrenceAddress address, bool beforeVisible,
      bool afterVisible) noexcept {
    return OccurrenceDelta(
        std::move(address),
        VisibilityOccurrenceDelta{beforeVisible, afterVisible});
  }
  [[nodiscard]] static OccurrenceDelta grounded(
      RootOccurrenceAddress address, bool beforeGrounded,
      bool afterGrounded) noexcept {
    return OccurrenceDelta(
        std::move(address),
        GroundedOccurrenceDelta{beforeGrounded, afterGrounded});
  }
  [[nodiscard]] static OccurrenceDelta suppressed(
      RootOccurrenceAddress address, bool beforeSuppressed,
      bool afterSuppressed) noexcept {
    return OccurrenceDelta(
        std::move(address),
        SuppressedOccurrenceDelta{beforeSuppressed, afterSuppressed});
  }

  [[nodiscard]] const RootOccurrenceAddress& address() const noexcept {
    return address_;
  }
  [[nodiscard]] constexpr bool isEmpty() const noexcept {
    return std::holds_alternative<std::monostate>(operation_) &&
           !syntheticForTesting_;
  }
  [[nodiscard]] const RenameOccurrenceDelta* rename() const noexcept {
    return std::get_if<RenameOccurrenceDelta>(&operation_);
  }
  [[nodiscard]] const VisibilityOccurrenceDelta* visibilityChange()
      const noexcept {
    return std::get_if<VisibilityOccurrenceDelta>(&operation_);
  }
  [[nodiscard]] const GroundedOccurrenceDelta* groundedChange()
      const noexcept {
    return std::get_if<GroundedOccurrenceDelta>(&operation_);
  }
  [[nodiscard]] const SuppressedOccurrenceDelta* suppressedChange()
      const noexcept {
    return std::get_if<SuppressedOccurrenceDelta>(&operation_);
  }

  friend bool operator==(const OccurrenceDelta&,
                         const OccurrenceDelta&) = default;

 private:
  friend class detail::AssemblyCommandHistoryTestAccess;

  explicit OccurrenceDelta(RootOccurrenceAddress address)
      : address_(std::move(address)) {}
  OccurrenceDelta(RootOccurrenceAddress address, bool syntheticForTesting)
      : address_(std::move(address)),
        syntheticForTesting_(syntheticForTesting) {}
  OccurrenceDelta(RootOccurrenceAddress address,
                  RenameOccurrenceDelta rename) noexcept
      : address_(std::move(address)), operation_(std::move(rename)) {}
  OccurrenceDelta(RootOccurrenceAddress address,
                  VisibilityOccurrenceDelta visibility) noexcept
      : address_(std::move(address)), operation_(visibility) {}
  OccurrenceDelta(RootOccurrenceAddress address,
                  GroundedOccurrenceDelta grounded) noexcept
      : address_(std::move(address)), operation_(grounded) {}
  OccurrenceDelta(RootOccurrenceAddress address,
                  SuppressedOccurrenceDelta suppressed) noexcept
      : address_(std::move(address)), operation_(suppressed) {}

  RootOccurrenceAddress address_;
  std::variant<std::monostate, RenameOccurrenceDelta,
               VisibilityOccurrenceDelta, GroundedOccurrenceDelta,
               SuppressedOccurrenceDelta>
      operation_;
  bool syntheticForTesting_{};
};

struct OccurrenceDeltaCreateResult final {
  OccurrenceDeltaCreateStatus status{OccurrenceDeltaCreateStatus::InvalidAfterName};
  std::optional<OccurrenceDelta> delta;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == OccurrenceDeltaCreateStatus::Created && delta.has_value();
  }
};

enum class OccurrenceDeltaStatus {
  Committed,
  NoChange,
  Busy,
  ProductIncarnationMismatch,
  RootDefinitionMismatch,
  RootIsNotAssembly,
  UnsupportedPathDepth,
  OccurrenceMissing,
  TargetDefinitionMismatch,
  InvalidName,
  CurrentValueMismatch,
  SuccessorRejected,
  ResourceFailure,
  UnexpectedFailure,
};

}  // namespace solidar
