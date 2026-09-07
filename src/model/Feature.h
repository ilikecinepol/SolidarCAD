#pragma once

#include <cstdint>
#include <memory>
#include <string>

class TopoDS_Shape;

namespace solidar {

class Document;
class Body;

struct RebuildContext {
  Document& document;
  const Body* body{};
  const TopoDS_Shape* previousShape{};
};

using FeatureId = std::uint64_t;
inline constexpr FeatureId kInvalidFeatureId = 0;
using SketchId = std::uint64_t;
inline constexpr SketchId kInvalidSketchId = 0;

enum class FeatureState { Dirty, Valid, Error };

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

  [[nodiscard]] virtual std::string typeName() const = 0;
  [[nodiscard]] virtual bool dependsOnSketch(SketchId sketchId) const noexcept;
  virtual bool rebuild(const RebuildContext& context) = 0;
  [[nodiscard]] virtual std::unique_ptr<Feature> clone() const = 0;

 protected:
  void markValid() noexcept;
  void markError(std::string message);

 private:
  static FeatureId nextId() noexcept;

  FeatureId id_{kInvalidFeatureId};
  std::string name_;
  FeatureState state_{FeatureState::Dirty};
  std::string error_;
};

}  // namespace solidar
