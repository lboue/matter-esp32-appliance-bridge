# Matter Appliance Bridge

A Matter bridge for cloud-connected home appliances, running on the Waveshare
ESP32-S3-ETH (W5500 Ethernet). Each appliance becomes a Bridged Node that any
Matter controller can see and control.

It currently supports:

- **HomeConnect dishwashers** (Bosch, Siemens, Neff, Gaggenau). The board talks
  to HomeConnect directly.
- **SmartThings washing machines** (Samsung). These go through
  [`cloud/`](cloud/README.md), a small self-hosted Python service. It handles
  the OAuth sign-in and the webhook events, which the board can't receive
  itself, and relays them over a WebSocket.

## What gets bridged: HomeConnect dishwasher

| Matter                     | HomeConnect                                              |
|----------------------------|----------------------------------------------------------|
| Dishwasher device type     | appliance of type `Dishwasher`                           |
| Bridged Device Basic Info  | name, brand, vib, haId; Reachable follows `connected`     |
| Operational State          | `BSH.Common.Status.OperationState`                       |
| CountdownTime              | `BSH.Common.Option.RemainingProgramTime`                 |
| Start / Stop               | `PUT` / `DELETE programs/active` (needs Remote Start)    |
| Pause / Resume             | `commands/BSH.Common.Command.Pause/ResumeProgram`, if offered |
| Dishwasher Mode            | one mode per available program, `ChangeToMode` selects it |
| On/Off (DeadFront)         | `BSH.Common.Setting.PowerState`                          |
| Dishwasher Alarm           | DoorError while the door is open mid-program             |

State changes arrive over the HomeConnect event stream (SSE), so door,
program and operation-state changes appear in the controller within a second
or two. The bridge only polls HomeConnect when it starts, when an appliance
reconnects, and when the event stream reconnects.

Starting a program from Matter only works after **Remote Start** has been
enabled on the appliance itself. HomeConnect does not allow remote start
otherwise, and the bridge then rejects Start with `UnableToStartOrResume`.

## What gets bridged: SmartThings washer

| Matter                     | SmartThings                                              |
|----------------------------|----------------------------------------------------------|
| Laundry Washer device type | device with `washerOperatingState`                       |
| Operational State          | `washerOperatingState.machineState`; phases from `washerJobState` |
| CountdownTime              | `washerOperatingState.completionTime` minus now (SNTP)   |
| Start / Stop / Pause / Resume | `washerOperatingState.setMachineState` (needs Smart Control on) |
| Laundry Washer Mode        | Normal / Delicate / Heavy / Whites, matched from the cycle name (read-only) |
| On/Off (DeadFront)         | `switch`                                                 |

Samsung reports cycles as model-specific codes, so the mode is a best guess
from the cycle name and can't be changed from Matter.

## Setup

1. Register an application at <https://developer.home-connect.com> with the
   **Device Flow** OAuth type. Note its client ID. The simulator has its own
   client ID.
2. Flash the firmware and connect the board to Ethernet.
3. Open <http://appliance-bridge.local> (or the board's IP address).
4. **HomeConnect** tab: paste the client ID and save. Then **Sign in**, and
   approve the request on the HomeConnect page that opens, either by scanning
   the QR code or by entering the code shown.
5. **Appliances** tab: click **Bridge** next to your dishwasher. It must be
   online so the bridge can read its program list.
6. **Commissioning** tab: open a commissioning window and add the bridge to
   your controller using the QR code or the manual pairing code.

For SmartThings:

1. Deploy st-relay and register the SmartThings app, as described in
   [`cloud/README.md`](cloud/README.md).
2. **SmartThings** tab: enter the relay URL (and the enrolment token, if you
   set one), then **Save**, **Enrol with relay**, and **Connect SmartThings**.
   Approve access in the tab that opens.
3. **Appliances** tab: click **Bridge** next to your washer.

## Building

Requires ESP-IDF v5.5.4 and Node.js. esp-matter 1.5 comes from the component
manager.

```sh
cd firmware
. ~/esp/esp-idf-v5.5.4/export.sh
idf.py set-target esp32s3
idf.py build flash monitor
```

`idf.py build` also builds the React app in `firmware/html_app`, and the result
is embedded in the firmware image, so OTA updates carry the UI. To work on the
UI against a running board:

```sh
cd firmware/html_app
npm install
VITE_API_TARGET=http://<board-ip> npm run dev
```

## Layout

```
firmware/main/
  app_main.cpp               node, aggregator, startup
  matter_ethernet_driver.cpp W5500 bring-up for CHIP
  web_server.cpp             REST API, WebSocket /api/events, embedded UI
  cloud_worker.cpp           single task for all outbound cloud calls
  homeconnect/               OAuth device flow, REST client, SSE stream
  smartthings/               st-relay connection (enrol, WebSocket), SmartThings REST client
  bridge/                    bridged endpoint lifecycle and persistence
  devices/                   cloud -> Matter mapping per appliance type
firmware/html_app/           React UI
cloud/                       st-relay: SmartThings OAuth + event relay (Python)
```

All outbound cloud calls run on one worker task. Matter command handlers
queue a job and return straight away, and the result arrives back through the
event stream. The CHIP stack lock is the bridge's lock: appliances are only
added, removed or updated while holding it.
