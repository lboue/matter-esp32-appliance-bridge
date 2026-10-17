import { useCallback, useEffect, useState } from "react";
import {
  bridgeAppliance,
  errorMessage,
  getBridgedAppliances,
  getHcAppliances,
  getStDevices,
  getStatus,
  refreshBridge,
  unbridgeAppliance,
  type BridgedAppliance,
  type BridgedDishwasher,
  type BridgedWasher,
  type HcAppliance,
  type Source,
  type StDevice,
} from "../api.ts";
import { useBridgeEvents } from "../events.ts";
import type { Tab } from "../App.tsx";

function fmtDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.round((seconds % 3600) / 60);
  return h > 0 ? `${h}h ${String(m).padStart(2, "0")}m` : `${m} min`;
}

function yesNo(value: boolean | undefined): string {
  return value === undefined ? "—" : value ? "Yes" : "No";
}

function capitalise(s: string): string {
  return s ? s[0].toUpperCase() + s.slice(1) : "—";
}

// ── Bridged appliance cards ────────────────────────────────────────────────

function CardHeader({ a, state, pill }: { a: BridgedAppliance; state: string; pill: string }) {
  return (
    <div className="card-header">
      <div>
        <h3>{a.name || a.type}</h3>
        <span className="muted">
          {[a.brand, a.model].filter(Boolean).join(" ")} · {a.source === "homeconnect" ? "HomeConnect" : "SmartThings"}{" "}
          · <span className="mono">endpoint {a.endpointId}</span>
        </span>
      </div>
      <span className={`pill ${a.connected ? pill : "off"}`}>{a.connected ? state : "Offline"}</span>
    </div>
  );
}

function RemoveButton({ onRemove }: { onRemove: () => void }) {
  return (
    <div className="actions">
      <button className="danger" onClick={onRemove}>
        Remove from bridge
      </button>
    </div>
  );
}

function dishwasherPill(a: BridgedDishwasher): string {
  if (a.operationState === "Error") return "danger";
  if (a.operationState === "Run") return "ok";
  if (["Pause", "ActionRequired", "DelayedStart"].includes(a.operationState)) return "warn";
  return "off";
}

function programLabel(a: BridgedDishwasher, key: string): string {
  if (!key) return "—";
  return a.programs.find((p) => p.key === key)?.label ?? key.split(".").pop() ?? key;
}

function DishwasherCard({ appliance: a, onRemove }: { appliance: BridgedDishwasher; onRemove: () => void }) {
  return (
    <section className="card">
      <CardHeader a={a} state={a.operationState} pill={dishwasherPill(a)} />

      {a.lastError && <div className="banner error">{a.lastError}</div>}
      {(a.saltLow || a.rinseAidLow) && (
        <div className="banner info">
          {[a.saltLow && "Salt is nearly empty.", a.rinseAidLow && "Rinse aid is nearly empty."]
            .filter(Boolean)
            .join(" ")}
        </div>
      )}

      <dl className="kv">
        <dt>{a.activeProgram ? "Running" : "Selected"}</dt>
        <dd>{programLabel(a, a.activeProgram || a.selectedProgram)}</dd>
        {a.remainingSeconds !== undefined && (
          <>
            <dt>Remaining</dt>
            <dd>{fmtDuration(a.remainingSeconds)}</dd>
          </>
        )}
        {a.progress !== undefined && (
          <>
            <dt>Progress</dt>
            <dd>
              {a.progress}%
              <div className="progress">
                <div style={{ width: `${Math.min(100, Math.max(0, a.progress))}%` }} />
              </div>
            </dd>
          </>
        )}
        <dt>Door</dt>
        <dd>{a.doorState || "—"}</dd>
        <dt>Power</dt>
        <dd>{a.powerState || "—"}</dd>
        <dt>Remote control</dt>
        <dd>{yesNo(a.remoteControlActive)}</dd>
        <dt>Remote start</dt>
        <dd>
          {yesNo(a.remoteStartAllowed)}
          {a.remoteStartAllowed === false && (
            <span className="hint"> — enable Remote Start on the appliance to start it from Matter</span>
          )}
        </dd>
        <dt>Matter modes</dt>
        <dd>
          {a.programs.map((p) => (
            <span
              key={p.mode}
              className="mono"
              style={{ marginRight: "0.75rem", fontWeight: p.mode === a.currentMode ? 700 : 400 }}
              title={p.key}
            >
              {p.mode}: {p.label}
            </span>
          ))}
        </dd>
        <dt>HomeConnect ID</dt>
        <dd className="mono">{a.id}</dd>
      </dl>
      <RemoveButton onRemove={onRemove} />
    </section>
  );
}

