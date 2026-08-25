#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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

  ShapeFeature& addFeature(std::unique_ptr<ShapeFeature> feature);
  [[nodiscard]] const std::vector<std::unique_ptr<ShapeFeature>>& features()
      const noexcept;
  [[nodiscard]] ShapeFeature* activeFeature() noexcept;
  [[nodiscard]] const ShapeFeature* activeFeature() const noexcept;
  [[nodiscard]] ShapeFeature::ShapePtr resultShape() const noexcept;
  [[nodiscard]] bool rebuild(const RebuildContext& context);
  void markDirtyFrom(std::size_t index) noexcept;

 private:
  static BodyId nextId() noexcept;

  BodyId id_{kInvalidBodyId};
  std::string name_;
  std::vector<std::unique_ptr<ShapeFeature>> features_;
};

}  // namespace solidar
