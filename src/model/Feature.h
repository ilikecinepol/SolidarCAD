#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace solidar {

class Document;

struct RebuildContext {
  const Document& document;
};

using FeatureId = std::uint64_t;
inline constexpr FeatureId kInvalidFeatureId = 0;

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
  [[nodiscard]] const std::string& error() const noexcept;
  void setDirty(bool dirty = true) noexcept;

  [[nodiscard]] virtual std::string typeName() const = 0;
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
