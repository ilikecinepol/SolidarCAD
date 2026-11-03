#pragma once

#include <QWidget>
#include <QString>

#include <cstddef>
#include <memory>

#include "sketch/Sketch.h"

class QPaintEvent;
class TopoDS_Shape;

namespace solidar {

class Document;

class DrawingSheetView final : public QWidget {
  Q_OBJECT

 public:
  explicit DrawingSheetView(QWidget* parent = nullptr);
  void setRectangle(double widthMm, double heightMm);
  void setDocument(const Document& document);
  void setRetainedSource(std::shared_ptr<TopoDS_Shape> shape,
                         std::size_t bodyCount, std::size_t solidCount,
                         QString title, bool landscape, double scale);
  void clearSource() noexcept;
  [[nodiscard]] const TopoDS_Shape* sourceShape() const noexcept;
  [[nodiscard]] std::size_t sourceBodyCount() const noexcept;
  [[nodiscard]] std::size_t sourceSolidCount() const noexcept;
  [[nodiscard]] const QString& sourceTitle() const noexcept { return title_; }

 signals:
  void backRequested();

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  sketch::Sketch sketch_;
  std::shared_ptr<TopoDS_Shape> sourceShape_;
  std::size_t sourceBodyCount_{0};
  std::size_t sourceSolidCount_{0};
  QString title_;
  bool landscape_{};
  double scale_{1.0};
};

}  // namespace solidar
