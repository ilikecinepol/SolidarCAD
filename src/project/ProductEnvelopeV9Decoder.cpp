#include "project/ProductEnvelopeV9Decoder.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "project/ProjectFile.h"
#include "project/ProjectFormatV4ReferenceDecoder.h"

namespace solidar::project {
namespace {

constexpr double kHalfPi =
    1.570796326794896619231321691639751442;

ProductEnvelopeV9ScanResult fail(ProductEnvelopeV9ScanStatus status,
                                 QString error) {
  ProductEnvelopeV9ScanResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}
ProductEnvelopeV9ScanResult invalid(QString error) {
  return fail(ProductEnvelopeV9ScanStatus::Invalid, std::move(error));
}
ProductEnvelopeV9ScanResult unsupported(QString error) {
  return fail(ProductEnvelopeV9ScanStatus::Unsupported, std::move(error));
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> fields) {
  if (object.size() != static_cast<qsizetype>(fields.size())) return false;
  for (const auto* field : fields)
    if (!object.contains(QLatin1String(field))) return false;
  return true;
}

bool integer(const QJsonValue& value, std::uint64_t minimum,
             std::uint64_t maximum, std::uint64_t* output) {
  if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
      std::trunc(value.toDouble()) != value.toDouble() ||
      value.toDouble() < static_cast<double>(minimum) ||
      value.toDouble() > static_cast<double>(maximum))
    return false;
  *output = static_cast<std::uint64_t>(value.toDouble());
  return true;
}

bool finiteNumber(const QJsonValue& value, double minimum, double maximum,
                  bool minimumExclusive, double* output) {
  if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
  const double number = value.toDouble();
  if ((minimumExclusive ? number <= minimum : number < minimum) ||
      number > maximum)
    return false;
  *output = number;
  return true;
}

enum class RawContext { Normal, Options, MateArray, Mate };
enum class RawTokenStatus { Valid, ForbiddenNegativeZero, Malformed };

class RawV9NumberScanner final {
 public:
  explicit RawV9NumberScanner(QByteArrayView bytes) : bytes_(bytes) {}

  RawTokenStatus scan() noexcept {
    if (!value(RawContext::Normal, false, 0))
      return forbidden_ ? RawTokenStatus::ForbiddenNegativeZero
                        : RawTokenStatus::Malformed;
    whitespace();
    return offset_ == bytes_.size() ? RawTokenStatus::Valid
                                    : RawTokenStatus::Malformed;
  }

 private:
  void whitespace() noexcept {
    while (offset_ < bytes_.size() &&
           (bytes_[offset_] == ' ' || bytes_[offset_] == '\t' ||
            bytes_[offset_] == '\r' || bytes_[offset_] == '\n'))
      ++offset_;
  }

  bool string(QByteArray* decoded) noexcept {
    if (offset_ >= bytes_.size() || bytes_[offset_] != '"') return false;
    if (decoded) decoded->clear();
    ++offset_;
    while (offset_ < bytes_.size()) {
      const unsigned char current =
          static_cast<unsigned char>(bytes_[offset_++]);
      if (current == '"') return true;
      if (current < 0x20) return false;
      if (current != '\\') {
        if (decoded && decoded->size() < 64)
          decoded->append(current < 0x80 ? static_cast<char>(current) : '#');
        continue;
      }
      if (offset_ >= bytes_.size()) return false;
      const char escaped = bytes_[offset_++];
      if (escaped == 'u') {
        if (bytes_.size() - offset_ < 4) return false;
        unsigned code = 0;
        for (int digit = 0; digit < 4; ++digit) {
          const char hex = bytes_[offset_++];
          code <<= 4;
          if (hex >= '0' && hex <= '9') code += hex - '0';
          else if (hex >= 'a' && hex <= 'f') code += hex - 'a' + 10;
          else if (hex >= 'A' && hex <= 'F') code += hex - 'A' + 10;
          else return false;
        }
        if (decoded && decoded->size() < 64)
          decoded->append(code < 0x80 ? static_cast<char>(code) : '#');
      } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                 escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                 escaped != 'r' && escaped != 't') {
        return false;
      }
    }
    return false;
  }