function washerPill(a: BridgedWasher): string {
  if (a.machineState === "run") return "ok";
  if (a.machineState === "pause") return "warn";
  return "off";
}

function WasherCard({ appliance: a, onRemove }: { appliance: BridgedWasher; onRemove: () => void }) {
  const running = a.machineState === "run" || a.machineState === "pause";
  return (
    <section className="card">
      <CardHeader a={a} state={capitalise(a.machineState)} pill={washerPill(a)} />

      {a.lastError && <div className="banner error">{a.lastError}</div>}

      <dl className="kv">
        <dt>Cycle</dt>
        <dd>
          {a.cycle || "—"}
          {a.mode && <span className="muted"> (Matter mode: {a.mode})</span>}
        </dd>
        {running && (
          <>
            <dt>Phase</dt>
            <dd>{a.jobState || "—"}</dd>
          </>
        )}
        {a.remainingSeconds !== undefined && (
          <>
            <dt>Remaining</dt>
            <dd>{fmtDuration(a.remainingSeconds)}</dd>
          </>
        )}
        <dt>Door</dt>
        <dd>{capitalise(a.doorState)}</dd>
        <dt>Power</dt>
        <dd>{capitalise(a.switch)}</dd>
        <dt>Remote control</dt>
        <dd>
          {yesNo(a.remoteControlEnabled)}
          {a.remoteControlEnabled === false && (
            <span className="hint"> — enable Smart Control on the washer to start or stop it from Matter</span>
          )}
        </dd>
        <dt>SmartThings ID</dt>
        <dd className="mono">{a.id}</dd>
      </dl>
      <RemoveButton onRemove={onRemove} />
    </section>
  );
}

// ── Account appliance lists ────────────────────────────────────────────────

interface AccountItem {
  id: string;
  name: string;
  detail: string;
  online?: boolean;
  supported: boolean;
  bridged: boolean;
}

function AccountSection(props: {
  title: string;
  ready: boolean | null;
  notReadyText: string;
  setupTab: Tab;
  items: AccountItem[] | null;
  error: string | null;
  busyId: string | null;
  onReload: () => void;
  onBridge: (id: string) => void;
  onNavigate: (tab: Tab) => void;
}) {
  const { ready, items, error, busyId } = props;
  return (
    <section className="card">
      <div className="card-header">
        <h2 style={{ margin: 0 }}>{props.title}</h2>
        {ready && <button onClick={props.onReload}>Reload</button>}
      </div>

      {ready === false && (
        <>
          <p className="muted">{props.notReadyText}</p>
          <button className="primary" onClick={() => props.onNavigate(props.setupTab)}>
            Set up
          </button>
        </>
      )}

      {error && <div className="banner error">{error}</div>}
      {ready && items === null && !error && <p className="muted">Loading…</p>}
      {items !== null && items.length === 0 && <p className="muted">No appliances found on this account.</p>}

      {items?.map((item) => (
        <div className="list-row" key={item.id}>
          <div className="grow">
            <strong>{item.name}</strong>
            <div className="muted">{item.detail}</div>
          </div>
          {item.bridged ? (
            <span className="pill ok">Bridged</span>
          ) : !item.supported ? (
            <span className="pill off">Not supported yet</span>
          ) : (
            <button
              className="primary"
              onClick={() => props.onBridge(item.id)}
              disabled={busyId !== null || item.online === false}
              title={item.online === false ? "The appliance must be online" : undefined}
            >
              {busyId === item.id ? "Bridging…" : "Bridge"}
            </button>
          )}
        </div>
      ))}
    </section>
  );
}

const hcItem = (a: HcAppliance): AccountItem => ({
  id: a.haId,
  name: a.name || a.type,
  detail: `${a.brand} ${a.vib} · ${a.type} · ${a.connected ? "online" : "offline"}`,
  online: a.connected,
  supported: a.supported,
  bridged: a.bridged,
});

const stItem = (d: StDevice): AccountItem => ({
  id: d.id,
  name: d.name,
  detail: [d.brand, d.model, d.type].filter(Boolean).join(" · "),
  supported: d.supported,
  bridged: d.bridged,
});

