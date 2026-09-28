import { Link, useSearchParams } from "react-router-dom";
import { docGroups } from "../data/docs";
import { githubUrl } from "../data/release";
export default function DocsPage() {
  const [params] = useSearchParams();
  const id = params.get("topic") || "install";
  const topic = docGroups.flatMap((g) => g.items).find((i) => i[0] === id);
  return (
    <div className="container page">
      <span className="eyebrow">БАЗА ЗНАНИЙ</span>
      <h1>Документация</h1>
      <p className="intro">От первого проекта до параметрической модели.</p>
      <div className="docs-layout">
        <aside aria-label="Разделы документации">
          {docGroups.map((g) => (
            <div key={g.title}>
              <h2>{g.title}</h2>
              {g.items.map(([key, label]) => (
                <Link
                  aria-current={id === key ? "page" : undefined}
                  key={key}
                  to={`/docs?topic=${key}`}
                >
                  {label}
                </Link>
              ))}
            </div>
          ))}
        </aside>
        <article className="doc-content" key={id}>
          <span className="badge">РУКОВОДСТВО ПОЛЬЗОВАТЕЛЯ</span>
          <h2>{topic?.[1] || "Раздел не найден"}</h2>
          <p className="doc-lead">
            {topic
              ? "Документация готовится"
              : "Выберите раздел в меню документации."}
          </p>
          <p>
            Руководство будет дополняться по мере подготовки MVP. Проверенные
            сведения об архитектуре, сборке и текущих возможностях доступны в
            репозитории.
          </p>
          <div className="doc-callout">
            <h3>Исходные материалы</h3>
            <a href={`${githubUrl}#readme`}>
              README — состояние проекта и сборка ↗
            </a>
            <a href={`${githubUrl}/blob/main/docs/persistent-topology.md`}>
              Топологические ссылки и ограничения ↗
            </a>
            <a href={`${githubUrl}/blob/main/docs/eskd-profile.md`}>
              Чертежи: реализованный профиль ЕСКД ↗
            </a>
          </div>
        </article>
      </div>
    </div>
  );
}