  bool number(bool rejectNegativeZero) noexcept {
    const bool negative = offset_ < bytes_.size() && bytes_[offset_] == '-';
    if (negative) ++offset_;
    if (offset_ >= bytes_.size()) return false;
    bool nonzeroSignificand = false;
    if (bytes_[offset_] == '0') {
      ++offset_;
    } else if (bytes_[offset_] >= '1' && bytes_[offset_] <= '9') {
      do {
        nonzeroSignificand =
            nonzeroSignificand || bytes_[offset_] != '0';
        ++offset_;
      } while (offset_ < bytes_.size() && bytes_[offset_] >= '0' &&
               bytes_[offset_] <= '9');
    } else {
      return false;
    }
    if (offset_ < bytes_.size() && bytes_[offset_] == '.') {
      ++offset_;
      const qsizetype fractionBegin = offset_;
      while (offset_ < bytes_.size() && bytes_[offset_] >= '0' &&
             bytes_[offset_] <= '9') {
        nonzeroSignificand =
            nonzeroSignificand || bytes_[offset_] != '0';
        ++offset_;
      }
      if (offset_ == fractionBegin) return false;
    }
    if (offset_ < bytes_.size() &&
        (bytes_[offset_] == 'e' || bytes_[offset_] == 'E')) {
      ++offset_;
      if (offset_ < bytes_.size() &&
          (bytes_[offset_] == '+' || bytes_[offset_] == '-'))
        ++offset_;
      const qsizetype exponentBegin = offset_;
      while (offset_ < bytes_.size() && bytes_[offset_] >= '0' &&
             bytes_[offset_] <= '9')
        ++offset_;
      if (offset_ == exponentBegin) return false;
    }
    if (rejectNegativeZero && negative && !nonzeroSignificand) {
      forbidden_ = true;
      return false;
    }
    return true;
  }

  static bool optionKey(const QByteArray& key) noexcept {
    return key == "linearToleranceMm" || key == "angularToleranceRad" ||
           key == "rankTolerance" || key == "maximumAbsoluteOffsetMm" ||
           key == "maximumTranslationStepMm" ||
           key == "maximumAngularStepRad" || key == "maximumIterations" ||
           key == "maximumNodes" || key == "maximumMates" ||
           key == "maximumWorkUnits";
  }

  bool value(RawContext context, bool rejectNegativeZero, int depth) noexcept {
    if (depth > 64) return false;
    whitespace();
    if (offset_ >= bytes_.size()) return false;
    if (bytes_[offset_] == '{') return object(context, depth + 1);
    if (bytes_[offset_] == '[') return array(context, depth + 1);
    if (bytes_[offset_] == '"') return string(nullptr);
    if (bytes_[offset_] == '-' ||
        (bytes_[offset_] >= '0' && bytes_[offset_] <= '9'))
      return number(rejectNegativeZero);
    for (const auto literal : {QByteArrayView("true"), QByteArrayView("false"),
                               QByteArrayView("null")}) {
      if (bytes_.mid(offset_, literal.size()) == literal) {
        offset_ += literal.size();
        return true;
      }
    }
    return false;
  }

  bool object(RawContext context, int depth) noexcept {
    ++offset_;
    whitespace();
    if (offset_ < bytes_.size() && bytes_[offset_] == '}') {
      ++offset_;
      return true;
    }
    while (offset_ < bytes_.size()) {
      QByteArray key;
      if (!string(&key)) return false;
      whitespace();
      if (offset_ >= bytes_.size() || bytes_[offset_++] != ':') return false;
      RawContext child = RawContext::Normal;
      if (key == "mateSolverOptions") child = RawContext::Options;
      else if (key == "mates") child = RawContext::MateArray;
      const bool target = (context == RawContext::Options && optionKey(key)) ||
                          (context == RawContext::Mate &&
                           (key == "offsetMm" || key == "distanceMm" ||
                            key == "angleRad"));
      if (!value(child, target, depth)) return false;
      whitespace();
      if (offset_ >= bytes_.size()) return false;
      if (bytes_[offset_] == '}') {
        ++offset_;
        return true;
      }
      if (bytes_[offset_++] != ',') return false;
      whitespace();
    }
    return false;
  }

  bool array(RawContext context, int depth) noexcept {
    ++offset_;
    whitespace();
    if (offset_ < bytes_.size() && bytes_[offset_] == ']') {
      ++offset_;
      return true;
    }
    while (offset_ < bytes_.size()) {
      const auto child = context == RawContext::MateArray ? RawContext::Mate
                                                          : RawContext::Normal;
      if (!value(child, false, depth)) return false;
      whitespace();
      if (offset_ >= bytes_.size()) return false;
      if (bytes_[offset_] == ']') {
        ++offset_;
        return true;
      }
      if (bytes_[offset_++] != ',') return false;
      whitespace();
    }
    return false;
  }

  QByteArrayView bytes_;
  qsizetype offset_{};
  bool forbidden_{};
};

const ProductEnvelopeV7AssemblyCandidate* assemblyCandidate(
    const ProductEnvelopeV8Candidate& base, DefinitionId id) {
  const auto found = std::find_if(
      base.assemblies.begin(), base.assemblies.end(),
      [id](const auto& assembly) { return assembly.definitionId == id; });
  return found == base.assemblies.end() ? nullptr : &*found;
}

