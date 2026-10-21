#include "TestAssertions.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <cstdlib>
#include <limits>
#include <string>

#include "model/Document.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4EnvelopeDecoder.h"

namespace {

using solidar::project::ProjectFormatV4DeferredUnknownKind;
using solidar::project::ProjectFormatV4EnvelopeScanResult;
using solidar::project::ProjectFormatV4EnvelopeScanStatus;

template <typename T>
concept HasSucceeded = requires(const T& value) { value.succeeded(); };

template <typename T>
concept HasSuccessMember = requires(const T& value) { value.success; };

static_assert(!HasSucceeded<ProjectFormatV4EnvelopeScanResult>);
static_assert(!HasSuccessMember<ProjectFormatV4EnvelopeScanResult>);

struct RootValues {
  QByteArray format{R"("solidar-project")"};
  QByteArray version{"4"};
  QByteArray capabilities{R"(["persistent-topology-v2"])"};
  QByteArray constraintTypeEncoding{R"("stable-name-v1")"};
  QByteArray name{R"("fixture")"};
  QByteArray createdAt{R"("2026-10-21T00:00:00Z")"};
  QByteArray document{R"({"widthMm":60,"heightMm":40,"extrusionMm":25})"};
  QByteArray sketches{"[]"};
  QByteArray extrusion{"{}"};
  QByteArray model{R"({"sketches":[],"bodies":[],"referenceImages":[]})"};
};

constexpr std::array<const char*, 10> kRootKeys{
    "format",       "version",   "capabilities", "constraintTypeEncoding",
    "name",         "createdAt", "document",     "sketches",
    "extrusion",    "model"};

QByteArray fieldValue(const RootValues& values, int index) {
  switch (index) {
    case 0:
      return values.format;
    case 1:
      return values.version;
    case 2:
      return values.capabilities;
    case 3:
      return values.constraintTypeEncoding;
    case 4:
      return values.name;
    case 5:
      return values.createdAt;
    case 6:
      return values.document;
    case 7:
      return values.sketches;
    case 8:
      return values.extrusion;
    default:
      return values.model;
  }
}

QByteArray rootBytes(const RootValues& values = {}, int omitted = -1,
                     QByteArray extra = {}) {
  QByteArray bytes{"{"};
  bool first = true;
  for (int index = 0; index < static_cast<int>(kRootKeys.size()); ++index) {
    if (index == omitted) continue;
    if (!first) bytes += ',';
    first = false;
    bytes += '"';
    bytes += kRootKeys[static_cast<std::size_t>(index)];
    bytes += R"(":)";
    bytes += fieldValue(values, index);
  }
  if (!extra.isEmpty()) {
    if (!first) bytes += ',';
    bytes += extra;
  }
  bytes += '}';
  return bytes;
}

ProjectFormatV4EnvelopeScanResult scan(const QByteArray& bytes) {
  return solidar::project::scanProjectFormatV4Envelope(
      QByteArrayView(bytes));
}

void checkFailure(const ProjectFormatV4EnvelopeScanResult& result,
                  ProjectFormatV4EnvelopeScanStatus status) {
  CHECK(result.status == status);
  CHECK(!result.hasCandidate());
  CHECK(!result.candidate.has_value());
  CHECK(!result.error.isEmpty());
}

void checkInvalid(const QByteArray& bytes) {
  checkFailure(scan(bytes), ProjectFormatV4EnvelopeScanStatus::Invalid);
}

QByteArray quotedUtf8(const QString& value) {
  QByteArray result{"\""};
  result += value.toUtf8();
  result += '"';
  return result;
}

QByteArray arrayOfNulls(qsizetype count) {
  QByteArray result{"["};
  for (qsizetype index = 0; index < count; ++index) {
    if (index != 0) result += ',';
    result += "null";
  }
  result += ']';
  return result;
}

