#include <QApplication>
#include <QImage>

#include <cstdlib>
#include <iostream>

#include "ui/Viewport.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {
bool resembles(const QColor& pixel, const QColor& expected) {
  constexpr int tolerance = 24;
  return std::abs(pixel.red() - expected.red()) <= tolerance &&
         std::abs(pixel.green() - expected.green()) <= tolerance &&
         std::abs(pixel.blue() - expected.blue()) <= tolerance;
}

bool containsManipulatorInk(const QImage& image) {
  const QColor blue{"#0874f9"};
  const QColor orange{"#ff8a00"};
  int bluePixels = 0;
  int orangePixels = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      bluePixels += resembles(pixel, blue);
      orangePixels += resembles(pixel, orange);
    }
  }
  return bluePixels >= 20 && orangePixels >= 10;
}

QImage render(solidar::Viewport& viewport) {
  QImage image(viewport.size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  viewport.render(&image);
  return image;
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  // No solid, Body or preview is installed. The active tool presentation is
  // nevertheless a first-class viewport overlay.
  solidar::Viewport viewport;
  viewport.resize(640, 480);
  viewport.setAngularToolManipulator(
      {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, 35.0, 225.0});
  CHECK(containsManipulatorInk(render(viewport)));

  // The world-space ring remains visible when its arbitrary axis is viewed
  // from materially different camera orientations.
  viewport.viewTop();
  CHECK(containsManipulatorInk(render(viewport)));
  viewport.viewIsometric();
  CHECK(containsManipulatorInk(render(viewport)));
  return EXIT_SUCCESS;
}
