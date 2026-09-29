# Third-Party Notices

Этот файл описывает прямые сторонние компоненты исходного дерева SolidarCAD.
Финальный файл релиза должен генерироваться/проверяться по фактическому составу
пакета и включать транзитивные компоненты.

## Qt 6

Copyright (C) The Qt Company Ltd. and other contributors.

Исходное дерево использует Qt Core, Gui, Widgets, OpenGLWidgets и PrintSupport.
Windows MVP собирается с Qt 6.8.3; его переносимый пакет динамически поставляет
Qt Core, Gui, Network, OpenGL, OpenGLWidgets, SVG и Widgets, а также плагины,
выбранные `windeployqt`.

Open-source поставка использует условия LGPL-3.0-only для применимых модулей.
Канонические тексты лицензий из официальных тегов `qtbase` и `qtsvg` версии
6.8.3, а также SPDX inventories установленной бинарной сборки находятся в
`LICENSES/Qt`. DLL и плагины остаются отдельными динамическими библиотеками и
могут быть заменены пользователем совместимыми сборками Qt.

Соответствующий исходный код Qt 6.8.3 доступен в официальном архиве:
https://download.qt.io/official_releases/qt/6.8/6.8.3/submodules/

Официальная информация: https://www.qt.io/licensing/ и
https://doc.qt.io/qt-6/licenses-used-in-qt.html

## Open CASCADE Technology 8.0.1

Copyright (C) OPEN CASCADE S.A.S. and contributors.

OCCT используется для B-Rep-геометрии, топологии, boolean-операций, fillet и
mesh. Компонент распространяется по LGPL-2.1 с Open CASCADE exception.
Релизный пакет должен содержать полученные вместе с точной сборкой OCCT файлы
`LICENSE_LGPL_21.txt` и `OCCT_LGPL_EXCEPTION.txt` (либо их канонические
эквиваленты без изменения текста).

Официальная информация: https://dev.opencascade.org/resources/licensing

## Инструменты сборки

vcpkg (MIT), CMake (BSD-3-Clause) и компилятор являются build-only
компонентами и не входят в runtime автоматически. Если их файлы попадут в
дистрибутив, соответствующие лицензии нужно включить в release notices.

Direct-build inventory воспроизводимо создаётся `scripts/generate_sbom.py`;
release inventory дополняется результатом сканирования фактического
staging-каталога.
