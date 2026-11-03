#include "project/internal/StrictOccurrenceCodec.h"

#include <QByteArray>
#include <QJsonArray>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "model/AssemblyGraphAnalysis.h"
#include "project/ProjectFile.h"

namespace solidar::project::detail {
namespace {

bool jsonWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void skipWhitespace(QByteArrayView bytes, qsizetype* offset) noexcept {
  while (*offset < bytes.size() && jsonWhitespace(bytes[*offset])) ++*offset;
}

bool scanJsonString(QByteArrayView bytes, qsizetype* offset,
                    QByteArray* smallAscii) noexcept {
  if (!offset || *offset >= bytes.size() || bytes[*offset] != '"') return false;
  if (smallAscii) smallAscii->clear();
  ++*offset;
  while (*offset < bytes.size()) {
    unsigned char value = static_cast<unsigned char>(bytes[*offset]);
    ++*offset;
    if (value == '"') return true;
    if (value < 0x20) return false;
    if (value != '\\') {
      if (smallAscii && smallAscii->size() <= 32)
        smallAscii->append(value < 0x80 ? static_cast<char>(value) : '#');
      continue;
    }
    if (*offset >= bytes.size()) return false;
    const char escaped = bytes[(*offset)++];
    if (escaped == 'u') {
      if (bytes.size() - *offset < 4) return false;
      unsigned code = 0;
      for (int digit = 0; digit < 4; ++digit) {
        const char hex = bytes[(*offset)++];
        code <<= 4;
        if (hex >= '0' && hex <= '9')
          code += hex - '0';
        else if (hex >= 'a' && hex <= 'f')
          code += hex - 'a' + 10;
        else if (hex >= 'A' && hex <= 'F')
          code += hex - 'A' + 10;
        else
          return false;
      }
      if (smallAscii && smallAscii->size() <= 32)
        smallAscii->append(code < 0x80 ? static_cast<char>(code) : '#');
    } else if (smallAscii && smallAscii->size() <= 32) {
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          smallAscii->append(escaped);
          break;
        case 'b': smallAscii->append('\b'); break;
        case 'f': smallAscii->append('\f'); break;
        case 'n': smallAscii->append('\n'); break;
        case 'r': smallAscii->append('\r'); break;
        case 't': smallAscii->append('\t'); break;
        default: return false;
      }
    } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
               escaped != 'b' && escaped != 'f' && escaped != 'n' &&
               escaped != 'r' && escaped != 't') {
      return false;
    }
  }
  return false;
}

bool scanJsonNumber(QByteArrayView bytes, qsizetype* offset,
                    bool* negativeZero) noexcept {
  const qsizetype begin = *offset;
  bool negative = false;
  if (*offset < bytes.size() && bytes[*offset] == '-') {
    negative = true;
    ++*offset;
  }
  bool sawDigit = false;
  bool nonZeroDigit = false;
  while (*offset < bytes.size() && bytes[*offset] >= '0' &&
         bytes[*offset] <= '9') {
    sawDigit = true;
    nonZeroDigit = nonZeroDigit || bytes[*offset] != '0';
    ++*offset;
  }
  if (*offset < bytes.size() && bytes[*offset] == '.') {
    ++*offset;
    while (*offset < bytes.size() && bytes[*offset] >= '0' &&
           bytes[*offset] <= '9') {
      sawDigit = true;
      nonZeroDigit = nonZeroDigit || bytes[*offset] != '0';
      ++*offset;
    }
  }
  if (*offset < bytes.size() &&
      (bytes[*offset] == 'e' || bytes[*offset] == 'E')) {
    ++*offset;
    if (*offset < bytes.size() &&
        (bytes[*offset] == '+' || bytes[*offset] == '-'))
      ++*offset;
    bool exponentDigit = false;
    while (*offset < bytes.size() && bytes[*offset] >= '0' &&
           bytes[*offset] <= '9') {
      exponentDigit = true;
      ++*offset;
    }
    if (!exponentDigit) return false;
  }
  if (!sawDigit || *offset == begin) return false;
  *negativeZero = negative && !nonZeroDigit;
  return true;
}

