# Third-Party Notices

Этот файл описывает прямые сторонние компоненты исходного дерева SolidarCAD.
Финальный файл релиза должен генерироваться/проверяться по фактическому составу
пакета и включать транзитивные компоненты.

## Qt 6

Copyright (C) The Qt Company Ltd. and other contributors.

SolidarCAD использует Qt Core, Widgets, OpenGLWidgets и PrintSupport. Текущий
локальный профиль разработки — Qt 6.11.1; CMake допускает Qt 6.5 и новее.
Предполагаемая open-source схема поставки — динамическое связывание с
соблюдением LGPL-3.0-only. Коммерческая поставка может использовать отдельную
лицензию Qt. Точные тексты лицензий и notices конкретной сборки должны быть
включены из установленного Qt distribution.

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
