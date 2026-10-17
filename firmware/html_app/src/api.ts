// Types mirror the JSON the firmware emits in web_server.cpp and
// DishwasherDevice::ToJson. Keep the two in sync: the field names are the wire
// contract.

export type AuthState = "unconfigured" | "signed_out" | "pending" | "authorized";
export type StState = "unconfigured" | "unenrolled" | "unlinked" | "linked";
export type Source = "homeconnect" | "smartthings";

export interface Status {
  network: { ipv4?: string };
  matter: { fabrics: number };
  homeconnect: {
    state: AuthState;
    error: string;
    simulator: boolean;
    streamConnected: boolean;
  };
  smartthings: {
    state: StState;
    relayConnected: boolean;
    error: string;
  };
  system: { uptimeSeconds: number; freeHeap: number; minFreeHeap: number };
}

export interface HcConfig {
  clientId: string;
  simulator: boolean;
  hasClientSecret: boolean;
}

export interface DeviceFlow {
  userCode: string;
  verificationUri: string;
  verificationUriComplete: string;
  expiresIn: number;
}

export interface HcAppliance {
  haId: string;
  name: string;
  brand: string;
  vib: string;
  type: string;
  connected: boolean;
  bridged: boolean;
  supported: boolean;
}

export interface Program {
  mode: number;
  key: string;
  label: string;
}

// Fields every bridged appliance has.
interface BridgedBase {
  source: Source;
  id: string;
  name: string;
  brand: string;
  model: string;
  type: string;
  endpointId: number;
  connected: boolean;
  lastError: string;
}

export interface BridgedDishwasher extends BridgedBase {
  kind: "dishwasher";
  operationState: string;
  doorState: string;
  powerState: string;
  remoteStartAllowed?: boolean;
  remoteControlActive?: boolean;
  remainingSeconds?: number;
  progress?: number;
  activeProgram: string;
  selectedProgram: string;
  saltLow: boolean;
  rinseAidLow: boolean;
  canPause: boolean;
  currentMode?: number;
  programs: Program[];
}

export interface BridgedWasher extends BridgedBase {
  kind: "washer";
  machineState: string;
  jobState: string;
  cycle: string;
  mode: string;
  switch: string;
  doorState: string;
  remoteControlEnabled?: boolean;
  remainingSeconds?: number;
}

export type BridgedAppliance = BridgedDishwasher | BridgedWasher;

export interface StConfig {
  relayUrl: string;
  hasEnrollToken: boolean;
  bridgeId: string;
}

export interface StDevice {
  id: string;
  name: string;
  brand: string;
  model: string;
  type: string;
  supported: boolean;
  bridged: boolean;
}

export interface Fabric {
  fabricIndex: number;
  vendorId: number;
  label: string;
  nodeId: string;
}

export interface PairingInfo {
  qrCode: string;
  manualPairingCode: string;
  windowOpen: boolean;
  windowSecondsRemaining: number;
  fabrics: Fabric[];
}

async function request(path: string, init?: RequestInit): Promise<Response> {
  const res = await fetch(path, {
    headers: { "Content-Type": "application/json" },
    ...init,
  });
  if (!res.ok) {
    // The firmware sends a JSON { error } for failures; fall back to the status text.
    let message = res.statusText;
    try {
      const body = await res.json();
      if (body && typeof body.error === "string") message = body.error;
    } catch {
      /* non-JSON error body */
    }
    throw new Error(message || `HTTP ${res.status}`);
  }
  return res;
}

const getJson = async <T>(path: string): Promise<T> => (await request(path)).json();

const post = async <T = unknown>(path: string, body?: unknown): Promise<T> =>
  (
    await request(path, {
      method: "POST",
      body: body === undefined ? undefined : JSON.stringify(body),
    })
  ).json();

export const getStatus = () => getJson<Status>("/api/status");

export const getHcConfig = () => getJson<HcConfig>("/api/homeconnect/config");

export const setHcConfig = (clientId: string, simulator: boolean, clientSecret?: string) =>
  post("/api/homeconnect/config", { clientId, simulator, clientSecret });

export const startSignIn = () => post<DeviceFlow>("/api/homeconnect/login");

export async function getSignIn(): Promise<DeviceFlow | null> {
  const res = await fetch("/api/homeconnect/login");
  return res.ok ? res.json() : null;
}

export const signOut = () => post("/api/homeconnect/logout");

export const getHcAppliances = () => getJson<HcAppliance[]>("/api/homeconnect/appliances");

export const getBridgedAppliances = () => getJson<BridgedAppliance[]>("/api/bridge/appliances");

export const bridgeAppliance = (source: Source, id: string) => post("/api/bridge/appliances", { source, id });

export async function unbridgeAppliance(source: Source, id: string): Promise<void> {
  await request(`/api/bridge/appliances/${source}/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export const refreshBridge = () => post("/api/bridge/refresh");

export const getStConfig = () => getJson<StConfig>("/api/smartthings/config");

export const setStConfig = (relayUrl: string, enrollToken?: string) =>
  post("/api/smartthings/config", { relayUrl, enrollToken });

export const enrollWithRelay = () => post("/api/smartthings/enroll");

export const getStLinkUrl = () => post<{ url: string }>("/api/smartthings/link");

export const unlinkSmartThings = () => post("/api/smartthings/unlink");

export const forgetRelay = () => post("/api/smartthings/forget");

export const getStDevices = () => getJson<StDevice[]>("/api/smartthings/devices");

export const getPairing = () => getJson<PairingInfo>("/api/matter/pairing");

export const openCommissioningWindow = () => post("/api/matter/open-commissioning-window");

export const removeFabric = (index: number) => post("/api/matter/remove-fabric", { index });

export const reboot = () => post("/api/reboot");

export const factoryReset = () => post("/api/factory-reset");

export function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}
