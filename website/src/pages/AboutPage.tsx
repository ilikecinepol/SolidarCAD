import { githubUrl } from "../data/release";
export default function AboutPage() {
  return (
    <div className="container page about">
      <span className="eyebrow">О ПРОЕКТЕ</span>
      <h1>
        Точность в геометрии.
        <br />
        Свобода в работе.
      </h1>
      <p className="intro">
        SolidarCAD — развивающаяся локальная система
        <br />
        параметрического 2D- и 3D-проектирования.
      </p>
      <div className="about-grid">
        <section>
          <h2>От эскиза к детали</h2>
          <p>
            Первый ориентир проекта — надёжный процесс построения детали: эскиз
            с ограничениями, создание объёма и редактируемая история операций.
          </p>
          <p>
            Проект находится на стадии раннего прототипа. Его исходный код
            доступен на GitHub; лицензия собственного кода пока не выбрана.
          </p>
          <a className="text-link" href={githubUrl}>
            Репозиторий SolidarCAD ↗
          </a>
        </section>
        <section>
          <h2>Принципы</h2>
          <ul className="principles">
            <li>Локальные файлы и работа без обязательного облака</li>
            <li>Параметры и история как основа модели</li>
            <li>Русскоязычный интерфейс</li>
            <li>Развитие с учётом Windows и Ubuntu</li>
          </ul>
        </section>
      </div>
      <section className="stack">
        <span className="eyebrow">ТЕХНОЛОГИИ В ОСНОВЕ</span>
        <div>
          <span>C++20</span>
          <span>Qt 6</span>
          <span>Open CASCADE</span>
          <span>CMake</span>
        </div>
        <p>
          Qt отвечает за интерфейс, OCCT — за B-Rep геометрию. Параметры и связи
          сохраняются в .solidar; геометрия восстанавливается при пересчёте.
        </p>
      </section>
    </div>
  );
}