bool terminalPartPath(const ProductEnvelopeV8Candidate& base,
                      DefinitionId ownerId, const OccurrencePath& path) {
  DefinitionId current = ownerId;
  const auto& entries = path.entries();
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto* owner = assemblyCandidate(base, current);
    if (!owner) return false;
    const auto occurrence = std::find_if(
        owner->occurrences.begin(), owner->occurrences.end(),
        [id = entries[index]](const auto& value) {
          return value.occurrenceId == id;
        });
    if (occurrence == owner->occurrences.end()) return false;
    const bool part = std::any_of(
        base.parts.begin(), base.parts.end(),
        [id = occurrence->targetDefinitionId](const auto& value) {
          return value.definitionId == id;
        });
    if (index + 1 == entries.size()) return part;
    if (part) return false;
    current = occurrence->targetDefinitionId;
  }
  return false;
}

bool decodeOptions(const QJsonValue& value, MateSolverOptions* options) {
  if (!value.isObject()) return false;
  const auto object = value.toObject();
  if (!exactFields(object,
                   {"linearToleranceMm", "angularToleranceRad",
                    "rankTolerance", "maximumAbsoluteOffsetMm",
                    "maximumTranslationStepMm", "maximumAngularStepRad",
                    "maximumIterations", "maximumNodes", "maximumMates",
                    "maximumWorkUnits"}))
    return false;
  constexpr double pi = 3.141592653589793238462643383279502884;
  std::uint64_t integerValue = 0;
  if (!finiteNumber(object.value("linearToleranceMm"), 0.0, 1.0, true,
                    &options->linearToleranceMm) ||
      !finiteNumber(object.value("angularToleranceRad"),
                    kMinimumMateAngularToleranceRad, pi, false,
                    &options->angularToleranceRad) ||
      !finiteNumber(object.value("rankTolerance"), 0.0, 1.0, true,
                    &options->rankTolerance) ||
      !finiteNumber(object.value("maximumAbsoluteOffsetMm"), 0.0, 1.0e9,
                    false, &options->maximumAbsoluteOffsetMm) ||
      !finiteNumber(object.value("maximumTranslationStepMm"), 0.0, 1.0e6,
                    true, &options->maximumTranslationStepMm) ||
      !finiteNumber(object.value("maximumAngularStepRad"), 0.0, pi, true,
                    &options->maximumAngularStepRad) ||
      !integer(object.value("maximumIterations"), 1, 64, &integerValue))
    return false;
  options->maximumIterations = static_cast<std::size_t>(integerValue);
  if (!integer(object.value("maximumNodes"), 1, 256, &integerValue))
    return false;
  options->maximumNodes = static_cast<std::size_t>(integerValue);
  if (!integer(object.value("maximumMates"), 1, 128, &integerValue))
    return false;
  options->maximumMates = static_cast<std::size_t>(integerValue);
  if (!integer(object.value("maximumWorkUnits"), 1, 1'000'000'000ULL,
               &integerValue))
    return false;
  options->maximumWorkUnits = integerValue;
  return options->isValid();
}

enum class EndpointStatus { Valid, Invalid, Unsupported };
EndpointStatus decodeEndpoint(
    const QJsonValue& value, const ProductEnvelopeV8Candidate& base,
    DefinitionId owner, std::optional<MateEndpoint>* output,
    std::size_t* pathEntries,
    std::size_t* provenanceRecords) {
  if (!value.isObject() ||
      !exactFields(value.toObject(), {"occurrencePath", "face"}))
    return EndpointStatus::Invalid;
  const auto object = value.toObject();
  if (!object.value("occurrencePath").isArray()) return EndpointStatus::Invalid;
  const auto authoredPath = object.value("occurrencePath").toArray();
  if (authoredPath.isEmpty() || authoredPath.size() > 32 ||
      static_cast<std::size_t>(authoredPath.size()) >
          kProductEnvelopeV9MaximumPathEntries - *pathEntries)
    return EndpointStatus::Invalid;
  std::vector<OccurrenceId> entries;
  entries.reserve(static_cast<std::size_t>(authoredPath.size()));
  for (const auto& item : authoredPath) {
    std::uint64_t id = 0;
    if (!integer(item, 1, ProjectFile::kMaximumPersistedId, &id))
      return EndpointStatus::Invalid;
    entries.emplace_back(id);
  }
  auto path = OccurrencePath::create(std::move(entries));
  if (!path || !terminalPartPath(base, owner, *path.path))
    return EndpointStatus::Invalid;
  const auto decoded = decodeProjectFormatV4TopologyReference(
      object.value("face"), TopologyKind::Face);
  if (decoded.status == ProjectFormatV4ReferenceDecodeStatus::Unsupported)
    return EndpointStatus::Unsupported;
  if (!decoded.succeeded() || !decoded.inspection ||
      decoded.inspection->variant != ProjectFormatV4ReferenceVariant::V2 ||
      !std::holds_alternative<FaceReference>(*decoded.reference))
    return EndpointStatus::Invalid;
  FaceReference face = std::get<FaceReference>(*decoded.reference);
  const auto* v2 = face.v2();
  if (!v2 || v2->provenance.origins.size() >
                 kProductEnvelopeV9MaximumOriginsPerReference ||
      decoded.provenanceRecordCount >
          kProductEnvelopeV9MaximumProvenanceRecords - *provenanceRecords)
    return EndpointStatus::Invalid;
  *pathEntries += static_cast<std::size_t>(authoredPath.size());
  *provenanceRecords += decoded.provenanceRecordCount;
  output->emplace(MateEndpoint{std::move(*path.path), std::move(face)});
  return EndpointStatus::Valid;
}

