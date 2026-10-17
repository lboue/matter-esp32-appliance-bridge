import { useEffect, useRef } from "react";

// One shared WebSocket to /api/events. The firmware has room for only a few
// clients, so every component subscribes to this rather than opening its own.

export interface BridgeEvent {
  type: string;
  haId?: string;
  state?: string;
  connected?: boolean;
}

type Listener = (event: BridgeEvent) => void;

const listeners = new Set<Listener>();
let socket: WebSocket | null = null;
let reconnectTimer: ReturnType<typeof setTimeout> | undefined;

function connect() {
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${scheme}://${location.host}/api/events`);
  socket.onmessage = (e) => {
    try {
      const event = JSON.parse(e.data as string) as BridgeEvent;
      listeners.forEach((l) => l(event));
    } catch {
      /* ignore malformed frames */
    }
  };
  socket.onclose = () => {
    socket = null;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = undefined;
      if (listeners.size > 0) connect();
    }, 3000);
  };
  socket.onerror = () => socket?.close();
}

/** Call `handler` for every event pushed by the bridge while mounted. */
export function useBridgeEvents(handler: Listener) {
  const ref = useRef(handler);
  ref.current = handler;

  useEffect(() => {
    const listener: Listener = (e) => ref.current(e);
    listeners.add(listener);
    if (socket === null && reconnectTimer === undefined) connect();
    return () => {
      listeners.delete(listener);
    };
  }, []);
}
