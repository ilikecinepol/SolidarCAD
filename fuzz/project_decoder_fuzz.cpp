#include <QFile>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdint>

#include "project/ProductEnvelopeV5Decoder.h"
#include "project/ProductEnvelopeV6Decoder.h"
#include "project/ProductEnvelopeV7Decoder.h"
#include "project/ProductEnvelopeV8Decoder.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/ProductEnvelopeV10Decoder.h"
#include "project/ProductEnvelopeV11Decoder.h"
#include "project/ProductEnvelopeV12Decoder.h"
#include "project/ProjectFile.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
  if (size > static_cast<std::size_t>(
                 solidar::project::ProjectFile::kMaximumFileBytes) +
                 1U)
    return 0;

  const QByteArrayView bytes(reinterpret_cast<const char*>(data),
                             static_cast<qsizetype>(size));
  (void)solidar::project::scanProductEnvelopeV5(bytes);
  (void)solidar::project::scanProductEnvelopeV6(bytes);
  (void)solidar::project::scanProductEnvelopeV7(bytes);
  (void)solidar::project::scanProductEnvelopeV8(bytes);
  (void)solidar::project::scanProductEnvelopeV9(bytes);
  (void)solidar::project::scanProductEnvelopeV10(bytes);
  (void)solidar::project::scanProductEnvelopeV11(bytes);
  (void)solidar::project::scanProductEnvelopeV12(bytes);

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
