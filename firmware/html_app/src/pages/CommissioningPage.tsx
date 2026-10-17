import { useCallback, useEffect, useRef, useState } from "react";
import QRCode from "react-qr-code";
import {
  errorMessage,
  getPairing,
  openCommissioningWindow,
  removeFabric,
  type Fabric,
  type PairingInfo,
} from "../api.ts";
import { useBridgeEvents } from "../events.ts";

// Same commissioning flow as the other matter-esp32-* adapters: open a basic
// window, then show the QR and manual pairing codes while it is open.

const WINDOW_SECONDS = 180;

// Friendly names for common Matter ecosystem vendor IDs (presentation only).
const VENDOR_NAMES: Record<number, string> = {
  0x1349: "Apple",
  0x6006: "Google",
  0x1049: "Samsung SmartThings",
  0x1217: "Amazon",
  0x100b: "Home Assistant",
};

function vendorHex(vendorId: number): string {
  return `0x${vendorId.toString(16).toUpperCase().padStart(4, "0")}`;
}

function vendorLabel(f: Fabric): string {
  if (f.label && f.label.trim()) return f.label.trim();
  return VENDOR_NAMES[f.vendorId] ?? "Matter controller";
}

function fmtManualCode(code: string): string {
  if (code.length === 11) return `${code.slice(0, 4)}-${code.slice(4, 7)}-${code.slice(7)}`;
  if (code.length === 21)
    return `${code.slice(0, 6)}-${code.slice(6, 11)}-${code.slice(11, 16)}-${code.slice(16)}`;
  return code;
}

function fmtCountdown(seconds: number): string {
  const m = Math.floor(seconds / 60);
  const s = String(seconds % 60).padStart(2, "0");
  return `${m}:${s}`;
}

export function CommissioningPage() {
  const [info, setInfo] = useState<PairingInfo | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [opening, setOpening] = useState(false);
  const [countdown, setCountdown] = useState<number | null>(null);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const stopCountdown = () => {
    if (timerRef.current) clearInterval(timerRef.current);
    timerRef.current = null;
    setCountdown(null);
  };

  const startCountdown = (seconds: number) => {
    if (timerRef.current) clearInterval(timerRef.current);
    setCountdown(seconds);
    timerRef.current = setInterval(() => {
      setCountdown((c) => {
        if (c === null || c <= 1) {
          if (timerRef.current) clearInterval(timerRef.current);
          timerRef.current = null;
          return null;
        }
        return c - 1;
      });
    }, 1000);
  };

  const load = useCallback(() => {
    getPairing()
      .then((data) => {
        setInfo(data);
        setError(null);
        if (data.windowOpen) startCountdown(data.windowSecondsRemaining || WINDOW_SECONDS);
        else stopCountdown();
      })
      .catch((e) => setError(errorMessage(e)));
  }, []);

  useEffect(() => {
    load();
    return () => {
      if (timerRef.current) clearInterval(timerRef.current);
    };
  }, [load]);

  useBridgeEvents((e) => {
    if (["window_opened", "window_closed", "commissioned", "fabric_removed"].includes(e.type)) load();
  });

  const onOpenWindow = async () => {
    setOpening(true);
    try {
      await openCommissioningWindow();
      startCountdown(WINDOW_SECONDS);
      // The window opens on the Matter task; fetch the codes once it has.
      setTimeout(load, 500);
    } catch (e) {
      setError(`Failed to open window: ${errorMessage(e)}`);
    } finally {
      setOpening(false);
    }
  };

  const onRemoveFabric = async (f: Fabric) => {
    if (!confirm(`Remove ${vendorLabel(f)} (fabric ${f.fabricIndex})? That controller will lose access to the bridge.`))
      return;
    try {
      await removeFabric(f.fabricIndex);
      load();
    } catch (e) {
      setError(`Failed to remove fabric: ${errorMessage(e)}`);
    }
  };

  if (!info) {
    return error ? <div className="banner error">{error}</div> : <p className="muted">Loading…</p>;
  }

  const windowOpen = countdown !== null;

  return (
    <>
      {error && <div className="banner error">{error}</div>}

      {windowOpen ? (
        <>
          <div className="banner info" style={{ display: "flex", justifyContent: "space-between", flexWrap: "wrap" }}>
            <strong>Commissioning window open</strong>
            <span className="mono">{fmtCountdown(countdown)} remaining</span>
          </div>

          <div style={{ display: "flex", gap: "1rem", flexWrap: "wrap", alignItems: "stretch" }}>
            <section className="card" style={{ flex: "0 0 auto", textAlign: "center" }}>
              <h2>QR code</h2>
              <div className="qr">
                <QRCode value={info.qrCode} size={200} />
              </div>
            </section>

            <div style={{ flex: "1 1 18rem", minWidth: 0 }}>
              <section className="card">
                <h2>Manual pairing code</h2>
                <div className="user-code">{fmtManualCode(info.manualPairingCode)}</div>
                <p className="hint">
                  Scan the QR code or enter this code in Apple Home, Google Home, Home Assistant, SmartThings
                  or Alexa.
                </p>
              </section>
              <section className="card">
                <h2>Setup payload</h2>
                <code className="mono" style={{ overflowWrap: "anywhere" }}>
                  {info.qrCode}
                </code>
              </section>
            </div>
          </div>
        </>
      ) : (
        <section className="card">
          <div className="card-header">
            <div>
              <h2 style={{ margin: 0 }}>Add the bridge to a Matter controller</h2>
              <p className="hint">Open a 3-minute window to show the QR code and manual pairing code.</p>
            </div>
            <button className="primary" onClick={onOpenWindow} disabled={opening}>
              {opening ? "Opening…" : "Open commissioning window"}
            </button>
          </div>
        </section>
      )}

      <section className="card">
        <div className="card-header">
          <h2 style={{ margin: 0 }}>Commissioned fabrics</h2>
          <span className="muted mono">{info.fabrics.length}</span>
        </div>
        {info.fabrics.length === 0 ? (
          <div className="empty">Not commissioned to any fabric yet.</div>
        ) : (
          info.fabrics.map((f) => (
            <div className="list-row" key={f.fabricIndex}>
              <span className="mono muted">#{f.fabricIndex}</span>
              <div className="grow">
                <strong>{vendorLabel(f)}</strong>
                <div className="muted mono" style={{ fontSize: "0.8rem" }}>
                  vendor {vendorHex(f.vendorId)} · node {f.nodeId}
                </div>
              </div>
              <button className="danger" onClick={() => onRemoveFabric(f)}>
                Remove
              </button>
            </div>
          ))
        )}
      </section>
    </>
  );
}
