import { ReleaseCard } from "../components/ReleaseCard";
import { currentRelease, releasesUrl, githubUrl } from "../data/release";
export default function DownloadPage() {
  return (
    <div className="container page">
      <span className="eyebrow">НАЧНИТЕ ПРОЕКТИРОВАТЬ</span>
      <h1>Скачать SolidarCAD</h1>
      <p className="intro">
        Ранний прототип параметрической CAD-системы.
        <br />
        MVP станет первым шагом к знакомству с продуктом.
      </p>
      <div className="download-grid">
        <ReleaseCard />
        <div className="download-details">
          <section>
            <h2>Перед установкой</h2>
            <p>
              Планируемая платформа MVP — Windows x64. Поддерживаемые версии
              Windows, требования к памяти и графике будут опубликованы после
              проверки дистрибутива.
            </p>
            <p>
              Репозиторий содержит исходный код и инструкции сборки для Windows
              и Ubuntu.
            </p>
            <a className="text-link" href={`${githubUrl}#prerequisites`}>
              Сборка из исходников ↗
            </a>
          </section>
          <section>
            <h2>Что нового</h2>
            {currentRelease.notes.length ? (
              <ul>
                {currentRelease.notes.map((n) => (
                  <li key={n}>{n}</li>
                ))}
              </ul>
            ) : (
              <p>
                Release notes появятся вместе с первым опубликованным релизом.
              </p>
            )}
          </section>
          <section>
            <h2>Предыдущие версии</h2>
            <p>
              Опубликованные сборки и их примечания хранятся в GitHub Releases.
            </p>
            <a className="text-link" href={releasesUrl}>
              Архив релизов ↗
            </a>
          </section>
        </div>
      </div>
    </div>
  );
}
