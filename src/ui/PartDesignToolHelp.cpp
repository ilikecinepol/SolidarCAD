#include "ui/PartDesignToolHelp.h"

#include <array>
#include <QPainter>
#include <QPixmap>

namespace solidar {
namespace {

using Kind = PartDesignToolKind;
struct Entry { Kind kind; const char* commandId; PartDesignToolHelp help; };

const std::array<Entry, 13> kEntries{{
    {Kind::None, "createSketch", {QString::fromUtf8("Создать эскиз"), QString::fromUtf8("Создаёт новый 2D-эскиз на базовой плоскости или плоской грани детали."), QString::fromUtf8("Создаёт новый 2D-эскиз для построения геометрии. Выберите базовую плоскость или плоскую грань детали. Эскиз можно использовать для выдавливания, вращения и других параметрических операций."), QString::fromUtf8("Выберите плоскость или плоскую грань для нового эскиза."), {}, {QString::fromUtf8("Выберите плоскость или плоскую грань для нового эскиза.")}}},
    {Kind::Extrude, "extrude", {QString::fromUtf8("Выдавливание"), QString::fromUtf8("Создаёт или изменяет объём, перемещая замкнутый профиль по прямой."), QString::fromUtf8("Создаёт 3D-объём из замкнутого эскиза. Выберите эскиз и задайте расстояние стрелкой во viewport либо числом. Операция может создать новое тело, объединить геометрию или выполнить вырез."), QString::fromUtf8("Выберите замкнутый эскиз."), QString::fromUtf8("Потяните стрелку или введите длину."), {QString::fromUtf8("Выберите замкнутый эскиз."), QString::fromUtf8("Потяните стрелку или введите длину."), QString::fromUtf8("Выберите операцию: новое тело / объединение / вырез.")}}},
    {Kind::Pocket, "pocket", {QString::fromUtf8("Вырез"), QString::fromUtf8("Удаляет объём, выдавливая замкнутый профиль внутрь детали."), QString::fromUtf8("Создаёт параметрический вырез из замкнутого эскиза. Выберите профиль и задайте глубину стрелкой во viewport либо числом."), QString::fromUtf8("Выберите замкнутый профиль для выреза."), QString::fromUtf8("Потяните стрелку или введите глубину."), {QString::fromUtf8("Выберите замкнутый профиль для выреза."), QString::fromUtf8("Задайте глубину выреза.")}}},
    {Kind::Revolve, "revolve", {QString::fromUtf8("Вращение"), QString::fromUtf8("Создаёт 3D-тело вращением замкнутого профиля вокруг выбранной оси."), QString::fromUtf8("Создаёт объём вращением замкнутого эскиза вокруг оси. Выберите профиль, затем прямую или базовую ось. Угол задаётся интерактивной дугой во viewport или числом."), QString::fromUtf8("Выберите замкнутый профиль, затем ось вращения."), QString::fromUtf8("Потяните дугу или введите угол."), {QString::fromUtf8("Выберите замкнутый профиль."), QString::fromUtf8("Выберите ось вращения."), QString::fromUtf8("Потяните дугу или введите угол.")}}},
    {Kind::Fillet, "fillet", {QString::fromUtf8("Скругление"), QString::fromUtf8("Скругляет выбранные рёбра детали заданным радиусом."), QString::fromUtf8("Создаёт плавное закругление выбранных рёбер. Выберите одно или несколько рёбер мышью. Радиус меняется манипулятором во viewport или числом."), QString::fromUtf8("Выберите рёбра для скругления."), QString::fromUtf8("Задайте радиус."), {QString::fromUtf8("Выберите рёбра для скругления."), QString::fromUtf8("Задайте радиус.")}}},
    {Kind::Chamfer, "chamfer", {QString::fromUtf8("Фаска"), QString::fromUtf8("Срезает выбранные рёбра детали на заданное расстояние."), QString::fromUtf8("Создаёт фаску на одном или нескольких рёбрах. Выберите рёбра мышью и задайте размер фаски манипулятором во viewport или числом."), QString::fromUtf8("Выберите рёбра для фаски."), QString::fromUtf8("Задайте размер фаски."), {QString::fromUtf8("Выберите рёбра для фаски."), QString::fromUtf8("Задайте размер фаски.")}}},
    {Kind::Shell, "shell", {QString::fromUtf8("Оболочка"), QString::fromUtf8("Превращает сплошное тело в тонкостенную оболочку."), QString::fromUtf8("Удаляет выбранные грани и создаёт стенки заданной толщины. Выберите одну или несколько граней, которые должны стать отверстиями, затем задайте толщину стенки. Оболочка может строиться внутрь или наружу."), QString::fromUtf8("Выберите грани, которые нужно удалить."), QString::fromUtf8("Задайте толщину и направление стенки."), {QString::fromUtf8("Выберите грани, которые нужно удалить."), QString::fromUtf8("Задайте толщину стенки."), QString::fromUtf8("Выберите направление: внутрь или наружу.")}}},
    {Kind::Draft, "draft", {QString::fromUtf8("Уклон"), QString::fromUtf8("Наклоняет выбранные грани относительно нейтральной плоскости."), QString::fromUtf8("Создаёт технологический уклон выбранных граней. В текущей версии используются нейтральная плоскость XY и направление вытягивания Z; выберите грани и задайте угол."), QString::fromUtf8("Выберите грани для уклона (плоскость XY, направление Z)."), QString::fromUtf8("Потяните угловой манипулятор или введите угол."), {QString::fromUtf8("Выберите грани для уклона."), QString::fromUtf8("Используются нейтральная плоскость XY и направление Z."), QString::fromUtf8("Потяните угловой манипулятор или введите угол.")}}},
    {Kind::Mirror, "mirror", {QString::fromUtf8("Зеркало"), QString::fromUtf8("Создаёт зеркальную копию геометрии относительно выбранной плоскости."), QString::fromUtf8("Отражает выбранную геометрию относительно плоскости. Выберите исходный объект или операцию, затем плоскость зеркального отражения."), QString::fromUtf8("Выберите исходную геометрию и плоскость зеркала."), {}, {QString::fromUtf8("Выберите исходную геометрию."), QString::fromUtf8("Выберите плоскость зеркала.")}}},
    {Kind::LinearPattern, "linearPattern", {QString::fromUtf8("Линейный массив"), QString::fromUtf8("Повторяет выбранную геометрию вдоль заданного направления."), QString::fromUtf8("Создаёт несколько копий выбранной геометрии вдоль одного направления. Выберите исходный объект и направление, затем задайте шаг и количество экземпляров."), QString::fromUtf8("Выберите исходную геометрию и направление массива."), QString::fromUtf8("Задайте шаг и количество экземпляров."), {QString::fromUtf8("Выберите исходную геометрию."), QString::fromUtf8("Выберите направление массива."), QString::fromUtf8("Задайте шаг."), QString::fromUtf8("Задайте количество экземпляров.")}}},
    {Kind::CircularPattern, "circularPattern", {QString::fromUtf8("Круговой массив"), QString::fromUtf8("Повторяет выбранную геометрию вокруг заданной оси."), QString::fromUtf8("Создаёт несколько копий выбранной геометрии вокруг оси. Выберите исходный объект и ось вращения, затем задайте общий угол и количество экземпляров."), QString::fromUtf8("Выберите исходную геометрию и ось массива."), QString::fromUtf8("Задайте угол и количество экземпляров."), {QString::fromUtf8("Выберите исходную геометрию."), QString::fromUtf8("Выберите ось массива."), QString::fromUtf8("Задайте угол распределения."), QString::fromUtf8("Задайте количество экземпляров.")}}},
}};

}  // namespace

const PartDesignToolHelp* partDesignToolHelp(PartDesignToolKind kind) noexcept {
  if (kind == PartDesignToolKind::None) return nullptr;
  for (const auto& entry : kEntries)
    if (entry.kind == kind) return &entry.help;
  return nullptr;
}

const PartDesignToolHelp* modelCommandHelp(const QString& commandId) noexcept {
  for (const auto& entry : kEntries)
    if (commandId == QLatin1String(entry.commandId)) return &entry.help;
  return nullptr;
}

QString partDesignToolStepHint(PartDesignToolKind kind,
                               ToolSelectionStage stage) {
  const auto* help = partDesignToolHelp(kind);
  if (!help) return {};
  switch (stage) {
    case ToolSelectionStage::SelectingInput:
      return help->selectionHint;
    case ToolSelectionStage::SelectingReference:
      return help->steps.size() > 1 ? help->steps[1] : help->selectionHint;
    case ToolSelectionStage::EditingParameters:
      return help->parameterHint;
    case ToolSelectionStage::None:
      return {};
  }
  return {};
}

QIcon partDesignToolIcon(PartDesignToolKind kind) {
  QPixmap pixmap(24, 24);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(QColor("#185ca8"), 2.0, Qt::SolidLine, Qt::RoundCap,
           Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(QColor("#d9eaff"));
  switch (kind) {
    case PartDesignToolKind::Extrude:
      painter.drawRect(4, 8, 11, 11); painter.drawLine(15, 8, 20, 4);
      painter.drawLine(15, 19, 20, 15); painter.drawLine(20, 4, 20, 15); break;
    case PartDesignToolKind::Pocket:
      painter.drawRect(3, 5, 18, 15); painter.drawRect(8, 5, 8, 9);
      painter.drawLine(10, 8, 14, 12); painter.drawLine(14, 8, 10, 12); break;
    case PartDesignToolKind::Revolve:
      painter.drawArc(3, 3, 18, 18, 40 * 16, 285 * 16);
      painter.drawLine(17, 3, 21, 5); painter.drawLine(17, 3, 18, 8); break;
    case PartDesignToolKind::Fillet:
      painter.setBrush(Qt::NoBrush); painter.drawLine(4, 20, 4, 11);
      painter.drawArc(4, 4, 16, 16, 90 * 16, 90 * 16); painter.drawLine(12, 4, 20, 4); break;
    case PartDesignToolKind::Chamfer:
      painter.setBrush(Qt::NoBrush); painter.drawPolyline(QPolygon({QPoint(4,20),QPoint(4,12),QPoint(12,4),QPoint(20,4)})); break;
    case PartDesignToolKind::Shell:
      painter.drawRect(4, 4, 16, 16); painter.setBrush(Qt::white);
      painter.drawRect(8, 8, 8, 12); break;
    case PartDesignToolKind::Draft:
      painter.drawPolygon(QPolygon({QPoint(5,20),QPoint(9,4),QPoint(19,4),QPoint(19,20)}));
      painter.drawLine(9, 4, 5, 4); break;
    case PartDesignToolKind::Mirror:
      painter.drawLine(12, 2, 12, 22); painter.drawPolygon(QPolygon({QPoint(3,18),QPoint(9,5),QPoint(9,18)}));
      painter.setBrush(Qt::NoBrush); painter.drawPolygon(QPolygon({QPoint(21,18),QPoint(15,5),QPoint(15,18)})); break;
    case PartDesignToolKind::LinearPattern:
      for (int x : {3, 10, 17}) painter.drawRect(x, 8, 5, 8); break;
    case PartDesignToolKind::CircularPattern:
      for (const QPoint& p : {QPoint(9,2), QPoint(16,9), QPoint(9,16), QPoint(2,9)})
        painter.drawEllipse(p.x(), p.y(), 5, 5); break;
    case PartDesignToolKind::None: break;
  }
  return QIcon(pixmap);
}

QIcon modelCommandIcon(const QString& commandId) {
  if (commandId == QLatin1String("createSketch")) {
    QPixmap pixmap(24, 24); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setPen(QPen(QColor("#185ca8"), 2));
    painter.drawRect(4, 4, 14, 14); painter.drawLine(9, 12, 21, 2);
    painter.drawLine(17, 2, 21, 6);
    return QIcon(pixmap);
  }
  for (const auto& entry : kEntries)
    if (commandId == QLatin1String(entry.commandId)) return partDesignToolIcon(entry.kind);
  return {};
}

}  // namespace solidar
