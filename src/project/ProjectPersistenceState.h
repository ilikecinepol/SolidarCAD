#pragma once

namespace solidar::project {

enum class ProjectPersistenceFormat {
  V3,
  V4,
  V5,
};

// Format-only routing value committed alongside application document
// ownership. Product identity and metadata remain authoritative exclusively
// in ProductDocument; V5 deliberately carries no shadow identity state here.
struct ProjectPersistenceState {
  ProjectPersistenceFormat format{ProjectPersistenceFormat::V3};

  friend bool operator==(const ProjectPersistenceState &,
                         const ProjectPersistenceState &) = default;
};

} // namespace solidar::project
