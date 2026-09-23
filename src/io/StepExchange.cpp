#include "io/StepExchange.h"

#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "model/Document.h"
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

QString failureMessage(const char* prefix, const Standard_Failure& failure) {
  const char* detail = failure.what();
  return QString::fromUtf8(prefix) +
         (detail && *detail ? QStringLiteral(": ") + QString::fromUtf8(detail)
                            : QString{});
}

TopoDS_Shape documentShape(const Document& document) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  TopoDS_Shape single;
  std::size_t count = 0;
  for (const Body& body : document.bodies()) {
    const auto shape = body.resultShape();
    if (!shape || shape->IsNull()) continue;
    single = *shape;
    builder.Add(compound, *shape);
    ++count;
  }
  if (count == 0) return {};
  return count == 1 ? single : TopoDS_Shape(compound);
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

}  // namespace

std::shared_ptr<const TopoDS_Shape> readStepFile(const QString& path,
                                                QString* error) {
  if (error) error->clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(error, QString::fromUtf8("Не удалось открыть STEP-файл: ") +
                        file.errorString());
    return {};
  }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFileDevice::NoError) {
    setError(error, QString::fromUtf8("Не удалось прочитать STEP-файл: ") +
                        file.errorString());
    return {};
  }
  if (bytes.isEmpty()) {
    setError(error, QString::fromUtf8("STEP-файл пуст."));
    return {};
  }

  try {
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
    const int transferred = reader.TransferRoots();
    if (transferred <= 0) {
      setError(error,
               QString::fromUtf8("STEP-файл не содержит переносимой B-Rep геометрии."));
      return {};
    }
    TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) {
      setError(error, QString::fromUtf8("После импорта получена пустая геометрия."));
      return {};
    }
    return std::make_shared<const TopoDS_Shape>(std::move(shape));
  } catch (const Standard_Failure& failure) {
    setError(error, failureMessage("Ошибка OCCT при импорте STEP", failure));
  } catch (const std::exception& exception) {
    setError(error, QString::fromUtf8("Ошибка при импорте STEP: ") +
                        QString::fromUtf8(exception.what()));
  } catch (...) {
    setError(error, QString::fromUtf8("Неизвестная ошибка при импорте STEP."));
  }
  return {};
}

bool importDocumentStep(const QString& path, Document* document,
                        const QString& featureName, QString* error) {
  if (!document) {
    setError(error, QString::fromUtf8("Не задан документ для импорта STEP."));
    return false;
  }
  const auto shape = readStepFile(path, error);
  if (!shape) return false;

  try {
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
  } catch (const Standard_Failure& failure) {
    setError(error, failureMessage("Ошибка OCCT при добавлении STEP", failure));
  } catch (const std::exception& exception) {
    setError(error, QString::fromUtf8("Ошибка при добавлении STEP: ") +
                        QString::fromUtf8(exception.what()));
  } catch (...) {
    setError(error, QString::fromUtf8("Неизвестная ошибка при добавлении STEP."));
  }
  return false;
}

bool exportDocumentStep(const QString& path, const Document& document,
                        QString* error) {
  if (error) error->clear();
  const TopoDS_Shape shape = documentShape(document);
  if (shape.IsNull()) {
    setError(error,
             QString::fromUtf8("Документ не содержит B-Rep геометрии для экспорта."));
    return false;
  }

  try {
    std::lock_guard lock(stepTranslatorMutex());
    // Constructing the writer initializes the STEP controller and registers
    // its Interface_Static parameters before the schema is changed.
    STEPControl_Writer writer;
    ScopedStepSchema schema;
    if (!schema.applied()) {
      setError(error, QString::fromUtf8("Не удалось включить схему STEP AP242DIS."));
      return false;
    }
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) {
      setError(error, QString::fromUtf8("OCCT не смог подготовить геометрию для STEP."));
      return false;
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
      setError(error, QString::fromUtf8("Не удалось полностью записать STEP-файл: ") +
                          file.errorString());
      file.cancelWriting();
      return false;
    }
    if (!file.commit()) {
      setError(error, QString::fromUtf8("Не удалось завершить запись STEP-файла: ") +
                          file.errorString());
      return false;
    }
    return true;
  } catch (const Standard_Failure& failure) {
    setError(error, failureMessage("Ошибка OCCT при экспорте STEP", failure));
  } catch (const std::exception& exception) {
    setError(error, QString::fromUtf8("Ошибка при экспорте STEP: ") +
                        QString::fromUtf8(exception.what()));
  } catch (...) {
    setError(error, QString::fromUtf8("Неизвестная ошибка при экспорте STEP."));
  }
  return false;
}

}  // namespace solidar::io
