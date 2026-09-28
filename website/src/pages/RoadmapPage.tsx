import { roadmap, statusLabels } from "../data/roadmap";
import { githubUrl } from "../data/release";
export default function RoadmapPage() {
  return (
    <div className="container page">
      <span className="eyebrow">НАПРАВЛЕНИЕ РАЗВИТИЯ</span>
      <h1>
        От эскиза —<br />к рабочему инструменту.
      </h1>
      <p className="intro">
        Roadmap SolidarCAD. Реализованная основа, текущая работа
        <br />и следующие этапы — без неподтверждённых сроков.
      </p>
      <div className="roadmap">
        {roadmap.map((r, i) => (
          <article key={r.title} className="roadmap-step">
            <span className="step-index">0{i + 1}</span>
            <div>
              <span className={`badge status-${r.status.replace(" ", "-")}`}>
                {statusLabels[r.status]}
              </span>
              <h2>{r.title}</h2>
              <p>{r.text}</p>
            </div>
          </article>
        ))}
      </div>
      <p className="source-note">
        Основа: <a href={`${githubUrl}#roadmap`}>roadmap в README</a> с
        уточнением по текущему коду. Исходный список частично устарел:
        интеграция OCCT и инструменты эскиза уже реализованы. Статусы отражают
        готовность основы, а не завершённость всего продукта.
      </p>
    </div>
  );
}
