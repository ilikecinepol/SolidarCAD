#pragma once

namespace solidar {

// User-facing application theme preference. Stored values must never carry
// localised UI text; they are persisted as stable tokens ("system", "light",
// "dark") so the settings namespace survives later RU/EN localization.
enum class AppTheme {
  System,
  Light,
  Dark,
};

}  // namespace solidar
