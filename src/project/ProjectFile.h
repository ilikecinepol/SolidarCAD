#pragma once

#include <QString>
#include <optional>
#include <string_view>
#include <vector>

#include "model/Document.h"
#include "model/PatternTypes.h"
#include "sketch/Sketch.h"

namespace solidar::project {

struct FeatureCodecDescriptor {
  FeatureKind kind;
  std::string_view token;
};

// Read-only projection of the authoritative v2 feature codec registry.
[[nodiscard]] std::vector<FeatureCodecDescriptor> featureCodecDescriptors();

struct SavedSketch {
  sketch::Sketch geometry;
  QString support{QStringLiteral("XY")};
};

struct ProjectData {
  BoxParameters box;
  std::vector<SavedSketch> sketches;
  bool hasExtrusion{false};
  std::optional<std::size_t> extrusionSourceSketch;
};

enum class ProjectLoadStatus { Valid, Invalid, Unsupported };

enum class ProjectPayloadKind {
  None,
  LegacyPartV1,
  ParametricPartV2,
  ParametricPartV3
};

enum class PartPayloadDisposition { Unsupported, Legacy, Parametric };

[[nodiscard]] constexpr PartPayloadDisposition classifyPartPayload(
    ProjectPayloadKind kind) noexcept {
  switch (kind) {
    case ProjectPayloadKind::LegacyPartV1:
      return PartPayloadDisposition::Legacy;
    case ProjectPayloadKind::ParametricPartV2:
    case ProjectPayloadKind::ParametricPartV3:
      return PartPayloadDisposition::Parametric;
    case ProjectPayloadKind::None:
    default:
      return PartPayloadDisposition::Unsupported;
  }
}

// Compatibility facade for existing part-only callers. New dispatch must use
// status, sourceVersion and payloadKind instead of inferring one axis from
// another.
enum class ProjectLoadKind { ValidV1, ValidV2, Invalid, Unsupported };

struct StagedProjectLoad {
  ProjectLoadStatus status{ProjectLoadStatus::Invalid};
  std::optional<qint64> sourceVersion;
  ProjectPayloadKind payloadKind{ProjectPayloadKind::None};
  ProjectLoadKind kind{ProjectLoadKind::Invalid};
  ProjectData legacy;
  std::optional<Document> document;
  QString error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == ProjectLoadStatus::Valid;
  }
};

class ProjectFile final {
 public:
  // Decoder resource limits are part of the persistence boundary and are
  // intentionally shared with regression tests.
  static constexpr qint64 kMaximumFileBytes = 64 * 1024 * 1024;
  // Persisted IDs are IEEE-754 exact integers.  Keep deterministic headroom
  // so a loaded document can create and save new objects without immediately
  // exhausting the persistence range.
  static constexpr qint64 kPersistedIdGenerationHeadroom = 1000000;
  static constexpr qint64 kMaximumPersistedId =
      9007199254740991LL - kPersistedIdGenerationHeadroom;
  static constexpr qsizetype kMaximumCollectionItems = 100000;
  static constexpr qsizetype kMaximumStringCharacters = 16384;
  static constexpr qsizetype kMaximumBRepBase64Characters =
      64 * 1024 * 1024;
  static constexpr qsizetype kMaximumSketchGeometryItems = 10000;
  static constexpr qsizetype kMaximumSketchConstraintItems = 5000;
  static constexpr qint64 kMaximumSketchSolveComplexity = 2000000;
  static constexpr qsizetype kMaximumDocumentSketchGeometryItems = 50000;
  static constexpr qsizetype kMaximumDocumentSketchConstraintItems = 20000;
  static constexpr qint64 kMaximumDocumentSketchSolveComplexity = 5000000;
  static constexpr qsizetype kMaximumDocumentSketches = 4096;
  static constexpr qsizetype kMaximumDocumentBodies = 256;
  static constexpr qsizetype kMaximumDocumentFeatures = 4096;
  static constexpr qint64 kMaximumDocumentRebuildWork = 10000;
  static constexpr qsizetype kMaximumFeatureTopologyReferences = 1024;
  static constexpr qsizetype kMaximumDocumentTopologyReferences = 4096;
  static constexpr int kMinimumPatternCount =
      ::solidar::kMinimumPatternCount;
  static constexpr int kMaximumPatternCount =
      ::solidar::kMaximumPatternCount;

  static bool create(const QString& path, QString* error = nullptr);
  static bool save(const QString& path, const ProjectData& data,
                   QString* error = nullptr);
  static bool load(const QString& path, ProjectData* data,
                   QString* error = nullptr);
  static bool saveDocument(const QString& path, const Document& document,
                           QString* error = nullptr);
  static bool loadDocument(const QString& path, Document* document,
                           QString* error = nullptr);
  static bool validate(const QString& path, QString* error = nullptr);
  [[nodiscard]] static StagedProjectLoad stageLoad(const QString& path);
};

}  // namespace solidar::project
