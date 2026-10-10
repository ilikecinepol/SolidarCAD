#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "model/ShapeFeature.h"

namespace solidar {

using BodyId = std::uint64_t;
inline constexpr BodyId kInvalidBodyId = 0;

class Body final {
 public:
  explicit Body(std::string name = {});
  Body(BodyId id, std::string name);
  Body(const Body& other);
  Body& operator=(const Body& other);
  Body(Body&&) noexcept = default;
  Body& operator=(Body&&) noexcept = default;

  [[nodiscard]] BodyId id() const noexcept;
  [[nodiscard]] const std::string& name() const noexcept;
  void setName(std::string name);
  [[nodiscard]] bool visible() const noexcept;
  void setVisible(bool visible) noexcept;

  ShapeFeature& addFeature(std::unique_ptr<ShapeFeature> feature);
  [[nodiscard]] const std::vector<std::unique_ptr<ShapeFeature>>& features()
      const noexcept;
  [[nodiscard]] ShapeFeature* activeFeature() noexcept;
  [[nodiscard]] const ShapeFeature* activeFeature() const noexcept;
  // Authoritative current result. Failed/blocked active Features never expose
  // stale geometry through this dependency-facing API.
  [[nodiscard]] ShapeFeature::ShapePtr resultShape() const noexcept;
  // Explicit presentation fallback to the newest validated committed Shape.
  // This must not be used as an input to feature rebuilds or new operations.
  [[nodiscard]] ShapeFeature::ShapePtr lastValidResultShape() const noexcept;
  void markDirtyFrom(std::size_t index) noexcept;
  [[nodiscard]] std::optional<std::size_t> featureIndex(
      FeatureId id) const noexcept;
  void eraseFeaturesFrom(std::size_t index);

 private:
  friend class Document;
  static BodyId nextId() noexcept;
  static void reserveId(BodyId id) noexcept;

  BodyId id_{kInvalidBodyId};
  std::string name_;
  bool visible_{true};
  std::vector<std::unique_ptr<ShapeFeature>> features_;
};

}  // namespace solidar
