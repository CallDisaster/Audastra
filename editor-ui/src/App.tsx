import { lazy, Suspense } from "react";
import Workbench from "./workbench/Workbench";

const LegacyStudio = lazy(() => import("./studio/Studio"));

export default function App() {
  if (new URLSearchParams(location.search).has('r01')) {
    return <Suspense fallback={<div className="studio">正在打开工作台…</div>}><LegacyStudio /></Suspense>;
  }
  return <Workbench />;
}
