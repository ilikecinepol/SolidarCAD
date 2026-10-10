#include "io/StepExchange.h"

#include <BRepCheck_Analyzer.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Shape.hxx>

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io/DocumentExportShapes.h"
#include "model/Document.h"
#include "model/GeometryOperation.h"
#include "model/ImportedShapeFeature.h"

namespace solidar::io {
namespace {

std::mutex& stepTranslatorMutex() {
  static std::mutex mutex;
  return mutex;
}

void setError(QString* error, const QString& message) {
  if (error) *error = message;
}

QString failureDiagnostic(const char* context, const GeometryFailure& failure) {
  QString message;
  switch (failure.kind) {
    case GeometryFailureKind::OcctException:
      message = QString::fromUtf8("Ошибка OCCT ") + QString::fromUtf8(context);
      break;
    case GeometryFailureKind::StandardException:
      message = QString::fromUtf8("Стандартное исключение ") +
                QString::fromUtf8(context);
      break;
    case GeometryFailureKind::UnknownException:
      message =
          QString::fromUtf8("Неизвестная ошибка ") + QString::fromUtf8(context);
      break;
    default:
      message = QString::fromUtf8("Ошибка ") + QString::fromUtf8(context);
      break;
  }
  message += QLatin1Char('.');
  if (!failure.detail.empty())
    message += QStringLiteral(": ") +
               QString::fromUtf8(failure.detail.data(),
                                 static_cast<qsizetype>(failure.detail.size()));
  return message;
}

class ScopedStepSchema final {
 public:
  ScopedStepSchema() {
    const char* current = Interface_Static::CVal("write.step.schema");
    if (current) previous_ = current;
    applied_ = Interface_Static::SetCVal("write.step.schema", "AP242DIS");
  }

  ~ScopedStepSchema() {
    if (applied_)
      Interface_Static::SetCVal("write.step.schema", previous_.c_str());
  }

  [[nodiscard]] bool applied() const noexcept { return applied_; }

 private:
  std::string previous_;
  bool applied_{false};
};

std::shared_ptr<const TopoDS_Shape> readStepFileImpl(const QString& path,
                                                     QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(error, QString::fromUtf8("Не удалось открыть STEP-файл: ") +
                        file.errorString());
    return {};
  }
  const qint64 fileSize = file.size();
  if (fileSize < 0) {
    setError(error,
             QString::fromUtf8("Не удалось определить размер STEP-файла."));
    return {};
  }
  if (fileSize > kMaximumStepFileBytes) {
    setError(error, QString::fromUtf8(
                        "STEP-файл превышает допустимый размер (%1 байт).")
                        .arg(kMaximumStepFileBytes));
    return {};
  }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFileDevice::NoError || bytes.size() != fileSize) {
    setError(error, QString::fromUtf8("Не удалось прочитать STEP-файл: ") +
                        file.errorString());
    return {};
  }
  if (bytes.isEmpty()) {
    setError(error, QString::fromUtf8("STEP-файл пуст."));
    return {};
  }

  std::lock_guard lock(stepTranslatorMutex());
  std::istringstream stream(
      std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
      std::ios::in | std::ios::binary);
  STEPControl_Reader reader;
  const QByteArray logicalName = QFileInfo(path).fileName().toUtf8();
  if (reader.ReadStream(logicalName.constData(), stream) != IFSelect_RetDone) {
    setError(error, QString::fromUtf8("OCCT не смог прочитать STEP-модель."));
    return {};
  }
  const int rootCount = reader.NbRootsForTransfer();
  if (rootCount <= 0) {
    setError(error, QString::fromUtf8(
                        "STEP-файл не содержит переносимой B-Rep геометрии."));
    return {};
  }
  const int transferred = reader.TransferRoots();
  if (transferred != rootCount) {
    setError(error,
             QString::fromUtf8(
                 "Не удалось полностью перенести корни STEP-модели (%1 из %2).")
                 .arg(transferred)
                 .arg(rootCount));
    return {};
  }
  TopoDS_Shape shape = reader.OneShape();
  if (shape.IsNull()) {
    setError(error,
             QString::fromUtf8("После импорта получена пустая геометрия."));
    return {};
  }
  BRepCheck_Analyzer analyzer(shape);
  if (!analyzer.IsValid()) {
    setError(error, QString::fromUtf8(
                        "STEP-файл содержит некорректную B-Rep геометрию."));
    return {};
  }
  return std::make_shared<const TopoDS_Shape>(std::move(shape));
}

