#include "TestAssertions.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "project/ProjectFile.h"

namespace {

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(bytes) == bytes.size();
}

bool destinationIsSentinel(const solidar::Document& document,
                           solidar::SketchId sketchId) {
  const auto& box = document.box();
  const auto* sketch = document.findSketch(sketchId);
  return box.widthMm == 91.0 && box.depthMm == 92.0 &&
         box.heightMm == 93.0 && document.sketches().size() == 1 &&
         document.bodies().empty() && sketch && sketch->name == "sentinel";
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory(
      QDir::current().filePath("project-decoder-property-tests-XXXXXX"));
  CHECK(directory.isValid());

  const QString seedPath = directory.filePath("valid-seed.solidar");
  QString error;
  CHECK(solidar::project::ProjectFile::create(seedPath, &error));

  QFile seedFile(seedPath);
  CHECK(seedFile.open(QIODevice::ReadOnly));
  const QJsonDocument parsedSeed = QJsonDocument::fromJson(seedFile.readAll());
  seedFile.close();
  CHECK(parsedSeed.isObject());
  QJsonObject stableSeed = parsedSeed.object();
  stableSeed["createdAt"] = QStringLiteral("2026-10-10T00:00:00.000Z");
  const QByteArray seed =
      QJsonDocument(stableSeed).toJson(QJsonDocument::Compact);
  CHECK(!seed.isEmpty());
  CHECK(writeBytes(seedPath, seed));
  CHECK(solidar::project::ProjectFile::stageLoad(seedPath).kind ==
        solidar::project::ProjectLoadKind::ValidV2);

  std::vector<QByteArray> cases{
      QByteArray{},
      QByteArray{"{"},
      QByteArray{"[]"},
      QByteArray{"not-json"},
      QByteArray{"{\"format\":\"solidar-project\",\"version\":2}"},
      seed.left(seed.size() / 2),
  };

  // Fixed-seed byte mutations are a deterministic property suite rather than
  // a collection of hand-picked parser examples. A mutation is allowed to
  // remain valid; failures must be stable, diagnosed and transactional.
  std::mt19937 generator(0x534f4c49U);
  std::uniform_int_distribution<int> byteDistribution(0, 255);
  for (int iteration = 0; iteration < 192; ++iteration) {
    QByteArray mutated = seed;
    const qsizetype position =
        static_cast<qsizetype>(generator() %
                               static_cast<std::uint32_t>(mutated.size()));
    switch (iteration % 4) {
      case 0:
        mutated[position] = static_cast<char>(byteDistribution(generator));
        break;
      case 1: {
        const qsizetype available = mutated.size() - position;
        const qsizetype count = qMin<qsizetype>(
            available, 1 + static_cast<qsizetype>(generator() % 8));
        mutated.remove(position, count);
        break;
      }
      case 2:
        mutated.insert(position,
                       static_cast<char>(byteDistribution(generator)));
        break;
      case 3: {
        static constexpr std::array<char, 8> structuralBytes{
            '{', '}', '[', ']', ':', ',', '"', '\0'};
        mutated[position] =
            structuralBytes[static_cast<std::size_t>(iteration) %
                            structuralBytes.size()];
        break;
      }
    }
    cases.push_back(std::move(mutated));
  }

  for (std::size_t index = 0; index < cases.size(); ++index) {
    const QString path =
        directory.filePath(QStringLiteral("mutation-%1.solidar").arg(index));
    CHECK(writeBytes(path, cases[index]));

    const auto first = solidar::project::ProjectFile::stageLoad(path);
    const auto second = solidar::project::ProjectFile::stageLoad(path);
    CHECK(first.kind == second.kind);
    CHECK(first.succeeded() == second.succeeded());

    if (!first.succeeded()) {
      CHECK(!first.error.isEmpty());

      solidar::Document destination;
      destination.setBox({91.0, 92.0, 93.0});
      const auto sentinelId = destination.addSketch("sentinel").id;
      QString loadError;
      CHECK(!solidar::project::ProjectFile::loadDocument(
          path, &destination, &loadError));
      CHECK(!loadError.isEmpty());
      CHECK(destinationIsSentinel(destination, sentinelId));
    }

    QFile after(path);
    CHECK(after.open(QIODevice::ReadOnly));
    CHECK(after.readAll() == cases[index]);
  }

  return 0;
}
