#include "io/DocumentExportShapes.h"

#include <BRepCheck_Analyzer.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <utility>

#include "model/Body.h"
#include "model/Document.h"
#include "model/GeometryOperation.h"

namespace solidar::io::detail {
namespace {

void setError(QString* error, const QString& message) {
  if (error) *error = message;
}

QString bodyDescription(const Body& body, std::size_t index) {
  const std::string& name = body.name();
  const QString decodedName =
      QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
  if (!decodedName.isEmpty())
    return QString::fromUtf8("тело «%1»").arg(decodedName);
  return QString::fromUtf8("тело №%1").arg(index + 1);
}

QString failureDiagnostic(const GeometryFailure& failure) {
  QString message;
  switch (failure.kind) {
    case GeometryFailureKind::OcctException:
      message =
          QString::fromUtf8("Ошибка OCCT при проверке геометрии документа.");
      break;
    case GeometryFailureKind::StandardException:
      message = QString::fromUtf8(
          "Стандартное исключение при проверке геометрии документа.");
      break;
    case GeometryFailureKind::UnknownException:
      message = QString::fromUtf8(
          "Неизвестная ошибка при проверке геометрии документа.");
      break;
    default:
      message = QString::fromUtf8("Ошибка при проверке геометрии документа.");
      break;
  }
  if (!failure.detail.empty())
    message += QStringLiteral(": ") +
               QString::fromUtf8(failure.detail.data(),
                                 static_cast<qsizetype>(failure.detail.size()));
  return message;
}

bool validateDocumentExportShapesImpl(
    const Document& document, std::vector<ShapeFeature::ShapePtr>* shapes,
    QString* error) {
  std::vector<ShapeFeature::ShapePtr> staged;
  if (shapes) staged.reserve(document.bodies().size());
  std::size_t exportableCount = 0;
  for (std::size_t index = 0; index < document.bodies().size(); ++index) {
    const Body& body = document.bodies()[index];
    if (body.features().empty()) continue;

    const ShapeFeature* const active = body.activeFeature();
    if (!active || !active->isValid()) {
      setError(error,
               QString::fromUtf8(
                   "Невозможно экспортировать документ: %1 содержит "
                   "ошибочную или заблокированную активную операцию.")
                   .arg(bodyDescription(body, index)));
      return false;
    }

    auto shape = body.resultShape();
    if (!shape || shape->IsNull()) {
      setError(error,
               QString::fromUtf8(
                   "Невозможно экспортировать документ: %1 содержит "
                   "историю, но не имеет актуального корректного результата.")
                   .arg(bodyDescription(body, index)));
      return false;
    }

    BRepCheck_Analyzer analyzer(*shape);
    if (!analyzer.IsValid()) {
      setError(error, QString::fromUtf8(
                          "Невозможно экспортировать документ: %1 содержит "
                          "некорректную B-Rep геометрию.")
                          .arg(bodyDescription(body, index)));
      return false;
    }
    ++exportableCount;
    if (shapes) staged.push_back(std::move(shape));
  }

  if (exportableCount == 0) {
    setError(error, QString::fromUtf8(
                        "Документ не содержит B-Rep геометрии для экспорта."));
    return false;
  }
  if (shapes) *shapes = std::move(staged);
  return true;
}

bool validateDocumentExportShapes(
    const Document& document, std::vector<ShapeFeature::ShapePtr>* shapes,
    QString* error) {
  bool collected = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        collected = validateDocumentExportShapesImpl(document, shapes, error);
      },
      &failure);
  if (completed) return collected;
  setError(error, failureDiagnostic(failure));
  return false;
}

}  // namespace

bool collectDocumentExportShapes(const Document& document,
                                 std::vector<ShapeFeature::ShapePtr>* shapes,
                                 QString* error) {
  if (error) error->clear();
  if (!shapes) {
    setError(error, QString::fromUtf8(
                        "Внутренняя ошибка подготовки геометрии к экспорту."));
    return false;
  }
  return validateDocumentExportShapes(document, shapes, error);
}

}  // namespace solidar::io::detail

namespace solidar::io {

bool hasExportableDocumentShapes(const Document& document, QString* error) {
  if (error) error->clear();
  return detail::validateDocumentExportShapes(document, nullptr, error);
}

}  // namespace solidar::io
