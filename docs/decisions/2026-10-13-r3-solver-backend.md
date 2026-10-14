# R3 solver backend: OCCT TKMath

Дата: 13 октября 2026 г.
Статус: принято для Solver v2

## Контекст

R3 требует component-level nonlinear solve, rank/DOF, локализацию конфликтов и
детерминированный выбор позы без собственного набора QR/SVD-примитивов. Были
проверены два backend-варианта за одной границей `ISketchSolver`.

## Изолированные spikes

### PlaneGCS

Исходники PlaneGCS были собраны отдельно от production graph в
`build/spikes/planegcs-src`; зависимости установлены в изолированный
`build/spikes/vcpkg_installed`. Минимальный C++/Python adapter использовал
pointer-stable arena, поскольку внутренний порядок указателей иначе влияет на
порядок неизвестных. Набор `test_basic.py` + `test_determinism.py`: 22/22,
0,30 с. Первичная Windows-сборка 36 транзитивных пакетов заняла 4,7 мин.

- Vocabulary: сильное покрытие классических 2D constraints, но требуется
  отдельный adapter и двустороннее сопоставление каждой SolidarCAD-ссылки.
- License/delivery: код извлечён из FreeCAD, LGPL-2.1-or-later; новая поставка,
  notices и SBOM policy обязательны.
- Windows/Linux: upstream поддерживает обе платформы, но добавляет Boost и
  Eigen в production dependency closure.
- Diagnostics: есть nonlinear solving, но стабильные SolidarCAD IDs,
  provenance, bounded conflict sets и наша нормализация всё равно остаются в
  adapter/application layer.
- Determinism: достигается только после фиксации storage/order в adapter.
- Cost: самый большой build/dependency и licensing footprint из вариантов.

### Тонкий backend поверх OCCT TKMath

Spike использовал уже поставляемый `TKMath`: `math_SVD` для damped least
squares и `MathLin::SVD` для rank/nullspace. Residuals и forward-AD Jacobian
остаются в R2 `EquationKernel`; backend не содержит отдельной геометрической
математики.

- Vocabulary: автоматически совпадает со всем registry R2.
- License/delivery: новой прямой зависимости нет; OCCT уже входит в Windows и
  Linux builds проекта.
- Diagnostics: rank, DOF, redundancy и remove-one conflict probes используют
  тот же normalized Jacobian и стабильные constraint IDs.
- Determinism: layout и rows сортируются по стабильным ID, previous candidate
  и transient hints задают единственную политику позы.
- Cost: минимальная поверхность сопровождения; плотные матрицы ограничивают
  масштаб одной connected component и требуют benchmark gate.

## Решение

Принят тонкий `OcctSketchSolver` за `ISketchSolver`. Он решает immutable
`EquationSystem`, возвращает candidate vector и typed `SolveReport`; публикация
candidate выполняется отдельно и атомарно. Собственные QR/SVD/DogLeg primitives
не реализуются. `vcpkg.json`, notices и SBOM не меняются, потому что новая
production dependency не появилась.

PlaneGCS остаётся differential oracle для малых fixtures, но не включается в
production binary. Решение следует пересмотреть, если connected-component
benchmarks устойчиво нарушат R3 latency gate или sparse systems станут обычной
нагрузкой.
