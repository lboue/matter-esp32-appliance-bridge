import { useCallback, useEffect, useRef, useState } from "react";
import {
  enrollWithRelay,
  errorMessage,
  forgetRelay,
  getStConfig,
  getStLinkUrl,
  getStatus,
  setStConfig,
  unlinkSmartThings,
  type StConfig,
  type StState,
} from "../api.ts";
import { useBridgeEvents } from "../events.ts";

const STATE_LABEL: Record<StState, string> = {
  unconfigured: "Not configured",
  unenrolled: "Not enrolled",
  unlinked: "Not linked",
  linked: "Linked",
};

export function SmartThingsPage() {
  const [config, setConfig] = useState<StConfig | null>(null);
  const [state, setState] = useState<StState>("unconfigured");
  const [relayConnected, setRelayConnected] = useState(false);
  const [relayError, setRelayError] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const [relayUrl, setRelayUrl] = useState("");
  const [enrollToken, setEnrollToken] = useState("");
  const formLoaded = useRef(false);

  const load = useCallback(async () => {
    try {
      const [cfg, status] = await Promise.all([getStConfig(), getStatus()]);
      setConfig(cfg);
      setState(status.smartthings.state);
      setRelayConnected(status.smartthings.relayConnected);
      setRelayError(status.smartthings.error);
      if (!formLoaded.current) {
        setRelayUrl(cfg.relayUrl);
        formLoaded.current = true;
      }
      setError(null);
    } catch (e) {
      setError(errorMessage(e));
    }
  }, []);

  useEffect(() => {
    load();
  }, [load]);

  useBridgeEvents((e) => {
    if (e.type === "smartthings") load();
  });

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
    const url = relayUrl.trim().replace(/\/+$/, "");
    if (config && url !== config.relayUrl && config.bridgeId &&
        !confirm("Changing the relay URL forgets this bridge's enrolment with the current relay. Continue?")) {
      return;
    }
    run(async () => {
      await setStConfig(url, enrollToken === "" ? undefined : enrollToken);
      setEnrollToken("");
    });
  };

  // Open the tab synchronously, inside the click, so pop-up blockers allow it;
  // then point it at the one-time link once the bridge has fetched it.
  const onLink = () => {
    const tab = window.open("about:blank", "_blank");
    run(async () => {
      try {
        const { url } = await getStLinkUrl();
        if (tab) tab.location.href = url;
        else window.location.href = url;
      } catch (e) {
        tab?.close();
        throw e;
      }
    });
  };

  const onUnlink = () => {
    if (!confirm("Unlink SmartThings? Bridged washers stop updating until you link again.")) return;
    run(unlinkSmartThings);
  };

  const onForget = () => {
    if (!confirm("Remove this bridge from the relay? You will need to enrol and link again.")) return;
    run(forgetRelay);
  };

  if (!config) {
    return error ? <div className="banner error">{error}</div> : <p className="muted">Loading…</p>;
  }

  const pillClass = state === "linked" ? (relayConnected ? "ok" : "warn") : "off";

  return (
    <>
      {error && <div className="banner error">{error}</div>}

      <section className="card">
        <div className="card-header">
          <h2 style={{ margin: 0 }}>SmartThings</h2>
          <span className={`pill ${pillClass}`}>
            {STATE_LABEL[state]}
            {state !== "unconfigured" && state !== "unenrolled" && (relayConnected ? " · relay connected" : " · relay offline")}
          </span>
        </div>

        {relayError && <div className="banner error">{relayError}</div>}

        {state === "unconfigured" && (
          <p className="muted">
            SmartThings needs a small cloud service, st-relay, to sign in and to deliver live events. Enter the URL of
            your instance below. The project's <span className="mono">cloud/README.md</span> explains how to deploy one.
          </p>
        )}

        {state === "unenrolled" && (
          <>
            <p className="muted">Register this bridge with the relay to get its own credentials.</p>
            <div className="actions">
              <button className="primary" onClick={() => run(enrollWithRelay)} disabled={busy}>
                {busy ? "Enrolling…" : "Enrol with relay"}
              </button>
            </div>
          </>
        )}

        {state === "unlinked" && (
          <>
            <p className="muted">
              Sign in to SmartThings in a new tab and allow access. This page updates when you're done.
            </p>
            <div className="actions">
              <button className="primary" onClick={onLink} disabled={busy || !relayConnected}>
                Connect SmartThings
              </button>
              <button onClick={onForget} disabled={busy}>
                Leave relay
              </button>
            </div>
            {!relayConnected && <p className="hint">Waiting for the relay connection…</p>}
          </>
        )}

        {state === "linked" && (
          <>
            <p className="muted" style={{ marginTop: 0 }}>
              SmartThings is linked. Bridge your washer from the Appliances tab.
            </p>
            <div className="actions">
              <button onClick={onUnlink} disabled={busy}>
                Unlink SmartThings
              </button>
              <button onClick={onForget} disabled={busy}>
                Leave relay
              </button>
            </div>
          </>
        )}

        {config.bridgeId && (
          <p className="hint">
            Bridge ID <span className="mono">{config.bridgeId}</span>
          </p>
        )}
      </section>

      <section className="card">
        <h2>Relay</h2>
        <form onSubmit={onSave}>
          <label htmlFor="relay-url">Relay URL</label>
          <input
            id="relay-url"
            type="text"
            className="mono"
            value={relayUrl}
            onChange={(e) => setRelayUrl(e.target.value)}
            placeholder="https://relay.example.com"
            autoComplete="off"
            spellCheck={false}
            required
          />

          <label htmlFor="enroll-token">Enrolment token (optional)</label>
          <input
            id="enroll-token"
            type="password"
            value={enrollToken}
            onChange={(e) => setEnrollToken(e.target.value)}
            placeholder={config.hasEnrollToken ? "Stored — leave blank to keep" : "Only if the relay sets ENROLL_TOKEN"}
            autoComplete="off"
          />

          <div className="actions">
            <button type="submit" className="primary" disabled={busy || relayUrl.trim() === ""}>
              Save
            </button>
          </div>
        </form>
      </section>
    </>
  );
}
