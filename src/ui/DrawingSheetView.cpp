#include "ui/DrawingSheetView.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include "drawing/DrawingSource.h"
#include "drawing/EskdRenderer.h"
#include "ui/ThemeManager.h"

namespace solidar {

DrawingSheetView::DrawingSheetView(QWidget* parent) : QWidget(parent) {
  setMinimumSize(480, 320);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 12, 16, 12);
  auto* header = new QHBoxLayout;
  auto* back = new QPushButton(QString::fromUtf8("Назад к 3D-модели"), this);
  back->setObjectName(QStringLiteral("drawingBackToModelButton"));
  header->addWidget(back);
  header->addStretch(1);
  layout->addLayout(header);
  layout->addStretch(1);
  connect(back, &QPushButton::clicked, this,
          &DrawingSheetView::backRequested);
}

void DrawingSheetView::setRectangle(double widthMm, double heightMm) {
  sketch_.setRectangle(widthMm, heightMm);
  update();
}

void DrawingSheetView::setDocument(const Document& document) {
  auto source = drawing::collectDrawingSource(document);
  setRetainedSource(std::move(source.shape), source.bodyCount,
                    source.solidCount, {}, false, 1.0);
}

void DrawingSheetView::setRetainedSource(std::shared_ptr<TopoDS_Shape> shape,
                                         std::size_t bodyCount,
                                         std::size_t solidCount,
                                         QString title, bool landscape,
                                         double scale) {
  // Component drawings are model-shape projections. If their valid retained
  // source is empty, never fall back to an unrelated Part sketch that this
  // reusable view may have displayed earlier.
  if (!title.isEmpty()) sketch_.clear();
  sourceShape_ = std::move(shape);
  sourceBodyCount_ = bodyCount;
  sourceSolidCount_ = solidCount;
  title_ = std::move(title);
  landscape_ = landscape;
  scale_ = scale;
  update();
}

void DrawingSheetView::clearSource() noexcept {
  sourceShape_.reset();
  sourceBodyCount_ = 0;
  sourceSolidCount_ = 0;
  title_.clear();
  landscape_ = false;
  scale_ = 1.0;
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
  const ThemeColors& theme = ThemeManager::instance().colors();
  painter.fillRect(rect(), theme.surfaceAlt);

  const double aspect = landscape_ ? 297.0 / 210.0 : 210.0 / 297.0;
  const double availableWidth = width() - 40.0;
  const double availableHeight = height() - 88.0;
  double pageWidth = availableWidth;
  double pageHeight = pageWidth / aspect;
  if (pageHeight > availableHeight) {
    pageHeight = availableHeight;
    pageWidth = pageHeight * aspect;
  }
  const QRectF page((width() - pageWidth) * 0.5,
                    64.0 + (availableHeight - pageHeight) * 0.5, pageWidth,
                    pageHeight);
  if (!title_.isEmpty()) {
    painter.setPen(theme.textPrimary);
    const QString orientation = landscape_ ? QString::fromUtf8("альбомная")
                                            : QString::fromUtf8("книжная");
    painter.drawText(QRectF(180.0, 14.0, width() - 200.0, 34.0),
                     Qt::AlignRight | Qt::AlignVCenter,
                     title_ + QString::fromUtf8(" · A4 · ") + orientation +
                         QString::fromUtf8(" · масштаб %1").arg(scale_));
  }
  painter.setPen(Qt::NoPen);
  QColor pageShadow = theme.shadow;
  pageShadow.setAlpha(90);
  painter.setBrush(pageShadow);
  painter.drawRect(page.translated(4.0, 5.0));
  drawing::EskdRenderer::renderA4(painter, page, sketch_, {},
                                  sourceShape_.get());
}

}  // namespace solidar
