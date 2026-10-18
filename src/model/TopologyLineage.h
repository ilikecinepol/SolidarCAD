#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/TopologyReference.h"

class TopoDS_Shape;

namespace solidar {

// Typed, part-local identity evidence for persistent topology v2.  These
// values are deliberately independent from OCCT traversal order and geometric
// signatures.  Persistence is outside the PT2-carrier milestone.
enum class TopologyProvenanceOperation : std::uint8_t {
  Seed,
  Generated,
  Modified,
  Retained,
  Merge,
};

enum class TopologySemanticRole : std::uint8_t {
  SketchBoundary,
  Cap,
  Side,
  Original,
  Reflected,
  PatternInstance,
  BooleanResult,
  Custom,
};

// Authored occurrence identity. A pattern count edit may append or remove
// slots, but must never renumber surviving slots.
struct TopologyOccurrenceKey {
  FeatureId featureId{kInvalidFeatureId};
  std::uint64_t slot{};

  friend bool operator==(const TopologyOccurrenceKey&,
                         const TopologyOccurrenceKey&) = default;
};

struct TopologyProvenanceOrigin {
  TopologyProvenanceOrigin() = default;
  TopologyProvenanceOrigin(
      FeatureId sourceFeatureId, TopologyKind topologyKind,
      TopologyProvenanceOperation provenanceOperation,
      TopologySemanticRole semanticRole, std::string authoredLocalName,
      std::optional<TopologyOccurrenceKey> authoredOccurrence = {});

  FeatureId featureId{kInvalidFeatureId};
  TopologyKind kind{TopologyKind::Face};
  TopologyProvenanceOperation operation{TopologyProvenanceOperation::Seed};
  TopologySemanticRole role{TopologySemanticRole::Custom};
  std::string localName;
  std::optional<TopologyOccurrenceKey> occurrence;

  [[nodiscard]] bool isValid(std::string* error = nullptr) const;

  friend bool operator==(const TopologyProvenanceOrigin&,
                         const TopologyProvenanceOrigin&) = default;
};

struct TopologyProvenanceKey {
  TopologyProvenanceKey() = default;
  TopologyProvenanceKey(
      FeatureId producerFeatureId, TopologyKind topologyKind,
      TopologyProvenanceOperation provenanceOperation,
      TopologySemanticRole semanticRole, std::string authoredLocalName,
      std::vector<TopologyProvenanceOrigin> parentOrigins = {},
      std::optional<TopologyOccurrenceKey> authoredOccurrence = {});

  FeatureId featureId{kInvalidFeatureId};
  TopologyKind kind{TopologyKind::Face};
  TopologyProvenanceOperation operation{TopologyProvenanceOperation::Seed};
  TopologySemanticRole role{TopologySemanticRole::Custom};
  std::string localName;
  // Producers provide origins in canonical sorted order.  Keeping that rule
  // at the carrier boundary prevents equivalent N-to-1 merges from acquiring
  // order-dependent identities.
  std::vector<TopologyProvenanceOrigin> origins;
  std::optional<TopologyOccurrenceKey> occurrence;

  [[nodiscard]] bool isCanonical(std::string* error = nullptr) const;
  // Immediate origins are intentionally non-recursive. Identity remains
  // lossless because the complete immediate key projection (including
  // operation, role and authored occurrence) is retained verbatim. Producer
  // validation only rejects two unequal full keys with that same projection.
  [[nodiscard]] TopologyProvenanceOrigin immediateOrigin() const;
  void canonicalizeOrigins();

  friend bool operator==(const TopologyProvenanceKey&,
                         const TopologyProvenanceKey&) = default;
};

struct TopologyReferenceV2 {
  BodyId bodyId{kInvalidBodyId};
  TopologyProvenanceKey provenance;

  [[nodiscard]] bool hasValidOwner() const noexcept {
    return bodyId != kInvalidBodyId &&
           provenance.featureId != kInvalidFeatureId;
  }

  friend bool operator==(const TopologyReferenceV2&,
                         const TopologyReferenceV2&) = default;
};

// A binding is transient evidence captured from an operation builder while
// its OCCT history is still alive.  TopologyIndex reconciles every binding to
// a canonical IsSame subshape of the committed output.
struct TopologyLineageBinding {
  TopologyProvenanceKey provenance;
  std::shared_ptr<const TopoDS_Shape> subshape;
};

enum class TopologyLineageCoverage : std::uint8_t {
  None,
  PartialFaces,
  CompleteFaces,
};

[[nodiscard]] bool isValidTopologyLineageCoverage(
    TopologyLineageCoverage coverage) noexcept;

class TopologyLineage final {
 public:
  [[nodiscard]] static std::shared_ptr<const TopologyLineage> create(
      std::vector<TopologyLineageBinding> bindings,
      TopologyLineageCoverage coverage,
      std::string* error = nullptr);

  [[nodiscard]] const std::vector<TopologyLineageBinding>& bindings()
      const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] TopologyLineageCoverage coverage() const noexcept;
  [[nodiscard]] bool validateForProducer(
      FeatureId producerFeatureId, std::string* error = nullptr) const;

 private:
  TopologyLineage(std::vector<TopologyLineageBinding> bindings,
                  TopologyLineageCoverage coverage);

  std::vector<TopologyLineageBinding> bindings_;
  TopologyLineageCoverage coverage_{TopologyLineageCoverage::None};
};

}  // namespace solidar
