#pragma once

namespace solidar::project {

enum class ProjectPersistenceFormat {
  V3,
  V4,
};

// Plain project-layer value committed alongside application Document
// ownership. New and legacy projects default to v3; opening v4 selects v4 for
// subsequent saves without changing the serialized Document model itself.
struct ProjectPersistenceState {
  ProjectPersistenceFormat format{ProjectPersistenceFormat::V3};

  friend bool operator==(const ProjectPersistenceState &,
                         const ProjectPersistenceState &) = default;
};

} // namespace solidar::project
