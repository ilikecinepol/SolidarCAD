#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TopoDS_Shape;

namespace solidar {

class Document;
class Body;
class TopologyIndex;
class ShapeFeature;

using FeatureId = std::uint64_t;
inline constexpr FeatureId kInvalidFeatureId = 0;
using SketchId = std::uint64_t;
inline constexpr SketchId kInvalidSketchId = 0;
using ShapeRevision = std::uint64_t;
inline constexpr ShapeRevision kInvalidShapeRevision = 0;

struct FeatureDependencies {
  std::vector<FeatureId> featureIds;
  std::vector<SketchId> sketchIds;
};

struct RebuildContext {
  Document& document;
  const Body* body{};
  const TopoDS_Shape* previousShape{};
  const ShapeFeature* previousFeature{};
};

enum class FeatureState { Dirty, Valid, Error };

// Stable model identity for persisted feature implementations.  Numeric
// values are part of the model/project boundary; do not reorder or reuse them.
enum class FeatureKind : std::uint8_t {
  Unknown = 0,
  ImportedShape = 1,
  Extrude = 2,
  Revolve = 3,
  Pocket = 4,
  Fillet = 5,
  Chamfer = 6,
  Mirror = 7,
  Move = 8,
  LinearPattern = 9,
  CircularPattern = 10,
  JoinBodies = 11,
  Shell = 12,
  Draft = 13,
};

inline constexpr std::size_t kPersistedFeatureKindCount = 13;

class Feature {
 public:
  explicit Feature(std::string name = {});
  Feature(FeatureId id, std::string name);
  virtual ~Feature() = default;

  Feature(const Feature&) = default;
  Feature& operator=(const Feature&) = default;

  [[nodiscard]] FeatureId id() const noexcept;
  [[nodiscard]] const std::string& name() const noexcept;
  void setName(std::string name);

  [[nodiscard]] FeatureState state() const noexcept;
  [[nodiscard]] bool isDirty() const noexcept;
  [[nodiscard]] bool isValid() const noexcept;
  [[nodiscard]] bool isFailed() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;
  void setDirty(bool dirty = true) noexcept;
  void markBlocked(std::string message);

  [[nodiscard]] virtual FeatureKind kind() const noexcept {
    return FeatureKind::Unknown;
  }
  // Explicit dependency enumeration lets Document build its operation-local
  // graph in O(nodes + edges), without probing every possible ID pair.
  [[nodiscard]] virtual FeatureDependencies dependencies() const;
  // Compatibility projections over the canonical declaration. They are
  // deliberately non-virtual so dependency truth cannot diverge.
  [[nodiscard]] bool dependsOnSketch(SketchId sketchId) const;
  [[nodiscard]] bool dependsOnFeature(FeatureId featureId) const;
  // The only public rebuild entry. It is deliberately non-virtual so callers
  // cannot bypass the model exception boundary implemented by Feature and
  // ShapeFeature.
  [[nodiscard]] bool rebuild(const RebuildContext& context) noexcept;
  [[nodiscard]] virtual std::unique_ptr<Feature> clone() const = 0;

 protected:
  virtual bool rebuildAtBoundary(const RebuildContext& context) = 0;
  void markValid() noexcept;
  void markError(std::string message);

 private:
  friend class Document;
  static FeatureId nextId() noexcept;
  static void reserveId(FeatureId id) noexcept;

  FeatureId id_{kInvalidFeatureId};
  std::string name_;
  FeatureState state_{FeatureState::Dirty};
  std::string error_;
};

}  // namespace solidar
