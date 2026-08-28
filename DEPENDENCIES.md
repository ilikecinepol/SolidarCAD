# Реестр зависимостей SolidarCAD

Актуально на 28 августа 2026 г. Новая запись добавляется до merge согласно
[`docs/dependency-policy.md`](docs/dependency-policy.md).

## Прямые компоненты

| Компонент | Версия/ограничение | Назначение | Тип связи | Лицензия | Источник и фиксация | Владелец |
|---|---|---|---|---|---|---|
| Qt | CMake требует >= 6.5; локальный профиль 6.11.1 | Core, Widgets, OpenGLWidgets, PrintSupport | динамическая runtime-библиотека | LGPL-3.0-only или коммерческая лицензия в зависимости от поставки | `find_package(Qt6 6.5)`; release обязан фиксировать точный комплект | release owner |
| Open CASCADE Technology | 8.0.1 в текущей локальной сборке | B-Rep, topology, boolean, fillet, mesh | динамическая runtime-библиотека | LGPL-2.1-only WITH OCCT-exception-1.0 | vcpkg port `opencascade`, baseline `00c5775211f45cd08b37fce0484b4cb940e422ab` | CAD core owner |
| vcpkg | baseline `00c5775211f45cd08b37fce0484b4cb940e422ab` | получение и фиксация OCCT | build-only | MIT | `vcpkg.json` | build owner |
| CMake | >= 3.24 | конфигурация и сборка | build-only | BSD-3-Clause | `CMakeLists.txt` | build owner |
| MSVC / GCC | профиль сборки | компиляция C++20 | build-only | лицензия выбранного toolchain | release metadata | build owner |

## Собственный код

Лицензия собственного кода ещё не выбрана. До решения правообладателя нельзя
маркировать весь проект лицензией одной из сторонних библиотек.

## Правило релиза

Таблица описывает прямые зависимости исходного дерева. Перед публикацией
релиза состав сверяется с реально поставляемыми DLL/SO, а транзитивные
компоненты добавляются в SBOM, notices и каталог `LICENSES/`.
