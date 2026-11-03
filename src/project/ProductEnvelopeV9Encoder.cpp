#include "project/ProductEnvelopeV9Encoder.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <exception>
#include <utility>

#include "model/AssemblyMate.h"
#include "project/ProductEnvelopeV9Decoder.h"
#include "project/ProjectFile.h"
#include "project/ProjectFormatV4ReferenceEncoder.h"
#include "project/internal/ProductEnvelopeV8Encoding.h"

namespace solidar::project {
namespace {

ProductEnvelopeV9EncodeResult failed(ProductEnvelopeV9EncodeStatus status,
                                     QString error) {
  ProductEnvelopeV9EncodeResult result;
  result.status = status;
  result.error = std::move(error);
  return result;
}

QJsonObject encodeOptions(const MateSolverOptions& options) {
  return {{QStringLiteral("linearToleranceMm"), options.linearToleranceMm},
          {QStringLiteral("angularToleranceRad"), options.angularToleranceRad},
          {QStringLiteral("rankTolerance"), options.rankTolerance},
          {QStringLiteral("maximumAbsoluteOffsetMm"),
           options.maximumAbsoluteOffsetMm},
          {QStringLiteral("maximumTranslationStepMm"),
           options.maximumTranslationStepMm},
          {QStringLiteral("maximumAngularStepRad"),
           options.maximumAngularStepRad},
          {QStringLiteral("maximumIterations"),
           static_cast<qint64>(options.maximumIterations)},
          {QStringLiteral("maximumNodes"),
           static_cast<qint64>(options.maximumNodes)},
          {QStringLiteral("maximumMates"),
           static_cast<qint64>(options.maximumMates)},
          {QStringLiteral("maximumWorkUnits"),
           static_cast<qint64>(options.maximumWorkUnits)}};
}

std::optional<QJsonObject> encodeEndpoint(const MateEndpoint& endpoint) {
  if (endpoint.occurrencePath.depth() == 0 ||
      endpoint.occurrencePath.depth() > kAssemblyGraphMaximumPathDepth ||
      !endpoint.face.isV2())
    return std::nullopt;
  QJsonArray path;
  for (const auto id : endpoint.occurrencePath.entries()) {
    if (!id.isValid() ||
        id.value() > static_cast<std::uint64_t>(
                         ProjectFile::kMaximumPersistedId))
      return std::nullopt;
    path.push_back(static_cast<qint64>(id.value()));
  }
  const auto face = encodeProjectFormatV4TopologyReference(endpoint.face);
  if (!face.succeeded() || !face.reference) return std::nullopt;
  return QJsonObject{{QStringLiteral("occurrencePath"), path},
                     {QStringLiteral("face"), *face.reference}};
}

std::optional<QJsonObject> encodeAxisEndpoint(
    const MateAxisEndpoint& endpoint) {
  if (endpoint.occurrencePath.depth() == 0 ||
      endpoint.occurrencePath.depth() > kAssemblyGraphMaximumPathDepth)
    return std::nullopt;
  QJsonArray path;
  for (const auto id : endpoint.occurrencePath.entries()) {
    if (!id.isValid() ||
        id.value() > static_cast<std::uint64_t>(
                         ProjectFile::kMaximumPersistedId))
      return std::nullopt;
    path.push_back(static_cast<qint64>(id.value()));
  }
  QJsonObject axis;
  if (const auto* datum = std::get_if<LocalDatumAxis>(&endpoint.reference)) {
    if (!isValidLocalDatumAxis(*datum)) return std::nullopt;
    axis.insert(QStringLiteral("referenceKind"),
                QStringLiteral("local-datum-axis-v1"));
    axis.insert(QStringLiteral("axis"),
                *datum == LocalDatumAxis::X
                    ? QStringLiteral("x")
                    : (*datum == LocalDatumAxis::Y ? QStringLiteral("y")
                                                   : QStringLiteral("z")));
  } else {
    const auto& face =
        std::get<CylindricalFaceAxis>(endpoint.reference).face;
    if (!face.isV2()) return std::nullopt;
    const auto encoded = encodeProjectFormatV4TopologyReference(face);
    if (!encoded.succeeded() || !encoded.reference) return std::nullopt;
    axis.insert(QStringLiteral("referenceKind"),
                QStringLiteral("cylindrical-face-axis-v1"));
    axis.insert(QStringLiteral("face"), *encoded.reference);
  }
  return QJsonObject{{QStringLiteral("occurrencePath"), path},
                     {QStringLiteral("axis"), axis}};
}

std::optional<QJsonObject> encodePointEndpoint(
    const MatePointEndpoint& endpoint) {
  if (endpoint.occurrencePath.depth() == 0 ||
      endpoint.occurrencePath.depth() > kAssemblyGraphMaximumPathDepth)
    return std::nullopt;
  QJsonArray path;
  for (const auto id : endpoint.occurrencePath.entries()) {
    if (!id.isValid() ||
        id.value() > static_cast<std::uint64_t>(
                         ProjectFile::kMaximumPersistedId))
      return std::nullopt;
    path.push_back(static_cast<qint64>(id.value()));
  }
  QJsonObject point;
  if (const auto* datum = std::get_if<LocalDatumPoint>(&endpoint.reference)) {
    if (!isValidLocalDatumPoint(*datum)) return std::nullopt;
    point.insert(QStringLiteral("referenceKind"),
                 QStringLiteral("local-datum-origin-v1"));
  } else {
    const auto& edge =
        std::get<CircularEdgeCenter>(endpoint.reference).edge;
    if (!edge.isV2()) return std::nullopt;
    const auto encoded = encodeProjectFormatV4TopologyReference(edge);
    if (!encoded.succeeded() || !encoded.reference) return std::nullopt;
    point.insert(QStringLiteral("referenceKind"),
                 QStringLiteral("circular-edge-center-v1"));
    point.insert(QStringLiteral("edge"), *encoded.reference);
  }
  return QJsonObject{{QStringLiteral("occurrencePath"), path},
                     {QStringLiteral("point"), point}};
}

}  // namespace

ProductEnvelopeV9EncodeResult encodeProductEnvelopeV9Root(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata) {
  try {
    auto base = detail::encodeProductEnvelopeV8BaseRoot(
        source, outputProjectId, metadata, false);
    if (!base.succeeded())
      return failed(base.status == ProductEnvelopeV8EncodeStatus::PayloadTooLarge
                        ? ProductEnvelopeV9EncodeStatus::PayloadTooLarge
                        : ProductEnvelopeV9EncodeStatus::Invalid,
                    base.error);
    QJsonObject root = base.payload->root;
    root.insert(QStringLiteral("version"), 9);
    bool hasAngularMate = false;
    bool hasAngleMate = false;
    bool hasConcentricMate = false;
    bool hasPointDistanceMate = false;
    for (const auto definitionId : source.definitions().definitionIds()) {
      const auto* assembly = source.definitions().findAssembly(definitionId);
      if (!assembly) continue;
      for (const auto& [mateId, mate] :
           assembly->mateRegistry().relations()) {
        static_cast<void>(mateId);
        const auto kind = assemblyMateKind(mate);
        hasAngularMate = hasAngularMate ||
                         kind == AssemblyMateKind::PlanarParallel ||
                         kind == AssemblyMateKind::PlanarPerpendicular;
        hasAngleMate = hasAngleMate ||
                       kind == AssemblyMateKind::PlanarAngle;
        hasConcentricMate =
            hasConcentricMate ||
            kind == AssemblyMateKind::AxisConcentric;
        hasPointDistanceMate =
            hasPointDistanceMate ||
            kind == AssemblyMateKind::PointDistance;
      }
    }
    QJsonArray capabilities;
    if (hasConcentricMate)
      capabilities.push_back(
          QStringLiteral("assembly-axis-concentric-mates-v1"));
    if (hasAngleMate)
      capabilities.push_back(
          QStringLiteral("assembly-planar-angle-mates-v1"));
    if (hasAngularMate)
      capabilities.push_back(
          QStringLiteral("assembly-planar-angular-mates-v1"));
    capabilities.push_back(QStringLiteral("assembly-planar-mates-v1"));
    if (hasPointDistanceMate)
      capabilities.push_back(
          QStringLiteral("assembly-point-distance-mates-v1"));
    capabilities.push_back(QStringLiteral("linked-parts-v1"));
    capabilities.push_back(QStringLiteral("nested-assemblies-v1"));
    capabilities.push_back(QStringLiteral("product-envelope-v1"));
    root.insert(QStringLiteral("capabilities"), capabilities);
    QJsonArray definitions;
    for (const auto& definitionValue :
         root.value(QStringLiteral("definitions")).toArray()) {
      auto definition = definitionValue.toObject();
      if (definition.value(QStringLiteral("definitionKind")).toString() !=
          QStringLiteral("assembly")) {
        definitions.push_back(definition);
        continue;
      }
      const DefinitionId id{static_cast<std::uint64_t>(
          definition.value(QStringLiteral("definitionId")).toInteger())};
      const auto* assembly = source.definitions().findAssembly(id);
      if (!assembly || !assembly->mateRegistry().options().isValid())
        return failed(ProductEnvelopeV9EncodeStatus::Invalid,
                      QStringLiteral("v9 assembly registry is invalid"));
      definition.insert(QStringLiteral("mateSolverOptions"),
                        encodeOptions(assembly->mateRegistry().options()));
      QJsonArray mates;
      for (const auto& [mateId, mate] :
           assembly->mateRegistry().relations()) {
        if (!mateId.isValid() ||
            mateId.value() > static_cast<std::uint64_t>(
                                 ProjectFile::kMaximumPersistedId) ||
            !isValidAssemblyMate(
                mate,
                assembly->mateRegistry().options().maximumAbsoluteOffsetMm,
                assembly->mateRegistry().options().angularToleranceRad))
          return failed(ProductEnvelopeV9EncodeStatus::Invalid,
                        QStringLiteral("v9 mate is invalid"));
        std::optional<QJsonObject> first;
        std::optional<QJsonObject> second;
        if (const auto* concentric =
                std::get_if<AxisConcentricMate>(&mate)) {
          first = encodeAxisEndpoint(concentric->first());
          second = encodeAxisEndpoint(concentric->second());
        } else if (const auto* distance =
                       std::get_if<PointDistanceMate>(&mate)) {
          first = encodePointEndpoint(distance->first());
          second = encodePointEndpoint(distance->second());
        } else {
          first = encodeEndpoint(*assemblyPlanarMateFirst(mate));
          second = encodeEndpoint(*assemblyPlanarMateSecond(mate));
        }
        if (!first || !second)
          return failed(ProductEnvelopeV9EncodeStatus::Invalid,
                        QStringLiteral("v9 endpoint is invalid"));
        QJsonObject encoded{
            {QStringLiteral("mateId"), static_cast<qint64>(mateId.value())},
            {QStringLiteral("first"), *first},
            {QStringLiteral("second"), *second}};
        if (const auto* coincident =
                std::get_if<PlanarCoincidentMate>(&mate)) {
          encoded.insert(QStringLiteral("type"),
                         QStringLiteral("planar-coincident-v1"));
          encoded.insert(
              QStringLiteral("orientation"),
              coincident->orientation() == PlanarMateOrientation::Aligned
                  ? QStringLiteral("aligned")
                  : QStringLiteral("opposed"));
          encoded.insert(QStringLiteral("offsetMm"), coincident->offsetMm());
        } else if (const auto* parallel =
                       std::get_if<PlanarParallelMate>(&mate)) {
          encoded.insert(QStringLiteral("type"),
                         QStringLiteral("planar-parallel-v1"));
          encoded.insert(
              QStringLiteral("orientation"),
              parallel->orientation() == PlanarMateOrientation::Aligned
                  ? QStringLiteral("aligned")
                  : QStringLiteral("opposed"));
        } else if (std::holds_alternative<PlanarPerpendicularMate>(mate)) {
          encoded.insert(QStringLiteral("type"),
                         QStringLiteral("planar-perpendicular-v1"));
        } else if (std::holds_alternative<AxisConcentricMate>(mate)) {
          encoded.insert(QStringLiteral("type"),
                         QStringLiteral("axis-concentric-v1"));
        } else if (const auto* distance =
                       std::get_if<PointDistanceMate>(&mate)) {
          encoded.insert(QStringLiteral("type"),
                         QStringLiteral("point-distance-v1"));
          encoded.insert(QStringLiteral("distanceMm"), distance->distanceMm());
        } else {
          const auto& angle = std::get<PlanarAngleMate>(mate);
          encoded.insert(QStringLiteral("type"),
                         QStringLiteral("planar-angle-v1"));
          encoded.insert(
              QStringLiteral("orientation"),
              angle.orientation() == PlanarMateOrientation::Aligned
                  ? QStringLiteral("aligned")
                  : QStringLiteral("opposed"));
          encoded.insert(QStringLiteral("angleRad"), angle.angleRad());
        }
        mates.push_back(encoded);
      }
      definition.insert(QStringLiteral("mates"), mates);
      definitions.push_back(definition);
    }
    root.insert(QStringLiteral("definitions"), definitions);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > ProjectFile::kMaximumFileBytes)
      return failed(ProductEnvelopeV9EncodeStatus::PayloadTooLarge,
                    QStringLiteral("serialized v9 exceeds file budget"));
    const auto rescanned = scanProductEnvelopeV9(QByteArrayView(bytes));
    if (!rescanned.hasCandidate() || rescanned.candidate->authoredRoot != root)
      return failed(ProductEnvelopeV9EncodeStatus::SerializedPayloadValidation,
                    rescanned.error.isEmpty()
                        ? QStringLiteral("serialized v9 failed self-scan")
                        : rescanned.error);
    ProductEnvelopeV9EncodeResult result;
    result.status = ProductEnvelopeV9EncodeStatus::Valid;
    result.payload.emplace(ProductEnvelopeV9EncodedPayload{root, bytes});
    return result;
  } catch (const std::exception& exception) {
    return failed(ProductEnvelopeV9EncodeStatus::Invalid,
                  QString::fromUtf8(exception.what()));
  } catch (...) {
    return failed(ProductEnvelopeV9EncodeStatus::Invalid,
                  QStringLiteral("unknown v9 encoding failure"));
  }
}

}  // namespace solidar::project
