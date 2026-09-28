import { useEffect } from "react";
import { Link, Route, Routes, useLocation } from "react-router-dom";
import { Layout } from "./components/Layout";
import HomePage from "./pages/HomePage";
import DownloadPage from "./pages/DownloadPage";
import DocsPage from "./pages/DocsPage";
import RoadmapPage from "./pages/RoadmapPage";
import AboutPage from "./pages/AboutPage";
const titles: Record<string, string> = {
  "/": "система автоматизированного проектирования",
  "/download": "Скачать",
  "/docs": "Документация",
  "/roadmap": "Roadmap",
  "/about": "О проекте",
};
function RouteMeta() {
  const { pathname, hash } = useLocation();
  useEffect(() => {
    document.title = `SolidarCAD — ${titles[pathname] || "Страница не найдена"}`;
    document
      .querySelector('meta[property="og:title"]')
      ?.setAttribute("content", document.title);
    const origin = import.meta.env.VITE_SITE_URL;
    if (origin) {
      let canonical = document.querySelector<HTMLLinkElement>(
        'link[rel="canonical"]',
      );
      if (!canonical) {
        canonical = document.createElement("link");
        canonical.rel = "canonical";
        document.head.append(canonical);
      }
      canonical.href = new URL(pathname, origin).href;
    }
    if (hash) {
      requestAnimationFrame(() =>
        document.getElementById(hash.slice(1))?.scrollIntoView(),
      );
    } else {
      window.scrollTo(0, 0);
      document.getElementById("main")?.focus({ preventScroll: true });
    }
  }, [pathname, hash]);
  return null;
}
export default function App() {
  return (
    <>
      <RouteMeta />
      <Routes>
        <Route element={<Layout />}>
          <Route index element={<HomePage />} />
          <Route path="download" element={<DownloadPage />} />
          <Route path="docs" element={<DocsPage />} />
          <Route path="roadmap" element={<RoadmapPage />} />
          <Route path="about" element={<AboutPage />} />
          <Route
            path="*"
            element={
              <div className="container page">
                <span className="eyebrow">404</span>
                <h1>Страница не найдена</h1>
                <Link className="button" to="/">
                  На главную
                </Link>
              </div>
            }
          />
        </Route>
      </Routes>
    </>
  );
}
