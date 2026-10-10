#include "ui/PartDesignErrorLocalization.h"

namespace solidar {

bool requiresPartDesignReselection(OperationFailureCode code) noexcept {
  switch (code) {
    case OperationFailureCode::TopologyReferenceMismatch:
    case OperationFailureCode::TopologyReferenceMissing:
    case OperationFailureCode::TopologyReferenceAmbiguous:
      return true;
    case OperationFailureCode::None:
    case OperationFailureCode::Unknown:
    case OperationFailureCode::InvalidInput:
    case OperationFailureCode::MissingSource:
    case OperationFailureCode::TopologyIndexUnavailable:
    case OperationFailureCode::TopologyReferenceInvalid:
    case OperationFailureCode::TopologyResolutionUnexpected:
    case OperationFailureCode::NonPlanarFace:
    case OperationFailureCode::UnsupportedSurface:
    case OperationFailureCode::UnsupportedGeometry:
    case OperationFailureCode::NoIntersection:
    case OperationFailureCode::InvalidProfileOpen:
    case OperationFailureCode::InvalidProfileOverlap:
    case OperationFailureCode::InvalidProfile:
    case OperationFailureCode::BodiesDoNotTouch:
    case OperationFailureCode::GeometryOperationFailed:
      return false;
  }
  return false;
}

QString localizedFaceToolError(const OperationFailure& failure) {
  if (requiresPartDesignReselection(failure.code)) {
    if (failure.code == OperationFailureCode::TopologyReferenceAmbiguous)
      return QString::fromUtf8(
          "Выбранная грань не может быть однозначно определена.");
    return QString::fromUtf8(
        "Не удалось восстановить выбранную грань после изменения модели.");
  }

  switch (failure.code) {
    case OperationFailureCode::NoIntersection:
      return QString::fromUtf8("Выдавливание не пересекает тело.");
    case OperationFailureCode::NonPlanarFace:
      return QString::fromUtf8("Для выдавливания выберите плоскую грань.");
    case OperationFailureCode::UnsupportedSurface:
      return QString::fromUtf8(
          "Создание эскиза на криволинейной поверхности пока не поддерживается.");
    default:
      return QString::fromUtf8("Не удалось определить выбранную поверхность.");
  }
}

QString localizedPartDesignError(PartDesignToolKind kind,
                                 const OperationFailure& failure) {
  if (requiresPartDesignReselection(failure.code))
    return QString::fromUtf8(
        "Выбранная геометрия больше не соответствует текущей модели. "
        "Выберите её заново.");

  switch (kind) {
    case PartDesignToolKind::Fillet:
      return QString::fromUtf8(
          "Не удалось построить скругление. Радиус слишком велик для "
          "выбранной геометрии или приводит к самопересечению.");
    case PartDesignToolKind::Chamfer:
      return QString::fromUtf8(
          "Не удалось построить фаску. Размер слишком велик для выбранной "
          "геометрии или приводит к самопересечению.");
    case PartDesignToolKind::Shell:
      return QString::fromUtf8(
          "Не удалось построить оболочку. Толщина слишком велика или "
          "приводит к самопересечению.");
    case PartDesignToolKind::Draft:
      return QString::fromUtf8(
          "Не удалось построить уклон. Проверьте угол, направление и "
          "выбранные грани.");
    default:
      return QString::fromUtf8("Не удалось построить предпросмотр операции.");
  }
}

}  // namespace solidar
