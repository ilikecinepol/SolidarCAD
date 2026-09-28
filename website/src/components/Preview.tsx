import { useState } from "react";
export function Preview() {
  const screenshots = import.meta.glob("/public/screenshots/interface.png", {
    eager: true,
    query: "?url",
    import: "default",
  });
  const [missing, setMissing] = useState(!Object.keys(screenshots).length);
  return (
    <figure className="preview">
      <div className="preview-bar">
        <span>SolidarCAD</span>
        <span>ПАРАМЕТРИЧЕСКОЕ ПРОЕКТИРОВАНИЕ</span>
        <span aria-hidden="true">— &nbsp; □ &nbsp; ×</span>
      </div>
      <div className="preview-body">
        {!missing ? (
          <img
            className="screenshot"
            src="/screenshots/interface.png"
            alt="Интерфейс SolidarCAD"
            onError={() => setMissing(true)}
          />
        ) : (
          <div className="preview-placeholder">
            <span className="crosshair" aria-hidden="true">
              +
            </span>
            <span className="eyebrow">МЕСТО ДЛЯ СНИМКА ПРИЛОЖЕНИЯ</span>
            <h2>SolidarCAD interface preview</h2>
            <p>Эскиз. Объём. История построения.</p>
            <span className="preview-note">
              Изображение интерфейса будет добавлено перед выпуском MVP.
            </span>
            <span className="axis" aria-hidden="true">
              Y ↑<br />
              └──→ X
            </span>
          </div>
        )}
      </div>
      <figcaption>
        <span>01 / Рабочее пространство</span>
        <span>Локальная CAD-система</span>
      </figcaption>
    </figure>
  );
}