// ── Page ───────────────────────────────────────────────────────────────────

export function AppliancesPage({ onNavigate }: { onNavigate: (tab: Tab) => void }) {
  const [bridged, setBridged] = useState<BridgedAppliance[]>([]);
  const [hcReady, setHcReady] = useState<boolean | null>(null);
  const [stReady, setStReady] = useState<boolean | null>(null);
  const [hcItems, setHcItems] = useState<AccountItem[] | null>(null);
  const [stItems, setStItems] = useState<AccountItem[] | null>(null);
  const [hcError, setHcError] = useState<string | null>(null);
  const [stError, setStError] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);

  const loadBridged = useCallback(() => {
    getBridgedAppliances()
      .then(setBridged)
      .catch((e) => setError(errorMessage(e)));
  }, []);

  // Listing an account's appliances costs a cloud API call, so it is only
  // fetched on demand and when the account changes.
  const loadHc = useCallback(() => {
    setHcError(null);
    getHcAppliances()
      .then((list) => setHcItems(list.map(hcItem)))
      .catch((e) => setHcError(errorMessage(e)));
  }, []);

  const loadSt = useCallback(() => {
    setStError(null);
    getStDevices()
      .then((list) => setStItems(list.map(stItem)))
      .catch((e) => setStError(errorMessage(e)));
  }, []);

  useEffect(() => {
    loadBridged();
    getStatus()
      .then((s) => {
        const hc = s.homeconnect.state === "authorized";
        const st = s.smartthings.state === "linked";
        setHcReady(hc);
        setStReady(st);
        if (hc) loadHc();
        if (st) loadSt();
      })
      .catch((e) => setError(errorMessage(e)));
  }, [loadBridged, loadHc, loadSt]);

  useBridgeEvents((e) => {
    if (e.type === "appliance" || e.type === "bridge_changed") loadBridged();
    if (e.type === "appliances_changed" && hcReady) loadHc();
  });

  const reloadAccounts = () => {
    if (hcReady) loadHc();
    if (stReady) loadSt();
  };

  const onBridge = async (source: Source, id: string) => {
    setBusyId(id);
    try {
      await bridgeAppliance(source, id);
      setError(null);
      loadBridged();
      reloadAccounts();
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setBusyId(null);
    }
  };

  const onRemove = async (a: BridgedAppliance) => {
    if (!confirm(`Remove ${a.name || a.type} from the bridge? Matter controllers will lose the device.`)) return;
    try {
      await unbridgeAppliance(a.source, a.id);
      setError(null);
      loadBridged();
      reloadAccounts();
    } catch (e) {
      setError(errorMessage(e));
    }
  };

  return (
    <>
      {error && <div className="banner error">{error}</div>}

      <div className="card-header">
        <h2 style={{ margin: 0 }}>Bridged appliances</h2>
        {bridged.length > 0 && (
          <button onClick={() => refreshBridge().catch((e) => setError(errorMessage(e)))}>Refresh</button>
        )}
      </div>

      {bridged.length === 0 ? (
        <div className="empty" style={{ marginBottom: "1.5rem" }}>
          No appliances bridged yet. Pick one from your accounts below.
        </div>
      ) : (
        bridged.map((a) =>
          a.kind === "washer" ? (
            <WasherCard key={`${a.source}/${a.id}`} appliance={a} onRemove={() => onRemove(a)} />
          ) : (
            <DishwasherCard key={`${a.source}/${a.id}`} appliance={a} onRemove={() => onRemove(a)} />
          ),
        )
      )}

      <AccountSection
        title="HomeConnect"
        ready={hcReady}
        notReadyText="Sign in to HomeConnect to see your Bosch, Siemens or Neff appliances."
        setupTab="homeconnect"
        items={hcItems}
        error={hcError}
        busyId={busyId}
        onReload={loadHc}
        onBridge={(id) => onBridge("homeconnect", id)}
        onNavigate={onNavigate}
      />

      <AccountSection
        title="SmartThings"
        ready={stReady}
        notReadyText="Link SmartThings to see your Samsung appliances."
        setupTab="smartthings"
        items={stItems}
        error={stError}
        busyId={busyId}
        onReload={loadSt}
        onBridge={(id) => onBridge("smartthings", id)}
        onNavigate={onNavigate}
      />
    </>
  );
}
