#pragma once

#include <QWidget>

class QHBoxLayout;

namespace solidar {

struct HistoryTimelineGeometry {
  int stepCount{};
  int leftPadding{12};
  int iconSize{28};
  int gap{8};
  [[nodiscard]] int pitch() const noexcept { return iconSize + gap; }
  [[nodiscard]] int center(int index) const noexcept {
    return leftPadding + iconSize / 2 + index * pitch();
  }
  [[nodiscard]] int trackStart() const noexcept {
    return stepCount ? center(0) : leftPadding;
  }
  [[nodiscard]] int trackEnd() const noexcept {
    return stepCount ? center(stepCount - 1) : leftPadding;
  }
  [[nodiscard]] int contentWidth() const noexcept {
    return stepCount ? trackEnd() + iconSize / 2 + leftPadding
                     : 2 * leftPadding;
  }
  [[nodiscard]] int nearestIndex(int x) const noexcept;
};

class HistoryTimelineWidget final : public QWidget {
  Q_OBJECT
 public:
  explicit HistoryTimelineWidget(QWidget* parent = nullptr);
  [[nodiscard]] QHBoxLayout* stepLayout() const noexcept;
  void setStepCount(int count);
  void setPosition(int position);
  [[nodiscard]] int position() const noexcept;

 signals:
  void positionChanged(int position);

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;

 private:
  void selectAt(int x);
  QHBoxLayout* layout_{};
  HistoryTimelineGeometry geometry_{};
  int position_{};
  bool dragging_{false};
};

}  // namespace solidar
