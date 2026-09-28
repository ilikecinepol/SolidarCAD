import { Link } from "react-router-dom";
import { Preview } from "../components/Preview";
import { features } from "../data/features";
export default function HomePage() {
  return (
    <>
      <section className="hero container">
        <div className="hero-topline">
          <span className="eyebrow">ИНЖЕНЕРНАЯ МЫСЛЬ. ТОЧНАЯ ФОРМА.</span>
          <span className="badge">РАННИЙ ПРОТОТИП / MVP</span>
        </div>
        <h1>
          Solidar<span>CAD</span>
          <span className="title-dot">.</span>
        </h1>
        <div className="hero-bottom">
          <div>
            <h2>
              Современная система
              <br />
              автоматизированного проектирования
            </h2>
            <p>
              Параметрическое 2D- и 3D-проектирование.
              <br />
              Локально. Без обязательного облака.
            </p>
          </div>
          <div className="hero-actions">
            <Link className="button" to="/download">
              Скачать MVP <span>↓</span>
            </Link>
            <a className="button secondary" href="#features">
              Возможности <span>↗</span>
            </a>
            <span className="meta">Windows x64 · сборка готовится</span>
          </div>
        </div>
        <Preview />
      </section>
      <section id="features" className="section container">
        <div className="section-heading">
          <div>
            <span className="eyebrow">ОТ ЗАМЫСЛА К МОДЕЛИ</span>
            <h2>Возможности SolidarCAD</h2>
          </div>
          <p>
            Основные инструменты уже в прототипе.
            <br />
            Рабочие процессы продолжают развиваться.
          </p>
        </div>
        <div className="feature-grid">
          {features.map((f) => (
            <article className="feature-card" key={f.number}>
              <div className="card-top">
                <span className="feature-number">{f.number}</span>
                <span className="eyebrow">{f.tag}</span>
              </div>
              <h3>{f.title}</h3>
              <p>{f.text}</p>
            </article>
          ))}
        </div>
      </section>
      <section className="container section vision">
        <div>
          <span className="eyebrow">СЛЕДУЮЩИЙ ШАГ</span>
          <h2>
            Инструмент растёт
            <br />
            вместе с задачами.
          </h2>
        </div>
        <div>
          <p>
            Ближайший ориентир — надёжный рабочий процесс: от ограниченного
            эскиза до редактируемой истории детали и публикации MVP.
          </p>
          <p className="muted">
            Расширение обмена форматами и развитие чертежей — в планах. Это
            направления развития, а не обещания текущей сборки.
          </p>
          <Link className="text-link" to="/roadmap">
            Развитие SolidarCAD ↗
          </Link>
        </div>
      </section>
    </>
  );
}