bool listed(std::initializer_list<const char*> fields, const QString& key) {
  return std::any_of(fields.begin(), fields.end(), [&](const char* field) {
    return key == QLatin1String(field);
  });
}

bool exactFields(const QJsonObject& object,
                 std::initializer_list<const char*> fields,
                 const QString& path, QString* error) {
  for (const char* field : fields) {
    if (!object.contains(QLatin1String(field))) {
      *error = QStringLiteral("%1 is missing %2")
                   .arg(path, QString::fromLatin1(field));
      return false;
    }
  }
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (!listed(fields, iterator.key())) {
      *error = QStringLiteral("%1 contains unexpected field %2")
                   .arg(path, iterator.key());
      return false;
    }
  }
  return object.size() == static_cast<qsizetype>(fields.size());
}

bool validUtf16(const QString& value) {
  for (qsizetype i = 0; i < value.size(); ++i) {
    if (value[i].isHighSurrogate()) {
      if (++i >= value.size() || !value[i].isLowSurrogate()) return false;
    } else if (value[i].isLowSurrogate()) {
      return false;
    }
  }
  return true;
}

bool boundedString(const QJsonValue& value, bool allowEmpty, QString* output,
                   const QString& path, QString* error) {
  if (!value.isString()) {
    *error = path + QStringLiteral(" must be a string");
    return false;
  }
  const QString text = value.toString();
  if ((!allowEmpty && text.isEmpty()) || !validUtf16(text) ||
      text.size() > ProjectFile::kMaximumStringCharacters) {
    *error = path + QStringLiteral(" is invalid");
    return false;
  }
  *output = text;
  return true;
}

bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum,
             qint64* output, const QString& path, QString* error) {
  if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
      std::trunc(value.toDouble()) != value.toDouble() ||
      value.toDouble() < static_cast<double>(minimum) ||
      value.toDouble() > static_cast<double>(maximum)) {
    *error = path + QStringLiteral(" must be an exact bounded integer");
    return false;
  }
  *output = static_cast<qint64>(value.toDouble());
  return true;
}

bool persistedId(const QJsonValue& value, qint64* output,
                 const QString& path, QString* error) {
  return integer(value, 1, ProjectFile::kMaximumPersistedId, output, path,
                 error);
}

bool numberArray(const QJsonValue& value, qsizetype count,
                 std::vector<double>* output, const QString& path,
                 QString* error) {
  if (!value.isArray() || value.toArray().size() != count) {
    *error = path + QStringLiteral(" has the wrong length");
    return false;
  }
  output->clear();
  for (const auto& item : value.toArray()) {
    if (!item.isDouble() || !std::isfinite(item.toDouble())) {
      *error = path + QStringLiteral(" must contain finite numbers");
      return false;
    }
    output->push_back(item.toDouble());
  }
  return true;
}

bool exactQuaternion(const std::array<double, 4>& authored,
                     const std::array<double, 4>& canonical) noexcept {
  for (std::size_t index = 0; index < authored.size(); ++index) {
    if (std::bit_cast<std::uint64_t>(authored[index]) !=
        std::bit_cast<std::uint64_t>(canonical[index]))
      return false;
  }
  return true;
}

bool persisted(std::uint64_t value) {
  return value > 0 &&
         value <= static_cast<std::uint64_t>(ProjectFile::kMaximumPersistedId);
}

QJsonArray numbers(const std::array<double, 3>& value) {
  return {value[0], value[1], value[2]};
}

QJsonArray numbers(const std::array<double, 4>& value) {
  return {value[0], value[1], value[2], value[3]};
}

StrictOccurrenceDecodeResult invalid(QString error) {
  StrictOccurrenceDecodeResult result;
  result.error = std::move(error);
  return result;
}

}  // namespace

