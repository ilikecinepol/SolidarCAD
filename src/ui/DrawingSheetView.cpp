#include "ui/DrawingSheetView.h"

#include <QPainter>

#include "drawing/DrawingSource.h"
#include "drawing/EskdRenderer.h"

namespace solidar {

DrawingSheetView::DrawingSheetView(QWidget* parent) : QWidget(parent) {
  setMinimumSize(480, 320);
}

void DrawingSheetView::setRectangle(double widthMm, double heightMm) {
  sketch_.setRectangle(widthMm, heightMm);
  update();
}

void DrawingSheetView::setDocument(const Document& document) {
  auto source = drawing::collectDrawingSource(document);
  sourceShape_ = std::move(source.shape);
  sourceBodyCount_ = source.bodyCount;
  sourceSolidCount_ = source.solidCount;
  update();
}

const TopoDS_Shape* DrawingSheetView::sourceShape() const noexcept {
  return sourceShape_.get();
}

std::size_t DrawingSheetView::sourceBodyCount() const noexcept {
  return sourceBodyCount_;
}

std::size_t DrawingSheetView::sourceSolidCount() const noexcept {
  return sourceSolidCount_;
}

void DrawingSheetView::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), QColor(76, 82, 91));

  constexpr double aspect = 210.0 / 297.0;
  const double availableWidth = width() - 40.0;
  const double availableHeight = height() - 40.0;
  double pageWidth = availableWidth;
  double pageHeight = pageWidth / aspect;
  if (pageHeight > availableHeight) {
    pageHeight = availableHeight;
    pageWidth = pageHeight * aspect;
  }
  const QRectF page((width() - pageWidth) * 0.5,
                    (height() - pageHeight) * 0.5, pageWidth, pageHeight);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(40, 40, 40, 90));
  painter.drawRect(page.translated(4.0, 5.0));
  drawing::EskdRenderer::renderA4(painter, page, sketch_, {},
                                  sourceShape_.get());
}

}  // namespace solidar
