#pragma once

namespace solidar {

enum class InteractionDeviceKind { Mouse, Trackpad, Pen, Touch };
enum class InteractionIntent {
  None, Hover, Select, Orbit, Pan, Zoom, BoxSelect, DirectDrag,
  Confirm, Cancel, ContextAction
};
enum class NavigationPreset { SolidarCADDefault };
enum class PointerPhase { Press, Move, Release };
enum class PointerButton { None, Primary, Middle, Secondary };

struct NormalizedPointerInput {
  InteractionDeviceKind device{InteractionDeviceKind::Mouse};
  PointerPhase phase{PointerPhase::Move};
  PointerButton button{PointerButton::None};
  bool primaryDown{};
  bool middleDown{};
  bool secondaryDown{};
  bool overDirectTarget{};
  bool overSelectable{};
};

struct NormalizedKeyInput {
  enum class Key { Other, Enter, Escape, Menu } key{Key::Other};
};

struct NormalizedWheelInput {
  InteractionDeviceKind device{InteractionDeviceKind::Mouse};
  double delta{};
};

class InteractionInputMapper final {
 public:
  explicit InteractionInputMapper(
      NavigationPreset preset = NavigationPreset::SolidarCADDefault)
      : preset_(preset) {}
  [[nodiscard]] InteractionIntent map(const NormalizedPointerInput& input) const;
  [[nodiscard]] InteractionIntent map(const NormalizedKeyInput& input) const;
  [[nodiscard]] InteractionIntent map(const NormalizedWheelInput& input) const;
  [[nodiscard]] NavigationPreset preset() const noexcept { return preset_; }

 private:
  NavigationPreset preset_;
};

}  // namespace solidar