namespace {

bool validateTransformRawTokens(QByteArrayView bytes,
                                bool rejectTranslationNegativeZero,
                                QString* error) noexcept {
  qsizetype offset = 0;
  QByteArray key;
  while (offset < bytes.size()) {
    if (bytes[offset] != '"') {
      ++offset;
      continue;
    }
    if (!scanJsonString(bytes, &offset, &key)) return true;
    qsizetype valueOffset = offset;
    skipWhitespace(bytes, &valueOffset);
    const bool quaternion = key == QByteArrayLiteral("quaternionWxyz");
    const bool translation =
        rejectTranslationNegativeZero &&
        key == QByteArrayLiteral("translationMm");
    if ((!quaternion && !translation) || valueOffset >= bytes.size() ||
        bytes[valueOffset] != ':')
      continue;
    ++valueOffset;
    skipWhitespace(bytes, &valueOffset);
    if (valueOffset >= bytes.size() || bytes[valueOffset] != '[') continue;
    ++valueOffset;
    const int componentCount = quaternion ? 4 : 3;
    for (int component = 0; component < componentCount; ++component) {
      skipWhitespace(bytes, &valueOffset);
      bool negativeZero = false;
      if (!scanJsonNumber(bytes, &valueOffset, &negativeZero)) break;
      if (negativeZero) {
        if (error)
          *error = QStringLiteral("%1[%2] uses forbidden negative zero")
                       .arg(QString::fromLatin1(key))
                       .arg(component);
        return false;
      }
      skipWhitespace(bytes, &valueOffset);
      if (component + 1 < componentCount) {
        if (valueOffset >= bytes.size() || bytes[valueOffset] != ',') break;
        ++valueOffset;
      }
    }
  }
  return true;
}

}  // namespace

bool validateStrictOccurrenceRawTokens(QByteArrayView bytes,
                                       QString* error) noexcept {
  return validateTransformRawTokens(bytes, false, error);
}

bool validateProductEnvelopeV12TransformRawTokens(
    QByteArrayView bytes, QString* error) noexcept {
  return validateTransformRawTokens(bytes, true, error);
}