EndpointStatus decodeAxisEndpoint(
    const QJsonValue& value, const ProductEnvelopeV8Candidate& base,
    DefinitionId owner, std::optional<MateAxisEndpoint>* output,
    std::size_t* pathEntries, std::size_t* provenanceRecords) {
  if (!value.isObject() ||
      !exactFields(value.toObject(), {"occurrencePath", "axis"}))
    return EndpointStatus::Invalid;
  const auto object = value.toObject();
  if (!object.value("occurrencePath").isArray() ||
      !object.value("axis").isObject())
    return EndpointStatus::Invalid;
  const auto authoredPath = object.value("occurrencePath").toArray();
  if (authoredPath.isEmpty() || authoredPath.size() > 32 ||
      static_cast<std::size_t>(authoredPath.size()) >
          kProductEnvelopeV9MaximumPathEntries - *pathEntries)
    return EndpointStatus::Invalid;
  std::vector<OccurrenceId> entries;
  entries.reserve(static_cast<std::size_t>(authoredPath.size()));
  for (const auto& item : authoredPath) {
    std::uint64_t id = 0;
    if (!integer(item, 1, ProjectFile::kMaximumPersistedId, &id))
      return EndpointStatus::Invalid;
    entries.emplace_back(id);
  }
  auto path = OccurrencePath::create(std::move(entries));
  if (!path || !terminalPartPath(base, owner, *path.path))
    return EndpointStatus::Invalid;

  const auto axis = object.value("axis").toObject();
  if (!axis.value("referenceKind").isString())
    return EndpointStatus::Invalid;
  const QString referenceKind = axis.value("referenceKind").toString();
  MateAxisReference reference;
  if (referenceKind == QStringLiteral("local-datum-axis-v1")) {
    if (!exactFields(axis, {"referenceKind", "axis"}) ||
        !axis.value("axis").isString())
      return EndpointStatus::Invalid;
    const QString authoredAxis = axis.value("axis").toString();
    if (authoredAxis == QStringLiteral("x"))
      reference = LocalDatumAxis::X;
    else if (authoredAxis == QStringLiteral("y"))
      reference = LocalDatumAxis::Y;
    else if (authoredAxis == QStringLiteral("z"))
      reference = LocalDatumAxis::Z;
    else
      return EndpointStatus::Invalid;
  } else if (referenceKind ==
             QStringLiteral("cylindrical-face-axis-v1")) {
    if (!exactFields(axis, {"referenceKind", "face"}))
      return EndpointStatus::Invalid;
    const auto decoded = decodeProjectFormatV4TopologyReference(
        axis.value("face"), TopologyKind::Face);
    if (decoded.status == ProjectFormatV4ReferenceDecodeStatus::Unsupported)
      return EndpointStatus::Unsupported;
    if (!decoded.succeeded() || !decoded.inspection ||
        decoded.inspection->variant != ProjectFormatV4ReferenceVariant::V2 ||
        !std::holds_alternative<FaceReference>(*decoded.reference))
      return EndpointStatus::Invalid;
    FaceReference face = std::get<FaceReference>(*decoded.reference);
    const auto* v2 = face.v2();
    if (!v2 || v2->provenance.origins.size() >
                   kProductEnvelopeV9MaximumOriginsPerReference ||
        decoded.provenanceRecordCount >
            kProductEnvelopeV9MaximumProvenanceRecords - *provenanceRecords)
      return EndpointStatus::Invalid;
    *provenanceRecords += decoded.provenanceRecordCount;
    reference = CylindricalFaceAxis{std::move(face)};
  } else {
    return EndpointStatus::Unsupported;
  }
  *pathEntries += static_cast<std::size_t>(authoredPath.size());
  output->emplace(
      MateAxisEndpoint{std::move(*path.path), std::move(reference)});
  return EndpointStatus::Valid;
}

