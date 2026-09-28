import { useState } from "react";
import { Link, NavLink, Outlet } from "react-router-dom";
import { githubUrl } from "../data/release";
export function Layout() {
  const [open, setOpen] = useState(false);
  return (
    <>
      <a className="skip" href="#main">
        К содержимому
      </a>
      <header className="header">
        <div className="container header-inner">
          <Link to="/" className="brand" aria-label="SolidarCAD — главная">
            <span className="brand-icon">
              <img src="/logo.png" alt="" />
            </span>
            SolidarCAD
          </Link>
          <button
            className="menu-toggle"
            aria-expanded={open}
            aria-controls="navigation"
            onClick={() => setOpen(!open)}
          >
            {open ? "Закрыть" : "Меню"}{" "}
            <span aria-hidden="true">{open ? "×" : "☰"}</span>
          </button>
          <nav
            id="navigation"
            className={open ? "nav open" : "nav"}
            aria-label="Основная навигация"
            onClick={() => setOpen(false)}
          >
            <Link to="/#features">Возможности</Link>
            <NavLink to="/download">Скачать</NavLink>
            <NavLink to="/docs">Документация</NavLink>
            <NavLink to="/roadmap">Roadmap</NavLink>
            <NavLink to="/about">О проекте</NavLink>
            <a href={githubUrl}>GitHub ↗</a>
          </nav>
          <Link className="button small header-cta" to="/download">
            Скачать MVP <span aria-hidden="true">↓</span>
          </Link>
        </div>
      </header>
      <main id="main" tabIndex={-1}>
        <Outlet />
      </main>
      <footer>
        <div className="container footer-grid">
          <div>
            <Link className="brand" to="/">
              SolidarCAD
            </Link>
            <p>
              Современная система
              <br />
              автоматизированного проектирования.
            </p>
          </div>
          <div>
            <span className="eyebrow">ПРОДУКТ</span>
            <Link to="/download">Скачать</Link>
            <Link to="/roadmap">Roadmap</Link>
          </div>
          <div>
            <span className="eyebrow">РЕСУРСЫ</span>
            <Link to="/docs">Документация</Link>
            <a href={githubUrl}>GitHub ↗</a>
          </div>
          <div>
            <span className="eyebrow">ПРОЕКТ</span>
            <Link to="/about">О SolidarCAD</Link>
            <span className="muted">© SolidarCAD</span>
          </div>
        </div>
      </footer>
    </>
  );
}