StrictOccurrenceDecodeResult decodeStrictOccurrences(
    const QJsonValue& value, const QString& definitionPath,
    std::size_t currentAuthoredOccurrences,
    std::size_t maximumAuthoredOccurrences) {
  if (!value.isArray())
    return invalid(definitionPath +
                   QStringLiteral(".occurrences must be an array"));
  const QJsonArray authored = value.toArray();
  if (!fitsAssemblyGraphBudget(currentAuthoredOccurrences,
                               static_cast<std::size_t>(authored.size()),
                               maximumAuthoredOccurrences))
    return invalid(QStringLiteral(
        "aggregate authored occurrence budget exceeded"));

  StrictOccurrenceDecodeResult result;
  result.aggregateAuthoredOccurrences =
      currentAuthoredOccurrences + static_cast<std::size_t>(authored.size());
  result.occurrences.reserve(static_cast<std::size_t>(authored.size()));
  qint64 previousOccurrenceId = 0;
  for (qsizetype occurrenceIndex = 0;
       occurrenceIndex < authored.size(); ++occurrenceIndex) {
    const QString occurrencePath =
        definitionPath +
        QStringLiteral(".occurrences[%1]").arg(occurrenceIndex);
    if (!authored.at(occurrenceIndex).isObject())
      return invalid(occurrencePath + QStringLiteral(" must be an object"));
    const QJsonObject occurrence = authored.at(occurrenceIndex).toObject();
    QString error;
    if (!exactFields(occurrence,
                     {"occurrenceId", "targetDefinitionId", "placement",
                      "name", "visible", "suppressed", "grounded"},
                     occurrencePath, &error))
      return invalid(std::move(error));
    qint64 occurrenceIdValue = 0;
    qint64 targetIdValue = 0;
    if (!persistedId(occurrence.value(QStringLiteral("occurrenceId")),
                     &occurrenceIdValue,
                     occurrencePath + QStringLiteral(".occurrenceId"),
                     &error) ||
        !persistedId(
            occurrence.value(QStringLiteral("targetDefinitionId")),
            &targetIdValue,
            occurrencePath + QStringLiteral(".targetDefinitionId"),
            &error))
      return invalid(std::move(error));
    if (occurrenceIdValue <= previousOccurrenceId)
      return invalid(definitionPath +
                     QStringLiteral(" occurrence IDs are not sorted"));
    previousOccurrenceId = occurrenceIdValue;

    const auto placementValue = occurrence.value(QStringLiteral("placement"));
    if (!placementValue.isObject() ||
        !exactFields(placementValue.toObject(),
                     {"translationMm", "quaternionWxyz"},
                     occurrencePath + QStringLiteral(".placement"), &error))
      return invalid(error.isEmpty()
                         ? occurrencePath +
                               QStringLiteral(" placement invalid")
                         : std::move(error));
    std::vector<double> translation;
    std::vector<double> rotation;
    if (!numberArray(placementValue.toObject().value(
                         QStringLiteral("translationMm")),
                     3, &translation, occurrencePath, &error) ||
        !numberArray(placementValue.toObject().value(
                         QStringLiteral("quaternionWxyz")),
                     4, &rotation, occurrencePath, &error))
      return invalid(std::move(error));
    const auto transform = RigidTransform3d::create(
        {translation[0], translation[1], translation[2]},
        {rotation[0], rotation[1], rotation[2], rotation[3]});
    if (!transform)
      return invalid(occurrencePath + QStringLiteral(" transform invalid"));
    const std::array<double, 4> authoredRotation{
        rotation[0], rotation[1], rotation[2], rotation[3]};
    if (!exactQuaternion(authoredRotation,
                         transform.transform->quaternionWxyz()))
      return invalid(occurrencePath + QStringLiteral(
          " quaternion is not in canonical persisted form"));

    QString name;
    if (!boundedString(occurrence.value(QStringLiteral("name")), true, &name,
                       occurrencePath + QStringLiteral(".name"), &error))
      return invalid(std::move(error));
    const QByteArray nameBytes = name.toUtf8();
    const std::string nameUtf8(nameBytes.constData(),
                               static_cast<std::size_t>(nameBytes.size()));
    if (validateOccurrenceDisplayNameUtf8(nameUtf8, true) !=
        OccurrenceDisplayNameStatus::Valid)
      return invalid(occurrencePath + QStringLiteral(" name invalid"));

    const auto visible = occurrence.value(QStringLiteral("visible"));
    const auto suppressed = occurrence.value(QStringLiteral("suppressed"));
    const auto grounded = occurrence.value(QStringLiteral("grounded"));
    if (!visible.isBool() || !suppressed.isBool() || !grounded.isBool())
      return invalid(occurrencePath + QStringLiteral(" flags invalid"));
    const OccurrenceFlags flags{visible.toBool(), suppressed.toBool(),
                                grounded.toBool()};
    ProductEnvelopeV7OccurrenceCandidate decoded{
        OccurrenceId{static_cast<std::uint64_t>(occurrenceIdValue)},
        DefinitionId{static_cast<std::uint64_t>(targetIdValue)},
        *transform.transform, flags, nameUtf8, occurrence};
    result.occurrences.push_back(std::move(decoded));
  }
  result.status = StrictOccurrenceDecodeStatus::Valid;
  return result;
}

StrictOccurrenceEncodeResult encodeStrictOccurrence(
    OccurrenceId authoredOccurrenceId,
    const ComponentOccurrence& occurrence) {
  if (!persisted(authoredOccurrenceId.value()) ||
      !persisted(occurrence.targetDefinitionId().value()))
    return {};
  const auto flags = occurrence.flags();
  StrictOccurrenceEncodeResult result;
  result.status = StrictOccurrenceEncodeStatus::Valid;
  result.occurrence.emplace(QJsonObject{
      {QStringLiteral("occurrenceId"),
       static_cast<qint64>(authoredOccurrenceId.value())},
      {QStringLiteral("targetDefinitionId"),
       static_cast<qint64>(occurrence.targetDefinitionId().value())},
      {QStringLiteral("placement"),
       QJsonObject{
           {QStringLiteral("translationMm"),
            numbers(occurrence.placement().translationMm())},
           {QStringLiteral("quaternionWxyz"),
            numbers(occurrence.placement().quaternionWxyz())}}},
      {QStringLiteral("name"),
       QString::fromUtf8(
           occurrence.displayNameUtf8().data(),
           static_cast<qsizetype>(occurrence.displayNameUtf8().size()))},
      {QStringLiteral("visible"), flags.visible},
      {QStringLiteral("suppressed"), flags.suppressed},
      {QStringLiteral("grounded"), flags.grounded}});
  return result;
}

}  // namespace solidar::project::detail
