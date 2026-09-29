#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QPushButton>

#include <iostream>

#include "sketch/SketchRibbon.h"
#include "ui/SketchCanvas.h"
#include "ui/ThemeManager.h"
#include "ui/ToolIcon.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << "CHECK failed at line " << __LINE__ << ": "           \
                << #condition << '\n';                                     \
      return 1;                                                            \
    }                                                                      \
  } while (false)

namespace {

bool hasVisiblePixel(const QIcon& icon) {
  const QImage image = icon.pixmap(48, 48).toImage();
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      if (image.pixelColor(x, y).alpha() != 0) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);

  const QIcon liveThemeIcon =
      solidar::toolIcon(solidar::ToolIconKind::Extrude);
  solidar::ThemeManager::instance().apply(solidar::ResolvedTheme::Light);
  const QImage lightIcon = liveThemeIcon.pixmap(48, 48).toImage();
  solidar::ThemeManager::instance().apply(solidar::ResolvedTheme::Dark);
  const QImage darkIcon = liveThemeIcon.pixmap(48, 48).toImage();
  CHECK(lightIcon != darkIcon);

  constexpr int first = static_cast<int>(solidar::ToolIconKind::CreateSketch);
  constexpr int last =
      static_cast<int>(solidar::ToolIconKind::RectangleFromCenter);
  for (const auto theme : {solidar::ResolvedTheme::Light,
                           solidar::ResolvedTheme::Dark}) {
    solidar::ThemeManager::instance().apply(theme);
    for (int value = first; value <= last; ++value) {
      const QIcon icon =
          solidar::toolIcon(static_cast<solidar::ToolIconKind>(value));
      CHECK(!icon.isNull());
      CHECK(hasVisiblePixel(icon));
    }
  }

  solidar::SketchCanvas canvas;
  solidar::SketchRibbon ribbon(&canvas);
  const auto buttons = ribbon.findChildren<QPushButton*>(
      QStringLiteral("toolButton"));
  CHECK(buttons.size() == 15);
  for (const auto* button : buttons) {
    CHECK(!button->icon().isNull());
    CHECK(hasVisiblePixel(button->icon()));
    CHECK(!button->toolTip().isEmpty());
    CHECK(!button->accessibleName().isEmpty());
  }
  return 0;
}