bool importDocumentStepImpl(const QString& path, Document* document,
                            const QString& featureName, QString* error) {
  if (!document) {
    setError(error, QString::fromUtf8("Не задан документ для импорта STEP."));
    return false;
  }
  const auto shape = readStepFileImpl(path, error);
  if (!shape) return false;

  Document staged = *document;
  const QString naturalName =
      featureName.isEmpty() ? QFileInfo(path).completeBaseName() : featureName;
  const std::string name = naturalName.isEmpty()
                               ? std::string("Imported STEP")
                               : naturalName.toUtf8().toStdString();
  auto& body = staged.addBody(name);
  body.addFeature(std::make_unique<ImportedShapeFeature>(shape, name));
  if (!staged.recompute()) {
    setError(error, QString::fromUtf8("Не удалось добавить STEP в документ: ") +
                        QString::fromStdString(staged.rebuildError()));
    return false;
  }
  *document = std::move(staged);
  return true;
}

bool exportDocumentStepImpl(const QString& path, const Document& document,
                            QString* error) {
  std::vector<ShapeFeature::ShapePtr> shapes;
  if (!detail::collectDocumentExportShapes(document, &shapes, error))
    return false;

  std::lock_guard lock(stepTranslatorMutex());
  // Constructing the writer initializes the STEP controller and registers
  // its Interface_Static parameters before the schema is changed.
  STEPControl_Writer writer;
  ScopedStepSchema schema;
  if (!schema.applied()) {
    setError(error,
             QString::fromUtf8("Не удалось включить схему STEP AP242DIS."));
    return false;
  }
  for (const auto& shape : shapes) {
    if (writer.Transfer(*shape, STEPControl_AsIs) != IFSelect_RetDone) {
      setError(error, QString::fromUtf8(
                          "OCCT не смог подготовить всю геометрию для STEP."));
      return false;
    }
  }
  std::ostringstream stream(std::ios::out | std::ios::binary);
  if (writer.WriteStream(stream) != IFSelect_RetDone || !stream.good()) {
    setError(error, QString::fromUtf8("OCCT не смог сформировать STEP-файл."));
    return false;
  }
  const std::string bytes = stream.str();
  if (bytes.empty()) {
    setError(error, QString::fromUtf8("OCCT сформировал пустой STEP-файл."));
    return false;
  }

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    setError(error, QString::fromUtf8("Не удалось открыть файл для записи: ") +
                        file.errorString());
    return false;
  }
  if (file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
      static_cast<qint64>(bytes.size())) {
    setError(error,
             QString::fromUtf8("Не удалось полностью записать STEP-файл: ") +
                 file.errorString());
    file.cancelWriting();
    return false;
  }
  if (!file.flush()) {
    setError(error,
             QString::fromUtf8("Не удалось полностью записать STEP-файл: ") +
                 file.errorString());
    file.cancelWriting();
    return false;
  }
  if (!file.commit()) {
    setError(error,
             QString::fromUtf8("Не удалось завершить запись STEP-файла: ") +
                 file.errorString());
    return false;
  }
  return true;
}

}  // namespace

std::shared_ptr<const TopoDS_Shape> readStepFile(const QString& path,
                                                 QString* error) {
  if (error) error->clear();
  std::shared_ptr<const TopoDS_Shape> shape;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] { shape = readStepFileImpl(path, error); }, &failure);
  if (completed) return shape;
  setError(error, failureDiagnostic("при импорте STEP", failure));
  return {};
}

bool importDocumentStep(const QString& path, Document* document,
                        const QString& featureName, QString* error) {
  if (error) error->clear();
  bool imported = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] {
        imported = importDocumentStepImpl(path, document, featureName, error);
      },
      &failure);
  if (completed) return imported;
  setError(error, failureDiagnostic("при добавлении STEP в документ", failure));
  return false;
}

bool exportDocumentStep(const QString& path, const Document& document,
                        QString* error) {
  if (error) error->clear();
  bool exported = false;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&] { exported = exportDocumentStepImpl(path, document, error); },
      &failure);
  if (completed) return exported;
  setError(error, failureDiagnostic("при экспорте STEP", failure));
  return false;
}

}  // namespace solidar::io