QByteArray documentWithOmittedField(int omitted) {
  constexpr std::array<const char*, 3> fields{"widthMm", "heightMm",
                                               "extrusionMm"};
  QByteArray result{"{"};
  bool first = true;
  for (int index = 0; index < static_cast<int>(fields.size()); ++index) {
    if (index == omitted) continue;
    if (!first) result += ',';
    first = false;
    result += '"';
    result += fields[static_cast<std::size_t>(index)];
    result += R"(":1)";
  }
  result += '}';
  return result;
}

QByteArray modelWithFieldVariation(int omitted, int wrongType) {
  constexpr std::array<const char*, 3> fields{"sketches", "bodies",
                                               "referenceImages"};
  QByteArray result{"{"};
  bool first = true;
  for (int index = 0; index < static_cast<int>(fields.size()); ++index) {
    if (index == omitted) continue;
    if (!first) result += ',';
    first = false;
    result += '"';
    result += fields[static_cast<std::size_t>(index)];
    result += R"(":)";
    result += index == wrongType ? "{}" : "[]";
  }
  result += '}';
  return result;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(bytes) != bytes.size())
    return false;
  file.close();
  return true;
}

}  // namespace

int main() {
  // Minimal envelope and exact authored outer payload projection.
  {
    const auto result = scan(rootBytes());
    CHECK(result.status == ProjectFormatV4EnvelopeScanStatus::Candidate);
    CHECK(result.hasCandidate());
    CHECK(result.error.isEmpty());
    CHECK(result.candidate->authoredRoot.value("name").toString() ==
          QStringLiteral("fixture"));
    CHECK(result.candidate->document.value("widthMm").toDouble() == 60.0);
    CHECK(result.candidate->legacySketches.isEmpty());
    CHECK(result.candidate->legacyExtrusion.isEmpty());
    CHECK(result.candidate->capabilities ==
          QStringList{QStringLiteral("persistent-topology-v2")});
    CHECK(result.candidate->deferredUnknowns.empty());
  }

  // Unknown tokens and malformed opaque nested elements coexist in a
  // Candidate. Independently parse the authored bytes as the preservation
  // oracle and compare every projected JSON value in full.
  {
    RootValues values;
    values.capabilities =
        R"(["future-a","persistent-topology-v2","z-future"])";
    values.constraintTypeEncoding = R"("future-name-v2")";
    values.sketches = R"([{"sentinel":[1,"two",null]},17])";
    values.extrusion = R"({"sentinel":{"future":true}})";
    values.model =
        R"({"sketches":[false,{"bad":1}],"bodies":["opaque",null],"referenceImages":[42]})";
    const QByteArray authoredBytes = rootBytes(values);
    QJsonParseError oracleError;
    const QJsonDocument oracleDocument =
        QJsonDocument::fromJson(authoredBytes, &oracleError);
    CHECK(oracleError.error == QJsonParseError::NoError);
    CHECK(oracleDocument.isObject());
    const QJsonObject oracleRoot = oracleDocument.object();

    const auto result = scan(authoredBytes);
    CHECK(result.hasCandidate());
    CHECK(result.candidate->authoredRoot == oracleRoot);
    CHECK(result.candidate->document ==
          oracleRoot.value("document").toObject());
    CHECK(result.candidate->legacySketches ==
          oracleRoot.value("sketches").toArray());
    CHECK(result.candidate->legacyExtrusion ==
          oracleRoot.value("extrusion").toObject());
    CHECK(result.candidate->model == oracleRoot.value("model").toObject());
    CHECK(result.candidate->capabilities ==
          QStringList({QStringLiteral("future-a"),
                       QStringLiteral("persistent-topology-v2"),
                       QStringLiteral("z-future")}));
    CHECK(result.candidate->deferredUnknowns.size() == 3);
    CHECK(result.candidate->deferredUnknowns[0] ==
          (solidar::project::ProjectFormatV4DeferredUnknown{
              ProjectFormatV4DeferredUnknownKind::Capability,
              QStringLiteral("$.capabilities[0]"),
              QStringLiteral("future-a")}));
    CHECK(result.candidate->deferredUnknowns[1] ==
          (solidar::project::ProjectFormatV4DeferredUnknown{
              ProjectFormatV4DeferredUnknownKind::Capability,
              QStringLiteral("$.capabilities[2]"),
              QStringLiteral("z-future")}));
    CHECK(result.candidate->deferredUnknowns[2] ==
          (solidar::project::ProjectFormatV4DeferredUnknown{
              ProjectFormatV4DeferredUnknownKind::ConstraintTypeEncoding,
              QStringLiteral("$.constraintTypeEncoding"),
              QStringLiteral("future-name-v2")}));
  }

  // Unknown bounded tokens are deferred, but never mask an envelope defect.
  {
    RootValues values;
    values.capabilities =
        R"(["future-a","persistent-topology-v2","z-future"])";
    values.constraintTypeEncoding = R"("future-name-v2")";
    auto result = scan(rootBytes(values));
    CHECK(result.hasCandidate());
    CHECK(result.candidate->deferredUnknowns.size() == 3);
    CHECK(result.candidate->deferredUnknowns[0].kind ==
          ProjectFormatV4DeferredUnknownKind::Capability);
    CHECK(result.candidate->deferredUnknowns[0].path ==
          QStringLiteral("$.capabilities[0]"));
    CHECK(result.candidate->deferredUnknowns[2].kind ==
          ProjectFormatV4DeferredUnknownKind::ConstraintTypeEncoding);
    CHECK(result.candidate->deferredUnknowns[2].token ==
          QStringLiteral("future-name-v2"));

    values.document = R"({"widthMm":60,"heightMm":40})";
    checkInvalid(rootBytes(values));
    values.document = R"({"widthMm":60,"heightMm":40,"extrusionMm":25})";
    values.model = R"({"sketches":[],"bodies":[]})";
    checkInvalid(rootBytes(values));
    values.model = R"({"sketches":[],"bodies":[],"referenceImages":[]})";
    values.capabilities = R"(["z-future","persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
  }

  // Byte and JSON dispatch boundary.
  {
    checkInvalid({});
    checkInvalid("{");
    checkInvalid("[]");
    checkInvalid("null");
    QByteArray oversized(
        solidar::project::ProjectFile::kMaximumFileBytes + 1, ' ');
    checkInvalid(oversized);

    QByteArray exact = rootBytes();
    exact.append(solidar::project::ProjectFile::kMaximumFileBytes - exact.size(),
                 ' ');
    CHECK(scan(exact).hasCandidate());
  }

  // Both discriminators are validated before opaque foreign content is
  // classified NotApplicable. No v4 schema checks are run afterwards.
  {
    RootValues values;
    values.format = R"("foreign-project")";
    values.version = "91";
    auto result = scan(rootBytes(values, 2, R"("opaque":{"anything":true})"));
    checkFailure(result, ProjectFormatV4EnvelopeScanStatus::NotApplicable);

    values.format = R"("solidar-project")";
    values.version = "3";
    result = scan(rootBytes(values, 6, R"("opaque":[1,2,3])"));
    checkFailure(result, ProjectFormatV4EnvelopeScanStatus::NotApplicable);

    values.version = "4.5";
    checkInvalid(rootBytes(values));
    values.version = "0";
    checkInvalid(rootBytes(values));
    values.version = "-1";
    checkInvalid(rootBytes(values));
    values.version = "9007199254740992";
    checkInvalid(rootBytes(values));
    values.version = "1";
    checkFailure(scan(rootBytes(values)),
                 ProjectFormatV4EnvelopeScanStatus::NotApplicable);
    values.version = "9007199254740991";
    checkFailure(scan(rootBytes(values)),
                 ProjectFormatV4EnvelopeScanStatus::NotApplicable);
    values.version = R"("4")";
    checkInvalid(rootBytes(values));
    values.version = "4";
    values.format = R"("")";
    checkInvalid(rootBytes(values));
    values.format = "7";
    checkInvalid(rootBytes(values));
    values.format = quotedUtf8(QString(16385, QChar('f')));
    checkInvalid(rootBytes(values));
    values.format = R"("\uD800")";
    checkInvalid(rootBytes(values));
  }

  // Every root field is mandatory, correctly typed, and the root is closed.
  {
    for (int index = 0; index < static_cast<int>(kRootKeys.size()); ++index)
      checkInvalid(rootBytes({}, index));
    checkInvalid(rootBytes({}, -1, R"("extra":true)"));

    const std::array<QByteArray, 10> wrongTypes{
        "false", "false", "{}", "[]", "7", "null", "[]", "{}", "[]",
        "[]"};
    for (int index = 0; index < static_cast<int>(wrongTypes.size()); ++index) {
      RootValues values;
      switch (index) {
        case 0: values.format = wrongTypes[index]; break;
        case 1: values.version = wrongTypes[index]; break;
        case 2: values.capabilities = wrongTypes[index]; break;
        case 3: values.constraintTypeEncoding = wrongTypes[index]; break;
        case 4: values.name = wrongTypes[index]; break;
        case 5: values.createdAt = wrongTypes[index]; break;
        case 6: values.document = wrongTypes[index]; break;
        case 7: values.sketches = wrongTypes[index]; break;
        case 8: values.extrusion = wrongTypes[index]; break;
        case 9: values.model = wrongTypes[index]; break;
      }
      checkInvalid(rootBytes(values));
    }
  }

  // Exact document schema and inherited v3 length ranges.
  {
    RootValues values;
    for (int omitted = 0; omitted < 3; ++omitted) {
      values.document = documentWithOmittedField(omitted);
      checkInvalid(rootBytes(values));
    }
    values.document =
        R"({"widthMm":1,"heightMm":2,"extrusionMm":3,"extra":4})";
    checkInvalid(rootBytes(values));
    for (const QByteArray number : {QByteArray("0"), QByteArray("-1"),
                                    QByteArray("100001"),
                                    QByteArray("\"1\"")}) {
      values.document = QByteArray("{\"widthMm\":") + number +
                        R"(,"heightMm":1,"extrusionMm":1})";
      checkInvalid(rootBytes(values));
    }
    values.document =
        R"({"widthMm":0.000001,"heightMm":100000,"extrusionMm":1})";
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.document = QByteArray("{\"widthMm\":") +
                      QByteArray::number(
                          std::nextafter(100000.0,
                                         std::numeric_limits<double>::infinity()),
                          'g', 17) +
                      R"(,"heightMm":1,"extrusionMm":1})";
    checkInvalid(rootBytes(values));
    values.document =
        R"({"widthMm":NaN,"heightMm":1,"extrusionMm":1})";
    checkInvalid(rootBytes(values));
  }

  // UTF-16 boundaries for authored metadata and encoding discriminator.
  {
    RootValues values;
    values.name = quotedUtf8(QString(16384, QChar('n')));
    values.createdAt = quotedUtf8(QString(16384, QChar('d')));
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.name = quotedUtf8(QString(16385, QChar('n')));
    checkInvalid(rootBytes(values));
    values.name = R"("\uD800")";
    checkInvalid(rootBytes(values));
    values.name = R"("ok")";
    values.createdAt = R"("\uDC00")";
    checkInvalid(rootBytes(values));
    values.createdAt = R"("ok")";
    values.constraintTypeEncoding = R"("")";
    checkInvalid(rootBytes(values));
    values.constraintTypeEncoding = quotedUtf8(QString(16385, QChar('e')));
    checkInvalid(rootBytes(values));
    values.constraintTypeEncoding = R"("\uD800")";
    checkInvalid(rootBytes(values));
  }

  // Capability count, token bounds, required token, and canonical unsigned
  // UTF-8 ordering (which intentionally differs from UTF-16 ordering here).
  {
    RootValues values;
    values.capabilities = "[]";
    checkInvalid(rootBytes(values));
    values.capabilities = R"([7,"persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
    values.capabilities = R"(["","persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
    values.capabilities = R"(["\uD800","persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
    values.capabilities = QByteArray("[") +
                          quotedUtf8(QString(64, QChar('a'))) +
                          R"(,"persistent-topology-v2"])";
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.capabilities = QByteArray("[") +
                          quotedUtf8(QString(65, QChar('a'))) +
                          R"(,"persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
    values.capabilities =
        R"(["persistent-topology-v2","persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
    values.capabilities = R"(["z","persistent-topology-v2"])";
    checkInvalid(rootBytes(values));
    values.capabilities = R"(["future-only"])";
    checkInvalid(rootBytes(values));

    const QString privateUse(QChar(0xe000));
    const QString supplementary = QString::fromUcs4(U"\U00010000");
    values.capabilities = QByteArray(R"(["persistent-topology-v2",)") +
                          quotedUtf8(privateUse) + ',' +
                          quotedUtf8(supplementary) + ']';
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.capabilities = QByteArray(R"(["persistent-topology-v2",)") +
                          quotedUtf8(supplementary) + ',' +
                          quotedUtf8(privateUse) + ']';
    checkInvalid(rootBytes(values));

    QByteArray sixteen{"["};
    for (int index = 0; index < 15; ++index) {
      if (index) sixteen += ',';
      sixteen += QByteArray("\"a") + QByteArray::number(index + 10) + '"';
    }
    sixteen += R"(,"persistent-topology-v2"])";
    values.capabilities = sixteen;
    CHECK(scan(rootBytes(values)).hasCandidate());
    sixteen.chop(1);
    sixteen += R"(,"z-last"])";
    values.capabilities = sixteen;
    checkInvalid(rootBytes(values));
  }

  // Closed model shell and immediate collection limits. Elements stay opaque.
  {
    RootValues values;
    for (int field = 0; field < 3; ++field) {
      values.model = modelWithFieldVariation(field, -1);
      checkInvalid(rootBytes(values));
      values.model = modelWithFieldVariation(-1, field);
      checkInvalid(rootBytes(values));
    }
    values.model =
        R"({"sketches":[],"bodies":[],"referenceImages":[],"extra":[]})";
    checkInvalid(rootBytes(values));

    values.model =
        R"({"sketches":[],"bodies":[],"referenceImages":[]})";
    values.sketches =
        arrayOfNulls(solidar::project::ProjectFile::kMaximumDocumentSketches);
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.sketches = arrayOfNulls(
        solidar::project::ProjectFile::kMaximumDocumentSketches + 1);
    checkInvalid(rootBytes(values));
    values.sketches = "[]";

    values.model = QByteArray("{\"sketches\":") +
                   arrayOfNulls(
                       solidar::project::ProjectFile::kMaximumDocumentSketches) +
                   R"(,"bodies":[],"referenceImages":[]})";
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.model = QByteArray("{\"sketches\":") +
                   arrayOfNulls(
                       solidar::project::ProjectFile::kMaximumDocumentSketches +
                       1) +
                   R"(,"bodies":[],"referenceImages":[]})";
    checkInvalid(rootBytes(values));
    values.model = QByteArray("{\"sketches\":[],\"bodies\":") +
                   arrayOfNulls(
                       solidar::project::ProjectFile::kMaximumDocumentBodies) +
                   R"(,"referenceImages":[]})";
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.model = QByteArray("{\"sketches\":[],\"bodies\":") +
                   arrayOfNulls(
                       solidar::project::ProjectFile::kMaximumDocumentBodies + 1) +
                   R"(,"referenceImages":[]})";
    checkInvalid(rootBytes(values));
    values.model = QByteArray(
                       "{\"sketches\":[],\"bodies\":[],\"referenceImages\":") +
                   arrayOfNulls(
                       solidar::project::ProjectFile::kMaximumCollectionItems) +
                   '}';
    CHECK(scan(rootBytes(values)).hasCandidate());
    values.model = QByteArray(
                       "{\"sketches\":[],\"bodies\":[],\"referenceImages\":") +
                   arrayOfNulls(
                       solidar::project::ProjectFile::kMaximumCollectionItems + 1) +
                   '}';
    checkInvalid(rootBytes(values));
  }

  // Classification and diagnostics are deterministic and failure never leaks
  // a candidate.
  {
    RootValues values;
    values.model = R"({"sketches":[],"bodies":[]})";
    const auto first = scan(rootBytes(values));
    const auto second = scan(rootBytes(values));
    checkFailure(first, ProjectFormatV4EnvelopeScanStatus::Invalid);
    checkFailure(second, ProjectFormatV4EnvelopeScanStatus::Invalid);
    CHECK(first.error == second.error);
  }

  // Isolation guard: a scanner candidate remains unsupported by production
  // loading and cannot mutate an existing destination. The writer remains v3.
  {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("v4-envelope-tests-XXXXXX")));
    CHECK(directory.isValid());
    const QString candidatePath = directory.filePath("candidate-v4.solidar");
    CHECK(writeBytes(candidatePath, rootBytes()));
    CHECK(scan(rootBytes()).hasCandidate());
    const auto staged = solidar::project::ProjectFile::stageLoad(candidatePath);
    CHECK(staged.status == solidar::project::ProjectLoadStatus::Unsupported);
    CHECK(staged.sourceVersion == 4);
    CHECK(staged.payloadKind == solidar::project::ProjectPayloadKind::None);
    CHECK(!staged.succeeded());
    CHECK(!staged.document.has_value());
    CHECK(!staged.error.isEmpty());

    solidar::Document destination;
    destination.setBox({91.0, 82.0, 73.0});
    auto& sentinelSketch = destination.addSketch("sentinel-sketch");
    sentinelSketch.geometry.addLine({1.25, -2.5}, {7.75, 9.5});
    const auto sentinelSketchId = sentinelSketch.id;
    auto& sentinelBody = destination.addBody("sentinel-body");
    sentinelBody.setVisible(false);
    const auto sentinelBodyId = sentinelBody.id();
    QString error;
    CHECK(!solidar::project::ProjectFile::loadDocument(candidatePath,
                                                       &destination, &error));
    CHECK(!error.isEmpty());
    CHECK(destination.sketches().size() == 1);
    CHECK(destination.bodies().size() == 1);
    CHECK(destination.sketches().front().id == sentinelSketchId);
    CHECK(destination.sketches().front().name == "sentinel-sketch");
    CHECK(destination.sketches().front().geometry.lines().size() == 1);
    CHECK(destination.sketches().front().geometry.lines().front().start ==
          (solidar::sketch::Point{1.25, -2.5}));
    CHECK(destination.sketches().front().geometry.lines().front().end ==
          (solidar::sketch::Point{7.75, 9.5}));
    CHECK(destination.bodies().front().id() == sentinelBodyId);
    CHECK(destination.bodies().front().name() == "sentinel-body");
    CHECK(!destination.bodies().front().visible());
    CHECK(destination.bodies().front().features().empty());
    CHECK(destination.box().widthMm == 91.0);
    CHECK(destination.box().depthMm == 82.0);
    CHECK(destination.box().heightMm == 73.0);

    const QString savedPath = directory.filePath("writer-output.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(savedPath, destination,
                                                      &error));
    QFile saved(savedPath);
    CHECK(saved.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const auto written = QJsonDocument::fromJson(saved.readAll(), &parseError);
    CHECK(parseError.error == QJsonParseError::NoError);
    CHECK(written.object().value("version").toInt() == 3);
  }

  return EXIT_SUCCESS;
}
