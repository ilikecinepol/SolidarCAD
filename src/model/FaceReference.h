#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "model/TopologyLineage.h"

namespace solidar {

enum class FaceReferenceVersion : std::uint8_t { V1, V2 };

// Closed carrier for persistent face identities. The active alternative is
// authoritative: a reference can never contain a mixture of v1 geometric
// fallback data and v2 operation provenance.
class FaceReference final {
 public:
  using Storage = std::variant<TopologyReference, TopologyReferenceV2>;

  // Compatibility default: an invalid v1 face reference.
  FaceReference() noexcept;
  FaceReference(BodyId bodyId, FeatureId featureId, std::size_t faceIndex,
                std::string persistentTag = {},
                std::optional<FaceSignature> signature = {});

  [[nodiscard]] static FaceReference fromV1(TopologyReference reference);
  [[nodiscard]] static FaceReference fromV2(TopologyReferenceV2 reference);

  [[nodiscard]] FaceReferenceVersion version() const noexcept;
  [[nodiscard]] bool isV1() const noexcept;
  [[nodiscard]] bool isV2() const noexcept;
  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId featureId() const noexcept;
  [[nodiscard]] const TopologyReference* v1() const noexcept;
  [[nodiscard]] const TopologyReferenceV2* v2() const noexcept;
  // Explicit v1-only compatibility accessors. They throw
  // std::bad_variant_access for v2 instead of silently projecting v2 into a
  // weaker v1 identity.
  [[nodiscard]] const std::size_t& faceIndex() const;
  [[nodiscard]] std::size_t& faceIndex();
  [[nodiscard]] const std::string& persistentTag() const;
  [[nodiscard]] std::string& persistentTag();
  [[nodiscard]] const std::optional<FaceSignature>& signature() const;
  [[nodiscard]] std::optional<FaceSignature>& signature();
  [[nodiscard]] const Storage& storage() const noexcept;

  friend bool operator==(const FaceReference&, const FaceReference&) = default;

 private:
  explicit FaceReference(Storage storage) noexcept;
  Storage storage_;
};

enum class FaceReferenceV2CreationStatus {
  Created,
  InvalidReference,
  Missing,
  Ambiguous,
  Unexpected,
};

struct FaceReferenceV2Creation {
  FaceReference reference;
  FaceReferenceV2CreationStatus status{
      FaceReferenceV2CreationStatus::Unexpected};
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == FaceReferenceV2CreationStatus::Created &&
           reference.isV2();
  }
};

inline FaceReference::FaceReference() noexcept {
  std::get<TopologyReference>(storage_).kind = TopologyKind::Face;
}

inline FaceReference::FaceReference(
    BodyId bodyId, FeatureId featureId, std::size_t faceIndex,
    std::string persistentTag, std::optional<FaceSignature> signature) {
  TopologyReference reference;
  reference.bodyId = bodyId;
  reference.featureId = featureId;
  reference.kind = TopologyKind::Face;
  reference.legacyIndex = faceIndex;
  reference.persistentTag = std::move(persistentTag);
  reference.faceSignature = std::move(signature);
  storage_ = std::move(reference);
}

inline FaceReference::FaceReference(Storage storage) noexcept
    : storage_(std::move(storage)) {}

inline FaceReference FaceReference::fromV1(TopologyReference reference) {
  reference.kind = TopologyKind::Face;
  reference.edgeSignature.reset();
  return FaceReference(Storage{std::move(reference)});
}

inline FaceReference FaceReference::fromV2(TopologyReferenceV2 reference) {
  return FaceReference(Storage{std::move(reference)});
}

inline FaceReferenceVersion FaceReference::version() const noexcept {
  return isV1() ? FaceReferenceVersion::V1 : FaceReferenceVersion::V2;
}

inline bool FaceReference::isV1() const noexcept {
  return std::holds_alternative<TopologyReference>(storage_);
}

inline bool FaceReference::isV2() const noexcept {
  return std::holds_alternative<TopologyReferenceV2>(storage_);
}

inline BodyId FaceReference::bodyId() const noexcept {
  return std::visit([](const auto& reference) { return reference.bodyId; },
                    storage_);
}

inline FeatureId FaceReference::featureId() const noexcept {
  return std::visit(
      [](const auto& reference) -> FeatureId {
        using Reference = std::decay_t<decltype(reference)>;
        if constexpr (std::is_same_v<Reference, TopologyReference>)
          return reference.featureId;
        else
          return reference.provenance.featureId;
      },
      storage_);
}

inline const TopologyReference* FaceReference::v1() const noexcept {
  return std::get_if<TopologyReference>(&storage_);
}

inline const TopologyReferenceV2* FaceReference::v2() const noexcept {
  return std::get_if<TopologyReferenceV2>(&storage_);
}

inline const std::size_t& FaceReference::faceIndex() const {
  return std::get<TopologyReference>(storage_).legacyIndex;
}

inline std::size_t& FaceReference::faceIndex() {
  return std::get<TopologyReference>(storage_).legacyIndex;
}

inline const std::string& FaceReference::persistentTag() const {
  return std::get<TopologyReference>(storage_).persistentTag;
}

inline std::string& FaceReference::persistentTag() {
  return std::get<TopologyReference>(storage_).persistentTag;
}

inline const std::optional<FaceSignature>& FaceReference::signature() const {
  return std::get<TopologyReference>(storage_).faceSignature;
}

inline std::optional<FaceSignature>& FaceReference::signature() {
  return std::get<TopologyReference>(storage_).faceSignature;
}

inline const FaceReference::Storage& FaceReference::storage() const noexcept {
  return storage_;
}

}  // namespace solidar
