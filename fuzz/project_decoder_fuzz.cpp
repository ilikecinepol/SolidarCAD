#include <QFile>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdint>

#include "project/ProjectFile.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
  if (size > static_cast<std::size_t>(
                 solidar::project::ProjectFile::kMaximumFileBytes) +
                 1U)
    return 0;

  // The harness owns exactly one process-local temporary directory. No fuzz
  // input can influence the path, and all decoder I/O stays inside it.
  static QTemporaryDir directory(QStringLiteral("solidar-project-fuzz-XXXXXX"));
  if (!directory.isValid()) return 0;

  const QString path = directory.filePath(QStringLiteral("input.solidar"));
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 0;
  if (size > 0 &&
      file.write(reinterpret_cast<const char*>(data),
                 static_cast<qint64>(size)) != static_cast<qint64>(size))
    return 0;
  file.close();

  (void)solidar::project::ProjectFile::stageLoad(path);
  return 0;
}
