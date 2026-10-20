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

enum class EdgeReferenceVersion : std::uint8_t { V1, V2 };

// Closed carrier for persistent edge identities. The active alternative is
// authoritative: a reference can never contain a mixture of v1 geometric
// fallback data and v2 operation provenance.
class EdgeReference final {
 public:
  using Storage = std::variant<TopologyReference, TopologyReferenceV2>;

  // Compatibility default: an invalid v1 edge reference.
  EdgeReference() noexcept;
  EdgeReference(BodyId bodyId, FeatureId featureId, std::size_t edgeIndex,
                std::string persistentTag = {},
                std::optional<EdgeSignature> signature = {});

  [[nodiscard]] static EdgeReference fromV1(TopologyReference reference);
  [[nodiscard]] static EdgeReference fromV2(TopologyReferenceV2 reference);

  [[nodiscard]] EdgeReferenceVersion version() const noexcept;
  [[nodiscard]] bool isV1() const noexcept;
  [[nodiscard]] bool isV2() const noexcept;
  [[nodiscard]] BodyId bodyId() const noexcept;
  [[nodiscard]] FeatureId featureId() const noexcept;
  [[nodiscard]] const TopologyReference* v1() const noexcept;
  [[nodiscard]] const TopologyReferenceV2* v2() const noexcept;
  // Explicit v1-only compatibility accessors. They throw
  // std::bad_variant_access for v2 instead of silently projecting v2 into a
  // weaker v1 identity.
  [[nodiscard]] const std::size_t& edgeIndex() const;
  [[nodiscard]] std::size_t& edgeIndex();
  [[nodiscard]] const std::string& persistentTag() const;
  [[nodiscard]] std::string& persistentTag();
  [[nodiscard]] const std::optional<EdgeSignature>& signature() const;
  [[nodiscard]] std::optional<EdgeSignature>& signature();
  [[nodiscard]] const Storage& storage() const noexcept;

  friend bool operator==(const EdgeReference&, const EdgeReference&) = default;

 private:
  explicit EdgeReference(Storage storage) noexcept;
  Storage storage_;
};

enum class EdgeReferenceV2CreationStatus {
  Created,
  InvalidReference,
  Missing,
  Ambiguous,
  Unexpected,
};

struct EdgeReferenceV2Creation {
  EdgeReference reference;
  EdgeReferenceV2CreationStatus status{
      EdgeReferenceV2CreationStatus::Unexpected};
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == EdgeReferenceV2CreationStatus::Created &&
           reference.isV2();
  }
};

inline EdgeReference::EdgeReference() noexcept {
  std::get<TopologyReference>(storage_).kind = TopologyKind::Edge;
}

inline EdgeReference::EdgeReference(
    BodyId bodyId, FeatureId featureId, std::size_t edgeIndex,
    std::string persistentTag, std::optional<EdgeSignature> signature) {
  TopologyReference reference;
  reference.bodyId = bodyId;
  reference.featureId = featureId;
  reference.kind = TopologyKind::Edge;
  reference.legacyIndex = edgeIndex;
  reference.persistentTag = std::move(persistentTag);
  reference.edgeSignature = std::move(signature);
  storage_ = std::move(reference);
}

inline EdgeReference::EdgeReference(Storage storage) noexcept
    : storage_(std::move(storage)) {}

inline EdgeReference EdgeReference::fromV1(TopologyReference reference) {
  reference.kind = TopologyKind::Edge;
  reference.faceSignature.reset();
  return EdgeReference(Storage{std::move(reference)});
}

inline EdgeReference EdgeReference::fromV2(TopologyReferenceV2 reference) {
  return EdgeReference(Storage{std::move(reference)});
}

inline EdgeReferenceVersion EdgeReference::version() const noexcept {
  return isV1() ? EdgeReferenceVersion::V1 : EdgeReferenceVersion::V2;
}

inline bool EdgeReference::isV1() const noexcept {
  return std::holds_alternative<TopologyReference>(storage_);
}

inline bool EdgeReference::isV2() const noexcept {
  return std::holds_alternative<TopologyReferenceV2>(storage_);
}

inline BodyId EdgeReference::bodyId() const noexcept {
  return std::visit([](const auto& reference) { return reference.bodyId; },
                    storage_);
}

inline FeatureId EdgeReference::featureId() const noexcept {
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

inline const TopologyReference* EdgeReference::v1() const noexcept {
  return std::get_if<TopologyReference>(&storage_);
}

inline const TopologyReferenceV2* EdgeReference::v2() const noexcept {
  return std::get_if<TopologyReferenceV2>(&storage_);
}

inline const std::size_t& EdgeReference::edgeIndex() const {
  return std::get<TopologyReference>(storage_).legacyIndex;
}

inline std::size_t& EdgeReference::edgeIndex() {
  return std::get<TopologyReference>(storage_).legacyIndex;
}

inline const std::string& EdgeReference::persistentTag() const {
  return std::get<TopologyReference>(storage_).persistentTag;
}

inline std::string& EdgeReference::persistentTag() {
  return std::get<TopologyReference>(storage_).persistentTag;
}

inline const std::optional<EdgeSignature>& EdgeReference::signature() const {
  return std::get<TopologyReference>(storage_).edgeSignature;
}

inline std::optional<EdgeSignature>& EdgeReference::signature() {
  return std::get<TopologyReference>(storage_).edgeSignature;
}

inline const EdgeReference::Storage& EdgeReference::storage() const noexcept {
  return storage_;
}

}  // namespace solidar
