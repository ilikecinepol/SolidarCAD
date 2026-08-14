#pragma once

#include <QString>

namespace solidar::project {

class ProjectFile final {
 public:
  static bool create(const QString& path, QString* error = nullptr);
  static bool validate(const QString& path, QString* error = nullptr);
};

}  // namespace solidar::project
