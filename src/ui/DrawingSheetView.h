#pragma once

#include <QWidget>

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
  [[nodiscard]] const TopoDS_Shape* sourceShape() const noexcept;
  [[nodiscard]] std::size_t sourceBodyCount() const noexcept;
  [[nodiscard]] std::size_t sourceSolidCount() const noexcept;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  sketch::Sketch sketch_;
  std::shared_ptr<TopoDS_Shape> sourceShape_;
  std::size_t sourceBodyCount_{0};
  std::size_t sourceSolidCount_{0};
};

}  // namespace solidar
