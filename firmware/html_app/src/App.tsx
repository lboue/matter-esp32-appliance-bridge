import { useState } from "react";
import { StatusPage } from "./pages/StatusPage.tsx";
import { HomeConnectPage } from "./pages/HomeConnectPage.tsx";
import { AppliancesPage } from "./pages/AppliancesPage.tsx";
import { CommissioningPage } from "./pages/CommissioningPage.tsx";
import { SmartThingsPage } from "./pages/SmartThingsPage.tsx";

const TABS = [
  { id: "status", label: "Status" },
  { id: "homeconnect", label: "HomeConnect" },
  { id: "smartthings", label: "SmartThings" },
  { id: "appliances", label: "Appliances" },
  { id: "commissioning", label: "Commissioning" },
] as const;

type Tab = (typeof TABS)[number]["id"];

function initialTab(): Tab {
  const hash = location.hash.slice(1);
  return (TABS.find((t) => t.id === hash)?.id ?? "status") as Tab;
}

export default function App() {
  const [tab, setTab] = useState<Tab>(initialTab);

  const select = (id: Tab) => {
    setTab(id);
    history.replaceState(null, "", `#${id}`);
  };

  return (
    <main>
      <header className="app-header">
        <h1>Appliance Bridge</h1>
        <span className="muted">HomeConnect &amp; SmartThings → Matter</span>
      </header>

      <nav className="tabs" role="tablist">
        {TABS.map((t) => (
          <button
            key={t.id}
            role="tab"
            className={tab === t.id ? "active" : ""}
            aria-selected={tab === t.id}
            onClick={() => select(t.id)}
          >
            {t.label}
          </button>
        ))}
      </nav>

      {tab === "status" && <StatusPage onNavigate={select} />}
      {tab === "homeconnect" && <HomeConnectPage />}
      {tab === "smartthings" && <SmartThingsPage />}
      {tab === "appliances" && <AppliancesPage onNavigate={select} />}
      {tab === "commissioning" && <CommissioningPage />}
    </main>
  );
}

export type { Tab };