EndpointStatus decodePointEndpoint(
    const QJsonValue& value, const ProductEnvelopeV8Candidate& base,
    DefinitionId owner, std::optional<MatePointEndpoint>* output,
    std::size_t* pathEntries, std::size_t* provenanceRecords) {
  if (!value.isObject() ||
      !exactFields(value.toObject(), {"occurrencePath", "point"}))
    return EndpointStatus::Invalid;
  const auto object = value.toObject();
  if (!object.value("occurrencePath").isArray() ||
      !object.value("point").isObject())
    return EndpointStatus::Invalid;
  const auto authoredPath = object.value("occurrencePath").toArray();
  if (authoredPath.isEmpty() || authoredPath.size() > 32 ||
      static_cast<std::size_t>(authoredPath.size()) >
          kProductEnvelopeV9MaximumPathEntries - *pathEntries)
    return EndpointStatus::Invalid;
  std::vector<OccurrenceId> entries;
  entries.reserve(static_cast<std::size_t>(authoredPath.size()));
  for (const auto& item : authoredPath) {
    std::uint64_t id = 0;
    if (!integer(item, 1, ProjectFile::kMaximumPersistedId, &id))
      return EndpointStatus::Invalid;
    entries.emplace_back(id);
  }
  auto path = OccurrencePath::create(std::move(entries));
  if (!path || !terminalPartPath(base, owner, *path.path))
    return EndpointStatus::Invalid;

  const auto point = object.value("point").toObject();
  if (!point.value("referenceKind").isString())
    return EndpointStatus::Invalid;
  const QString referenceKind = point.value("referenceKind").toString();
  MatePointReference reference;
  if (referenceKind == QStringLiteral("local-datum-origin-v1")) {
    if (!exactFields(point, {"referenceKind"}))
      return EndpointStatus::Invalid;
    reference = LocalDatumPoint::Origin;
  } else if (referenceKind ==
             QStringLiteral("circular-edge-center-v1")) {
    if (!exactFields(point, {"referenceKind", "edge"}))
      return EndpointStatus::Invalid;
    const auto decoded = decodeProjectFormatV4TopologyReference(
        point.value("edge"), TopologyKind::Edge);
    if (decoded.status == ProjectFormatV4ReferenceDecodeStatus::Unsupported)
      return EndpointStatus::Unsupported;
    if (!decoded.succeeded() || !decoded.inspection ||
        decoded.inspection->variant != ProjectFormatV4ReferenceVariant::V2 ||
        !std::holds_alternative<EdgeReference>(*decoded.reference))
      return EndpointStatus::Invalid;
    EdgeReference edge = std::get<EdgeReference>(*decoded.reference);
    const auto* v2 = edge.v2();
    if (!v2 || v2->provenance.origins.size() >
                   kProductEnvelopeV9MaximumOriginsPerReference ||
        decoded.provenanceRecordCount >
            kProductEnvelopeV9MaximumProvenanceRecords - *provenanceRecords)
      return EndpointStatus::Invalid;
    *provenanceRecords += decoded.provenanceRecordCount;
    reference = CircularEdgeCenter{std::move(edge)};
  } else {
    return EndpointStatus::Unsupported;
  }
  *pathEntries += static_cast<std::size_t>(authoredPath.size());
  output->emplace(
      MatePointEndpoint{std::move(*path.path), std::move(reference)});
  return EndpointStatus::Valid;
}

}  // namespace

