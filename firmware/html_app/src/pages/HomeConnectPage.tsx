import { useCallback, useEffect, useRef, useState } from "react";
import QRCode from "react-qr-code";
import {
  errorMessage,
  getHcConfig,
  getSignIn,
  getStatus,
  setHcConfig,
  signOut,
  startSignIn,
  type AuthState,
  type DeviceFlow,
  type HcConfig,
} from "../api.ts";
import { useBridgeEvents } from "../events.ts";

function fmtCountdown(seconds: number): string {
  const m = Math.floor(seconds / 60);
  const s = String(seconds % 60).padStart(2, "0");
  return `${m}:${s}`;
}

export function HomeConnectPage() {
  const [config, setConfig] = useState<HcConfig | null>(null);
  const [authState, setAuthState] = useState<AuthState>("unconfigured");
  const [authError, setAuthError] = useState("");
  const [flow, setFlow] = useState<DeviceFlow | null>(null);
  const [remaining, setRemaining] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  // Form fields
  const [clientId, setClientId] = useState("");
  const [clientSecret, setClientSecret] = useState("");
  const [simulator, setSimulator] = useState(false);
  const formLoaded = useRef(false);

  const load = useCallback(async () => {
    try {
      const [cfg, status] = await Promise.all([getHcConfig(), getStatus()]);
      setConfig(cfg);
      setAuthState(status.homeconnect.state);
      setAuthError(status.homeconnect.error);
      if (!formLoaded.current) {
        setClientId(cfg.clientId);
        setSimulator(cfg.simulator);
        formLoaded.current = true;
      }
      const pending = status.homeconnect.state === "pending" ? await getSignIn() : null;
      setFlow(pending);
      if (pending) setRemaining(pending.expiresIn);
      setError(null);
    } catch (e) {
      setError(errorMessage(e));
    }
  }, []);

  useEffect(() => {
    load();
  }, [load]);

  useBridgeEvents((e) => {
    if (e.type === "auth" || e.type === "stream") load();
  });

  // Sign-in code countdown
  useEffect(() => {
    if (!flow) return;
    const timer = setInterval(() => setRemaining((r) => (r > 0 ? r - 1 : 0)), 1000);
    return () => clearInterval(timer);
  }, [flow]);

  const run = async (action: () => Promise<unknown>) => {
    setBusy(true);
    try {
      await action();
      setError(null);
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setBusy(false);
      load();
    }
  };

  const onSave = (e: React.FormEvent) => {
    e.preventDefault();
    const changesIdentity = config !== null && (clientId !== config.clientId || simulator !== config.simulator);
    if (
      changesIdentity &&
      authState === "authorized" &&
      !confirm("Changing the client ID or API signs you out of HomeConnect. Continue?")
    ) {
      return;
    }
    run(async () => {
      await setHcConfig(clientId.trim(), simulator, clientSecret === "" ? undefined : clientSecret);
      setClientSecret("");
    });
  };

  const onSignIn = () =>
    run(async () => {
      const f = await startSignIn();
      setFlow(f);
      setRemaining(f.expiresIn);
    });

  const onSignOut = () => {
    if (authState === "authorized" && !confirm("Sign out of HomeConnect? Bridged appliances stop updating.")) return;
    run(signOut);
  };

  if (!config) {
    return error ? <div className="banner error">{error}</div> : <p className="muted">Loading…</p>;
  }

  return (
    <>
      {error && <div className="banner error">{error}</div>}

      <section className="card">
        <div className="card-header">
          <h2>Account</h2>
          <span
            className={`pill ${authState === "authorized" ? "ok" : authState === "pending" ? "warn" : "off"}`}
          >
            {authState === "authorized"
              ? "Signed in"
              : authState === "pending"
                ? "Waiting for approval"
                : authState === "signed_out"
                  ? "Signed out"
                  : "Not configured"}
          </span>
        </div>

        {authError && <div className="banner error">{authError}</div>}

        {authState === "unconfigured" && (
          <p className="muted">Enter your HomeConnect client ID below, then sign in.</p>
        )}

        {authState === "signed_out" && (
          <>
            <p className="muted">
              Sign in to let the bridge read and control the appliances on your HomeConnect account.
            </p>
            <div className="actions">
              <button className="primary" onClick={onSignIn} disabled={busy}>
                {busy ? "Starting…" : "Sign in with HomeConnect"}
              </button>
            </div>
          </>
        )}

        {authState === "pending" && flow && (
          <div style={{ display: "flex", gap: "1.5rem", flexWrap: "wrap", alignItems: "center" }}>
            <div className="qr">
              <QRCode value={flow.verificationUriComplete} size={168} />
            </div>
            <div style={{ flex: "1 1 16rem" }}>
              <p style={{ marginTop: 0 }}>
                Scan the code, or open{" "}
                <a href={flow.verificationUriComplete} target="_blank" rel="noreferrer">
                  {flow.verificationUri}
                </a>{" "}
                and enter:
              </p>
              <div className="user-code">{flow.userCode}</div>
              <p className="hint">
                {remaining > 0
                  ? `Code expires in ${fmtCountdown(remaining)}. This page updates once you approve.`
                  : "The code has expired."}
              </p>
              <div className="actions">
                <button onClick={onSignOut} disabled={busy}>
                  Cancel
                </button>
                {remaining === 0 && (
                  <button className="primary" onClick={onSignIn} disabled={busy}>
                    Get a new code
                  </button>
                )}
              </div>
            </div>
          </div>
        )}

        {authState === "authorized" && (
          <>
            <p className="muted" style={{ marginTop: 0 }}>
              The bridge is signed in to the HomeConnect {config.simulator ? "simulator" : "production"} API.
            </p>
            <div className="actions">
              <button onClick={onSignOut} disabled={busy}>
                Sign out
              </button>
            </div>
          </>
        )}
      </section>

      <section className="card">
        <h2>Client registration</h2>
        <p className="hint" style={{ marginTop: 0 }}>
          Register an application at{" "}
          <a href="https://developer.home-connect.com/applications" target="_blank" rel="noreferrer">
            developer.home-connect.com
          </a>{" "}
          with the <strong>Device Flow</strong> OAuth type, then paste its client ID here. Use the simulator
          client ID with the simulator API.
        </p>
        <form onSubmit={onSave}>
          <label htmlFor="client-id">Client ID</label>
          <input
            id="client-id"
            type="text"
            className="mono"
            value={clientId}
            onChange={(e) => setClientId(e.target.value)}
            autoComplete="off"
            spellCheck={false}
            required
          />

          <label htmlFor="client-secret">Client secret (optional)</label>
          <input
            id="client-secret"
            type="password"
            value={clientSecret}
            onChange={(e) => setClientSecret(e.target.value)}
            placeholder={config.hasClientSecret ? "Stored — leave blank to keep" : "Only if your registration needs it"}
            autoComplete="off"
          />
          <p className="hint">Sent only to HomeConnect's token endpoint. It is never shown again.</p>

          <label className="inline">
            <input type="checkbox" checked={simulator} onChange={(e) => setSimulator(e.target.checked)} />
            Use the HomeConnect simulator
          </label>

          <div className="actions">
            <button type="submit" className="primary" disabled={busy || clientId.trim() === ""}>
              Save
            </button>
          </div>
        </form>
      </section>
    </>
  );
}
