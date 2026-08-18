#pragma once

#include <QString>
#include <optional>
#include <vector>

#include "model/Document.h"
#include "sketch/Sketch.h"

namespace solidar::project {

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

class ProjectFile final {
 public:
  static bool create(const QString& path, QString* error = nullptr);
  static bool save(const QString& path, const ProjectData& data,
                   QString* error = nullptr);
  static bool load(const QString& path, ProjectData* data,
                   QString* error = nullptr);
  static bool validate(const QString& path, QString* error = nullptr);
};

}  // namespace solidar::project