ProductEnvelopeV9ScanResult scanProductEnvelopeV9(QByteArrayView bytes) {
  try {
    if (bytes.isEmpty() || bytes.size() > ProjectFile::kMaximumFileBytes)
      return invalid(QStringLiteral("v9 input size is invalid"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), bytes.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
      return invalid(QStringLiteral("v9 root is not valid JSON object"));
    const auto root = document.object();
    if (!root.value("format").isString() || !root.value("version").isDouble())
      return invalid(QStringLiteral("v9 dispatch fields are invalid"));
    const auto version = root.value("version").toDouble();
    if (root.value("format").toString() != QStringLiteral("solidar-project") ||
        version < 9.0)
      return fail(ProductEnvelopeV9ScanStatus::NotApplicable,
                  QStringLiteral("not a v9 product envelope"));
    if (version > 9.0)
      return unsupported(QStringLiteral("newer product envelope"));
    if (std::trunc(version) != version ||
        !exactFields(root,
                     {"format", "version", "capabilities", "projectId",
                      "rootDefinitionId", "metadata", "definitions"}))
      return invalid(QStringLiteral("v9 root schema is not exact"));
    const auto rawNumbers = RawV9NumberScanner(bytes).scan();
    if (rawNumbers == RawTokenStatus::ForbiddenNegativeZero)
      return invalid(QStringLiteral(
          "v9 options and authored mate scalars forbid negative zero"));
    if (rawNumbers == RawTokenStatus::Malformed)
      return invalid(QStringLiteral("v9 raw JSON tokenization failed"));

    if (!root.value("capabilities").isArray())
      return invalid(QStringLiteral("v9 capabilities must be an array"));
    QStringList capabilities;
    std::set<QString> actualSet;
    for (const auto& value : root.value("capabilities").toArray()) {
      if (!value.isString())
        return invalid(QStringLiteral("v9 capability must be a string"));
      capabilities.push_back(value.toString());
      if (!actualSet.insert(value.toString()).second)
        return invalid(QStringLiteral("v9 capability is duplicated"));
    }
    const std::set<QString> expectedSet{
        QStringLiteral("assembly-axis-concentric-mates-v1"),
        QStringLiteral("assembly-planar-angle-mates-v1"),
        QStringLiteral("assembly-planar-angular-mates-v1"),
        QStringLiteral("assembly-planar-mates-v1"),
        QStringLiteral("assembly-point-distance-mates-v1"),
        QStringLiteral("linked-parts-v1"),
        QStringLiteral("nested-assemblies-v1"),
        QStringLiteral("product-envelope-v1")};
    if (std::any_of(actualSet.begin(), actualSet.end(),
                    [&expectedSet](const QString& capability) {
                      return !expectedSet.contains(capability);
                    }))
      return unsupported(QStringLiteral("unsupported v9 capability"));
    const bool angularCapability = capabilities.contains(
        QStringLiteral("assembly-planar-angular-mates-v1"));
    const bool angleCapability = capabilities.contains(
        QStringLiteral("assembly-planar-angle-mates-v1"));
    const bool concentricCapability = capabilities.contains(
        QStringLiteral("assembly-axis-concentric-mates-v1"));
    const bool distanceCapability = capabilities.contains(
        QStringLiteral("assembly-point-distance-mates-v1"));
    QStringList canonicalCapabilities;
    if (concentricCapability)
      canonicalCapabilities.push_back(
          QStringLiteral("assembly-axis-concentric-mates-v1"));
    if (angleCapability)
      canonicalCapabilities.push_back(
          QStringLiteral("assembly-planar-angle-mates-v1"));
    if (angularCapability)
      canonicalCapabilities.push_back(
          QStringLiteral("assembly-planar-angular-mates-v1"));
    canonicalCapabilities.push_back(
        QStringLiteral("assembly-planar-mates-v1"));
    if (distanceCapability)
      canonicalCapabilities.push_back(
          QStringLiteral("assembly-point-distance-mates-v1"));
    canonicalCapabilities.push_back(QStringLiteral("linked-parts-v1"));
    canonicalCapabilities.push_back(QStringLiteral("nested-assemblies-v1"));
    canonicalCapabilities.push_back(QStringLiteral("product-envelope-v1"));
    if (capabilities != canonicalCapabilities)
      return invalid(QStringLiteral("v9 capabilities are not canonical"));
    if (!root.value("definitions").isArray())
      return invalid(QStringLiteral("v9 definitions must be an array"));

    QJsonObject v8Root = root;
    v8Root.insert("version", 8);
    v8Root.insert("capabilities",
                  QJsonArray{QStringLiteral("linked-parts-v1"),
                             QStringLiteral("nested-assemblies-v1"),
                             QStringLiteral("product-envelope-v1")});
    QJsonArray strippedDefinitions;
    std::map<DefinitionId, QJsonObject> authoredAssembly;
    for (const auto& value : root.value("definitions").toArray()) {
      if (!value.isObject())
        return invalid(QStringLiteral("v9 definition is not an object"));
      const auto definition = value.toObject();
      if (definition.value("definitionKind").toString() !=
          QStringLiteral("assembly")) {
        strippedDefinitions.push_back(definition);
        continue;
      }
      if (!exactFields(definition,
                       {"definitionId", "definitionKind", "occurrences",
                        "mateSolverOptions", "mates"}))
        return invalid(QStringLiteral("v9 assembly schema is not exact"));
      std::uint64_t id = 0;
      if (!integer(definition.value("definitionId"), 1,
                   ProjectFile::kMaximumPersistedId, &id) ||
          !authoredAssembly.emplace(DefinitionId{id}, definition).second)
        return invalid(QStringLiteral("v9 assembly ID is invalid"));
      strippedDefinitions.push_back(QJsonObject{
          {"definitionId", definition.value("definitionId")},
          {"definitionKind", QStringLiteral("assembly")},
          {"occurrences", definition.value("occurrences")}});
    }
    v8Root.insert("definitions", strippedDefinitions);
    const QByteArray v8Bytes =
        QJsonDocument(v8Root).toJson(QJsonDocument::Compact);
    auto baseScan = scanProductEnvelopeV8(QByteArrayView(v8Bytes));
    if (baseScan.status == ProductEnvelopeV8ScanStatus::Unsupported)
      return unsupported(baseScan.error);
    if (!baseScan.hasCandidate())
      return invalid(QStringLiteral("v9 inherited v8 schema rejected: %1")
                         .arg(baseScan.error));

    ProductEnvelopeV9Candidate candidate;
    candidate.authoredRoot = root;
    candidate.base = std::move(*baseScan.candidate);
    candidate.capabilities = capabilities;
    bool hasAngularMate = false;
    bool hasAngleMate = false;
    bool hasConcentricMate = false;
    bool hasDistanceMate = false;
    for (const auto& baseAssembly : candidate.base.assemblies) {
      const auto found = authoredAssembly.find(baseAssembly.definitionId);
      if (found == authoredAssembly.end())
        return invalid(QStringLiteral("v9 assembly fields are missing"));
      ProductEnvelopeV9AssemblyCandidate assembly;
      assembly.definitionId = baseAssembly.definitionId;
      if (!decodeOptions(found->second.value("mateSolverOptions"),
                         &assembly.options) ||
          !found->second.value("mates").isArray())
        return invalid(QStringLiteral("v9 mate options/list is invalid"));
      const auto mates = found->second.value("mates").toArray();
      if (mates.size() >
          static_cast<qsizetype>(kProductEnvelopeV9MaximumMatesPerAssembly))
        return invalid(QStringLiteral("v9 per-assembly mate budget exceeded"));
      if (static_cast<std::size_t>(mates.size()) >
          kProductEnvelopeV9MaximumMates - candidate.mateCount)
        return invalid(QStringLiteral("v9 aggregate mate budget exceeded"));
      std::uint64_t previousMate = 0;
      assembly.mates.reserve(static_cast<std::size_t>(mates.size()));
      for (const auto& mateValue : mates) {
        if (!mateValue.isObject())
          return invalid(QStringLiteral("v9 mate schema is not exact"));
        const auto mate = mateValue.toObject();
        if (!mate.value("type").isString())
          return invalid(QStringLiteral("v9 mate type is invalid"));
        const QString type = mate.value("type").toString();
        if (type != QStringLiteral("planar-coincident-v1") &&
            type != QStringLiteral("planar-parallel-v1") &&
            type != QStringLiteral("planar-perpendicular-v1") &&
            type != QStringLiteral("planar-angle-v1") &&
            type != QStringLiteral("axis-concentric-v1") &&
            type != QStringLiteral("point-distance-v1"))
          return unsupported(QStringLiteral("unsupported v9 mate type"));
        const bool perpendicular =
            type == QStringLiteral("planar-perpendicular-v1");
        const bool concentric =
            type == QStringLiteral("axis-concentric-v1");
        const bool coincident =
            type == QStringLiteral("planar-coincident-v1");
        const bool distance =
            type == QStringLiteral("point-distance-v1");
        const bool angle =
            type == QStringLiteral("planar-angle-v1");
        if ((perpendicular || concentric)
                ? !exactFields(mate,
                               {"mateId", "type", "first", "second"})
                : (distance
                       ? !exactFields(mate,
                                      {"mateId", "type", "first", "second",
                                       "distanceMm"})
                : (angle
                       ? !exactFields(mate,
                                      {"mateId", "type", "first", "second",
                                       "orientation", "angleRad"})
                : (coincident
                       ? !exactFields(mate,
                                      {"mateId", "type", "first", "second",
                                       "orientation", "offsetMm"})
                       : !exactFields(mate,
                                      {"mateId", "type", "first", "second",
                                       "orientation"})))))
          return invalid(QStringLiteral("v9 mate schema is not exact"));
        std::uint64_t mateId = 0;
        if (!integer(mate.value("mateId"), 1,
                     ProjectFile::kMaximumPersistedId, &mateId) ||
            mateId <= previousMate)
          return invalid(QStringLiteral("v9 mate IDs are not sorted/unique"));
        previousMate = mateId;
        PlanarMateOrientation orientation = PlanarMateOrientation::Aligned;
        if (!perpendicular && !concentric && !distance) {
          if (!mate.value("orientation").isString())
            return invalid(QStringLiteral("v9 mate orientation is invalid"));
          if (mate.value("orientation").toString() ==
              QStringLiteral("aligned"))
            orientation = PlanarMateOrientation::Aligned;
          else if (mate.value("orientation").toString() ==
                   QStringLiteral("opposed"))
            orientation = PlanarMateOrientation::Opposed;
          else
            return invalid(QStringLiteral("v9 mate orientation is unknown"));
        }
        double offset = 0.0;
        if (coincident &&
            !finiteNumber(mate.value("offsetMm"),
                          -assembly.options.maximumAbsoluteOffsetMm,
                          assembly.options.maximumAbsoluteOffsetMm, false,
                          &offset))
          return invalid(QStringLiteral("v9 mate offset is invalid"));
        double distanceMm = 0.0;
        if (distance &&
            !finiteNumber(mate.value("distanceMm"), 0.0,
                          assembly.options.maximumAbsoluteOffsetMm, true,
                          &distanceMm))
          return invalid(QStringLiteral("v9 mate distance is invalid"));
        double angleRad = 0.0;
        if (angle &&
            !finiteNumber(mate.value("angleRad"), 0.0, kHalfPi, true,
                          &angleRad))
          return invalid(QStringLiteral("v9 mate angle is invalid"));
        if (candidate.endpointCount > kProductEnvelopeV9MaximumEndpoints - 2)
          return invalid(QStringLiteral("v9 endpoint budget exceeded"));
        std::optional<MateEndpoint> first;
        std::optional<MateEndpoint> second;
        std::optional<MateAxisEndpoint> firstAxis;
        std::optional<MateAxisEndpoint> secondAxis;
        std::optional<MatePointEndpoint> firstPoint;
        std::optional<MatePointEndpoint> secondPoint;
        const auto firstStatus =
            concentric
                ? decodeAxisEndpoint(
                      mate.value("first"), candidate.base,
                      assembly.definitionId, &firstAxis,
                      &candidate.pathEntryCount,
                      &candidate.provenanceRecordCount)
                : distance
                ? decodePointEndpoint(
                      mate.value("first"), candidate.base,
                      assembly.definitionId, &firstPoint,
                      &candidate.pathEntryCount,
                      &candidate.provenanceRecordCount)
                : decodeEndpoint(
                      mate.value("first"), candidate.base,
                      assembly.definitionId, &first,
                      &candidate.pathEntryCount,
                      &candidate.provenanceRecordCount);
        const auto secondStatus =
            concentric
                ? decodeAxisEndpoint(
                      mate.value("second"), candidate.base,
                      assembly.definitionId, &secondAxis,
                      &candidate.pathEntryCount,
                      &candidate.provenanceRecordCount)
                : distance
                ? decodePointEndpoint(
                      mate.value("second"), candidate.base,
                      assembly.definitionId, &secondPoint,
                      &candidate.pathEntryCount,
                      &candidate.provenanceRecordCount)
                : decodeEndpoint(
                      mate.value("second"), candidate.base,
                      assembly.definitionId, &second,
                      &candidate.pathEntryCount,
                      &candidate.provenanceRecordCount);
        if (firstStatus == EndpointStatus::Unsupported ||
            secondStatus == EndpointStatus::Unsupported)
          return unsupported(QStringLiteral("unsupported v9 face reference"));
        if (firstStatus != EndpointStatus::Valid ||
            secondStatus != EndpointStatus::Valid)
          return invalid(QStringLiteral("v9 mate endpoint is invalid"));
        AssemblyMate relation = concentric
                                    ? AssemblyMate{AxisConcentricMate{
                                          MateId{mateId},
                                          std::move(*firstAxis),
                                          std::move(*secondAxis)}}
                                    : distance
                                    ? AssemblyMate{PointDistanceMate{
                                          MateId{mateId},
                                          std::move(*firstPoint),
                                          std::move(*secondPoint), distanceMm}}
                                    : perpendicular
                                    ? AssemblyMate{PlanarPerpendicularMate{
                                          MateId{mateId}, std::move(*first),
                                          std::move(*second)}}
                                    : angle
                                    ? AssemblyMate{PlanarAngleMate{
                                          MateId{mateId}, std::move(*first),
                                          std::move(*second), orientation,
                                          angleRad}}
                                    : (coincident
                                           ? AssemblyMate{
                                                 PlanarCoincidentMate{
                                                     MateId{mateId},
                                                     std::move(*first),
                                                     std::move(*second),
                                                     orientation, offset}}
                                           : AssemblyMate{PlanarParallelMate{
                                                 MateId{mateId},
                                                 std::move(*first),
                                                 std::move(*second),
                                                 orientation}});
        if (!isValidAssemblyMate(
                relation, assembly.options.maximumAbsoluteOffsetMm,
                assembly.options.angularToleranceRad))
          return invalid(QStringLiteral("v9 mate is semantically invalid"));
        hasAngularMate =
            hasAngularMate ||
            (!coincident && !concentric && !distance && !angle);
        hasAngleMate = hasAngleMate || angle;
        hasConcentricMate = hasConcentricMate || concentric;
        hasDistanceMate = hasDistanceMate || distance;
        candidate.endpointCount += 2;
        assembly.mates.push_back(std::move(relation));
      }
      candidate.mateCount += static_cast<std::size_t>(mates.size());
      candidate.assemblies.push_back(std::move(assembly));
    }
    if (hasAngularMate != angularCapability)
      return invalid(QStringLiteral(
          "v9 angular mate capability does not match authored mates"));
    if (hasAngleMate != angleCapability)
      return invalid(QStringLiteral(
          "v9 angle mate capability does not match authored mates"));
    if (hasConcentricMate != concentricCapability)
      return invalid(QStringLiteral(
          "v9 concentric mate capability does not match authored mates"));
    if (hasDistanceMate != distanceCapability)
      return invalid(QStringLiteral(
          "v9 point distance capability does not match authored mates"));
    ProductEnvelopeV9ScanResult result;
    result.status = ProductEnvelopeV9ScanStatus::Candidate;
    result.candidate = std::move(candidate);
    return result;
  } catch (const std::exception& exception) {
    return invalid(QStringLiteral("v9 scan failure: %1")
                       .arg(QString::fromUtf8(exception.what())));
  } catch (...) {
    return invalid(QStringLiteral("unknown v9 scan failure"));
  }
}

}  // namespace solidar::project
