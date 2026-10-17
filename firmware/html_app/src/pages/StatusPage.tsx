import { useCallback, useEffect, useState } from "react";
import {
  errorMessage,
  factoryReset,
  getBridgedAppliances,
  getStatus,
  reboot,
  type BridgedAppliance,
  type Status,
} from "../api.ts";
import { useBridgeEvents } from "../events.ts";
import type { Tab } from "../App.tsx";

const ST_LABEL: Record<string, string> = {
  unconfigured: "Not configured",
  unenrolled: "Not enrolled",
  unlinked: "Not linked",
  linked: "Linked",
};

const AUTH_LABEL: Record<string, string> = {
  unconfigured: "Not configured",
  signed_out: "Signed out",
  pending: "Waiting for sign-in",
  authorized: "Signed in",
};

function fmtUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return d > 0 ? `${d}d ${h}h` : h > 0 ? `${h}h ${m}m` : `${m}m`;
}

export function StatusPage({ onNavigate }: { onNavigate: (tab: Tab) => void }) {
  const [status, setStatus] = useState<Status | null>(null);
  const [appliances, setAppliances] = useState<BridgedAppliance[]>([]);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(() => {
    Promise.all([getStatus(), getBridgedAppliances()])
      .then(([s, a]) => {
        setStatus(s);
        setAppliances(a);
        setError(null);
      })
      .catch((e) => setError(errorMessage(e)));
  }, []);

  useEffect(() => {
    load();
    const timer = setInterval(load, 15000);
    return () => clearInterval(timer);
  }, [load]);

  useBridgeEvents((e) => {
    if (e.type !== "appliance") load();
  });

  const onReboot = async () => {
    if (!confirm("Reboot the bridge?")) return;
    await reboot().catch((e) => setError(errorMessage(e)));
  };

  const onFactoryReset = async () => {
    if (
      !confirm(
        "Factory reset? This removes every Matter fabric, the HomeConnect sign-in and all bridged appliances. The bridge will reboot.",
      )
    )
      return;
    await factoryReset().catch((e) => setError(errorMessage(e)));
  };

  if (!status) {
    return error ? <div className="banner error">{error}</div> : <p className="muted">Loading…</p>;
  }

  const hc = status.homeconnect;
  const signedIn = hc.state === "authorized";
  const st = status.smartthings;
  const stLinked = st.state === "linked";

  return (
    <>
      {error && <div className="banner error">{error}</div>}

      <div className="grid">
        <section className="card">
          <div className="card-header">
            <h2>HomeConnect</h2>
            <span className={`pill ${signedIn ? "ok" : hc.state === "pending" ? "warn" : "off"}`}>
              {AUTH_LABEL[hc.state] ?? hc.state}
            </span>
          </div>
          <dl className="kv">
            <dt>API</dt>
            <dd>{hc.simulator ? "Simulator" : "Production"}</dd>
            <dt>Live events</dt>
            <dd>{hc.streamConnected ? "Connected" : signedIn ? "Reconnecting…" : "—"}</dd>
          </dl>
          {hc.error && <p className="hint" style={{ color: "var(--danger)" }}>{hc.error}</p>}
          {!signedIn && (
            <div className="actions">
              <button className="primary" onClick={() => onNavigate("homeconnect")}>
                Set up HomeConnect
              </button>
            </div>
          )}
        </section>

        <section className="card">
          <div className="card-header">
            <h2>SmartThings</h2>
            <span className={`pill ${stLinked ? (st.relayConnected ? "ok" : "warn") : "off"}`}>
              {ST_LABEL[st.state] ?? st.state}
            </span>
          </div>
          <dl className="kv">
            <dt>Relay</dt>
            <dd>{st.relayConnected ? "Connected" : st.state === "unconfigured" || st.state === "unenrolled" ? "—" : "Reconnecting…"}</dd>
          </dl>
          {st.error && <p className="hint" style={{ color: "var(--danger)" }}>{st.error}</p>}
          {!stLinked && (
            <div className="actions">
              <button className="primary" onClick={() => onNavigate("smartthings")}>
                Set up SmartThings
              </button>
            </div>
          )}
        </section>

        <section className="card">
          <div className="card-header">
            <h2>Matter</h2>
            <span className={`pill ${status.matter.fabrics > 0 ? "ok" : "off"}`}>
              {status.matter.fabrics > 0 ? "Commissioned" : "Not commissioned"}
            </span>
          </div>
          <dl className="kv">
            <dt>Fabrics</dt>
            <dd>{status.matter.fabrics}</dd>
            <dt>Bridged</dt>
            <dd>
              {appliances.length} appliance{appliances.length === 1 ? "" : "s"}
            </dd>
          </dl>
          {status.matter.fabrics === 0 && (
            <div className="actions">
              <button className="primary" onClick={() => onNavigate("commissioning")}>
                Commission
              </button>
            </div>
          )}
        </section>

        <section className="card">
          <div className="card-header">
            <h2>System</h2>
          </div>
          <dl className="kv">
            <dt>IPv4</dt>
            <dd className="mono">{status.network.ipv4 ?? "—"}</dd>
            <dt>Uptime</dt>
            <dd>{fmtUptime(status.system.uptimeSeconds)}</dd>
            <dt>Free heap</dt>
            <dd>
              {Math.round(status.system.freeHeap / 1024)} KB{" "}
              <span className="muted">(min {Math.round(status.system.minFreeHeap / 1024)} KB)</span>
            </dd>
          </dl>
        </section>
      </div>

      {appliances.length > 0 && (
        <section className="card">
          <div className="card-header">
            <h2>Bridged appliances</h2>
            <button onClick={() => onNavigate("appliances")}>Details</button>
          </div>
          {appliances.map((a) => (
            <div className="list-row" key={`${a.source}/${a.id}`}>
              <div className="grow">
                <strong>{a.name || a.type}</strong>{" "}
                <span className="muted">
                  {a.brand} {a.model}
                </span>
              </div>
              <span className="muted">
                {!a.connected ? "Offline" : a.kind === "washer" ? a.machineState || "—" : a.operationState}
              </span>
              <span className="mono muted">EP {a.endpointId}</span>
            </div>
          ))}
        </section>
      )}

      <section className="card">
        <h2>Maintenance</h2>
        <div className="actions">
          <button onClick={onReboot}>Reboot</button>
          <button className="danger" onClick={onFactoryReset}>
            Factory reset
          </button>
        </div>
      </section>
    </>
  );
}
