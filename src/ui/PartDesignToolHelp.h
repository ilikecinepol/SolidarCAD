#pragma once

#include <QString>
#include <QStringList>

#include "model/PartDesignToolFramework.h"

namespace solidar {

struct PartDesignToolHelp {
  QString title;
  QString shortDescription;
  QString detailedDescription;
  QString selectionHint;
  QString parameterHint;
  QStringList steps;
};

[[nodiscard]] const PartDesignToolHelp* partDesignToolHelp(
    PartDesignToolKind kind) noexcept;
[[nodiscard]] const PartDesignToolHelp* modelCommandHelp(
    const QString& commandId) noexcept;
[[nodiscard]] QString partDesignToolStepHint(PartDesignToolKind kind,
                                             ToolSelectionStage stage);

}  // namespace solidar
