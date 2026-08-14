#include "project/ProjectFile.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace solidar::project {
namespace {

void setError(QString* target, const QString& value) {
  if (target) *target = value;
}

}  // namespace

bool ProjectFile::create(const QString& path, QString* error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    setError(error, file.errorString());
    return false;
  }
  QJsonObject root;
  root["format"] = "solidar-project";
  root["version"] = 1;
  root["name"] = QFileInfo(path).completeBaseName();
  root["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  root["document"] = QJsonObject{{"widthMm", 60.0},
                                  {"heightMm", 40.0},
                                  {"extrusionMm", 25.0}};
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!file.commit()) {
    setError(error, file.errorString());
    return false;
  }
  return true;
}

bool ProjectFile::validate(const QString& path, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(error, file.errorString());
    return false;
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    setError(error, QString::fromUtf8("Файл проекта повреждён: ") +
                        parseError.errorString());
    return false;
  }
  const auto root = document.object();
  if (root.value("format").toString() != "solidar-project" ||
      root.value("version").toInt() != 1) {
    setError(error, QString::fromUtf8("Неподдерживаемый формат проекта."));
    return false;
  }
  return true;
}

}  // namespace solidar::project
