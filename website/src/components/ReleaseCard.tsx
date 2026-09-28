import { currentRelease as release, releasesUrl } from "../data/release";
export function ReleaseCard() {
  const ready = Boolean(release.downloadUrl && release.version);
  return (
    <div className="release-card">
      <div className="release-top">
        <span className="eyebrow">{release.platform}</span>
        <span className="badge">{release.channel}</span>
      </div>
      <h2>SolidarCAD MVP</h2>
      <p>
        {ready
          ? "Сборка для знакомства с SolidarCAD."
          : "Публичная сборка готовится"}
      </p>
      <dl className="release-meta">
        <div>
          <dt>Версия</dt>
          <dd>{release.version || "Не опубликована"}</dd>
        </div>
        <div>
          <dt>Дата выпуска</dt>
          <dd>{release.releaseDate || "Не объявлена"}</dd>
        </div>
        <div>
          <dt>Размер</dt>
          <dd>{release.size || "Будет указан"}</dd>
        </div>
      </dl>
      {ready ? (
        <a className="button" href={release.downloadUrl}>
          Скачать для {release.platform} ↓
        </a>
      ) : (
        <button className="button" disabled>
          Сборка пока недоступна
        </button>
      )}
      <a className="text-link" href={release.releaseUrl || releasesUrl}>
        {ready ? "Открыть GitHub Release" : "Проверить GitHub Releases"} ↗
      </a>
      <div className="checksum">
        <span className="eyebrow">SHA-256</span>
        <code>
          {release.sha256 || "Контрольная сумма появится вместе со сборкой."}
        </code>
      </div>
    </div>
  );
}
