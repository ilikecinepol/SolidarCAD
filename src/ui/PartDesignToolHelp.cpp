#include "ui/PartDesignToolHelp.h"

#include <array>
#include "ui/ToolIcon.h"

namespace solidar {
namespace {

using Kind = PartDesignToolKind;
struct Entry { Kind kind; const char* commandId; PartDesignToolHelp help; };

const std::array<Entry, 15> kEntries{{
    {Kind::None, "createSketch", {QString::fromUtf8("Создать эскиз"), QString::fromUtf8("Создаёт новый 2D-эскиз на базовой плоскости или плоской грани детали."), QString::fromUtf8("Создаёт новый 2D-эскиз для построения геометрии. Выберите базовую плоскость или плоскую грань детали. Эскиз можно использовать для выдавливания, вращения и других параметрических операций."), QString::fromUtf8("Выберите плоскость или плоскую грань для нового эскиза."), {}, {QString::fromUtf8("Выберите плоскость или плоскую грань для нового эскиза.")}}},
    {Kind::None, "referenceImage", {QString::fromUtf8("Изображение"), QString::fromUtf8("Размещает фотографию или чертёж на выбранной плоскости."), QString::fromUtf8("Выберите файл изображения, затем базовую плоскость или плоскую грань. Положение и масштаб меняются в свойствах изображения; видимость управляется в дереве построений."), QString::fromUtf8("Выберите плоскость или плоскую грань для изображения."), {}, {QString::fromUtf8("Выберите файл изображения."), QString::fromUtf8("Выберите плоскость или плоскую грань."), QString::fromUtf8("Настройте положение и масштаб в свойствах.")}}},
    {Kind::Extrude, "extrude", {QString::fromUtf8("Выдавливание"), QString::fromUtf8("Создаёт или изменяет объём, перемещая замкнутый профиль по прямой."), QString::fromUtf8("Создаёт 3D-объём из замкнутого эскиза. Выберите эскиз и задайте расстояние стрелкой во viewport либо числом. Операция может создать новое тело, объединить геометрию или выполнить вырез."), QString::fromUtf8("Выберите замкнутый эскиз."), QString::fromUtf8("Потяните стрелку или введите длину."), {QString::fromUtf8("Выберите замкнутый эскиз."), QString::fromUtf8("Потяните стрелку или введите длину."), QString::fromUtf8("Выберите операцию: новое тело / объединение / вырез.")}}},
    {Kind::Pocket, "pocket", {QString::fromUtf8("Вырез"), QString::fromUtf8("Удаляет объём, выдавливая замкнутый профиль внутрь детали."), QString::fromUtf8("Создаёт параметрический вырез из замкнутого эскиза. Выберите профиль и задайте глубину стрелкой во viewport либо числом."), QString::fromUtf8("Выберите замкнутый профиль для выреза."), QString::fromUtf8("Потяните стрелку или введите глубину."), {QString::fromUtf8("Выберите замкнутый профиль для выреза."), QString::fromUtf8("Задайте глубину выреза.")}}},
    {Kind::Revolve, "revolve", {QString::fromUtf8("Вращение"), QString::fromUtf8("Создаёт 3D-тело вращением выбранных профилей вокруг оси."), QString::fromUtf8("Выберите одну или несколько замкнутых областей прямо в 3D-виде (Ctrl добавляет или убирает область), затем щёлкните базовую ось или прямую эскиза. Угол меняется интерактивной дугой либо числом рядом с ней."), QString::fromUtf8("Выберите профиль; удерживайте Ctrl для нескольких областей."), QString::fromUtf8("Потяните синюю дугу или сразу введите угол в поле рядом с ней."), {QString::fromUtf8("Выберите профиль; Ctrl добавляет области."), QString::fromUtf8("Щёлкните базовую ось или прямую эскиза."), QString::fromUtf8("Потяните дугу или введите угол и нажмите Enter.")}}},
    {Kind::Fillet, "fillet", {QString::fromUtf8("Скругление"), QString::fromUtf8("Скругляет выбранные рёбра детали заданным радиусом."), QString::fromUtf8("Создаёт плавное закругление выбранных рёбер. Выберите одно или несколько рёбер мышью. Радиус меняется манипулятором во viewport или числом."), QString::fromUtf8("Выберите рёбра для скругления."), QString::fromUtf8("Задайте радиус."), {QString::fromUtf8("Выберите рёбра для скругления."), QString::fromUtf8("Задайте радиус.")}}},
    {Kind::Chamfer, "chamfer", {QString::fromUtf8("Фаска"), QString::fromUtf8("Срезает выбранные рёбра детали на заданное расстояние."), QString::fromUtf8("Создаёт фаску на одном или нескольких рёбрах. Выберите рёбра мышью и задайте размер фаски манипулятором во viewport или числом."), QString::fromUtf8("Выберите рёбра для фаски."), QString::fromUtf8("Задайте размер фаски."), {QString::fromUtf8("Выберите рёбра для фаски."), QString::fromUtf8("Задайте размер фаски.")}}},
    {Kind::JoinBodies, "joinBodies", {QString::fromUtf8("Соединить тела"), QString::fromUtf8("Объединяет два соприкасающихся или пересекающихся тела в одно."), QString::fromUtf8("Выберите последовательно два тела в 3D-виде. Будет создано новое параметрическое Body, зависящее от обоих исходных тел; исходные тела останутся в истории и будут скрыты."), QString::fromUtf8("Выберите два тела в 3D-виде."), {}, {QString::fromUtf8("Выберите первое тело."), QString::fromUtf8("Выберите второе тело.")}}},
    {Kind::Move, "move", {QString::fromUtf8("Перемещение"), QString::fromUtf8("Перемещает выбранное тело вдоль осей X, Y и Z."), QString::fromUtf8("Выберите тело прямо в 3D-виде. Перетаскивайте цветные стрелки X, Y и Z для интерактивного перемещения либо задайте точные смещения числом."), QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Потяните стрелку X, Y или Z либо введите смещения."), {QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Потяните стрелки или введите смещения X, Y и Z.")}}},
    {Kind::None, "ruler", {QString::fromUtf8("Линейка"), QString::fromUtf8("Измеряет расстояние между двумя точками в 3D-виде."), QString::fromUtf8("Укажите две точки на видимых телах. Линейка привязывается к вершинам и рёбрам, а также позволяет выбрать произвольную точку грани. Результат и разности координат показываются прямо в 3D-виде."), QString::fromUtf8("Выберите первую точку."), QString::fromUtf8("Выберите вторую точку."), {QString::fromUtf8("Выберите первую точку на теле."), QString::fromUtf8("Выберите вторую точку; Esc завершает измерение.")}}},
    {Kind::Shell, "shell", {QString::fromUtf8("Оболочка"), QString::fromUtf8("Превращает сплошное тело в тонкостенную оболочку."), QString::fromUtf8("Удаляет выбранные грани и создаёт стенки заданной толщины. Выберите одну или несколько граней, которые должны стать отверстиями, затем задайте толщину стенки. Оболочка может строиться внутрь или наружу."), QString::fromUtf8("Выберите грани, которые нужно удалить."), QString::fromUtf8("Задайте толщину и направление стенки."), {QString::fromUtf8("Выберите грани, которые нужно удалить."), QString::fromUtf8("Задайте толщину стенки."), QString::fromUtf8("Выберите направление: внутрь или наружу.")}}},
    {Kind::Draft, "draft", {QString::fromUtf8("Уклон"), QString::fromUtf8("Наклоняет выбранную поверхность относительно указанной оси."), QString::fromUtf8("Выберите поверхность детали прямо в 3D-виде, затем щёлкните базовую ось X, Y, Z либо прилегающее прямолинейное ребро этой поверхности. Направление задаётся знаком угла: перетаскивайте дугу в диапазоне от −89,99° до +89,99° либо введите значение рядом с ней."), QString::fromUtf8("Выберите поверхность для уклона в 3D-виде."), QString::fromUtf8("Потяните угловой манипулятор в нужную сторону или введите угол со знаком."), {QString::fromUtf8("Выберите поверхность для уклона."), QString::fromUtf8("Выберите ось X, Y, Z или прилегающее прямое ребро в 3D-виде."), QString::fromUtf8("Потяните дугу в нужную сторону или введите угол со знаком и нажмите Enter.")}}},
    {Kind::Mirror, "mirror", {QString::fromUtf8("Зеркало"), QString::fromUtf8("Создаёт зеркальную копию тела относительно выбранной плоскости."), QString::fromUtf8("Выберите тело в 3D-виде, затем укажите базовую плоскость зеркального отражения прямо на чертеже."), QString::fromUtf8("Выберите тело в 3D-виде."), {}, {QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Выберите базовую плоскость в 3D-виде.")}}},
    {Kind::LinearPattern, "linearPattern", {QString::fromUtf8("Линейный массив"), QString::fromUtf8("Повторяет выбранное тело вдоль выбранной оси."), QString::fromUtf8("Выберите тело прямо в 3D-виде, затем щёлкните базовую ось X, Y или Z. Шаг меняется интерактивной стрелкой либо числом; результат можно создать как новое тело или добавить в выбранное."), QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Потяните стрелку или задайте шаг, количество и операцию."), {QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Выберите базовую ось X, Y или Z в 3D-виде."), QString::fromUtf8("Потяните стрелку или введите шаг."), QString::fromUtf8("Задайте количество и операцию.")}}},
    {Kind::CircularPattern, "circularPattern", {QString::fromUtf8("Круговой массив"), QString::fromUtf8("Повторяет выбранное тело вокруг выбранной оси."), QString::fromUtf8("Выберите тело прямо в 3D-виде, затем щёлкните базовую ось X, Y или Z. Общий угол меняется интерактивной дугой либо числом; результат можно создать как новое тело или добавить в выбранное."), QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Потяните дугу или задайте угол, количество и операцию."), {QString::fromUtf8("Выберите тело в 3D-виде."), QString::fromUtf8("Выберите базовую ось X, Y или Z в 3D-виде."), QString::fromUtf8("Потяните дугу или введите угол."), QString::fromUtf8("Задайте количество и операцию.")}}},
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
  switch (kind) {
    case PartDesignToolKind::Extrude: return toolIcon(ToolIconKind::Extrude);
    case PartDesignToolKind::Pocket: return toolIcon(ToolIconKind::Pocket);
    case PartDesignToolKind::Revolve: return toolIcon(ToolIconKind::Revolve);
    case PartDesignToolKind::Fillet: return toolIcon(ToolIconKind::Fillet);
    case PartDesignToolKind::Chamfer: return toolIcon(ToolIconKind::Chamfer);
    case PartDesignToolKind::JoinBodies: return toolIcon(ToolIconKind::JoinBodies);
    case PartDesignToolKind::Move: return toolIcon(ToolIconKind::Move);
    case PartDesignToolKind::Mirror: return toolIcon(ToolIconKind::Mirror);
    case PartDesignToolKind::LinearPattern:
      return toolIcon(ToolIconKind::LinearPattern);
    case PartDesignToolKind::CircularPattern:
      return toolIcon(ToolIconKind::CircularPattern);
    case PartDesignToolKind::Shell: return toolIcon(ToolIconKind::Shell);
    case PartDesignToolKind::Draft: return toolIcon(ToolIconKind::Draft);
    case PartDesignToolKind::None: return {};
  }
  return {};
}

QIcon modelCommandIcon(const QString& commandId) {
  if (commandId == QLatin1String("createSketch"))
    return toolIcon(ToolIconKind::CreateSketch);
  if (commandId == QLatin1String("referenceImage"))
    return toolIcon(ToolIconKind::ReferenceImage);
  if (commandId == QLatin1String("ruler"))
    return toolIcon(ToolIconKind::Ruler);
  for (const auto& entry : kEntries)
    if (commandId == QLatin1String(entry.commandId)) return partDesignToolIcon(entry.kind);
  return {};
}

}  // namespace solidar
