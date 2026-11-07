| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- | -------- | -------- |

# GofkuCamEmbedded

An ESP-IDF C++ controller for a PWM RC-servo gate. MQTT messages on `CatStatus`
and HTTP commands control the same `GateController` active object. Wi-Fi uses a
static IPv4 address, and NTP supplies wall-clock log timestamps.

The application is implemented in `main/app_main.cpp` and compiled as C++ by ESP-IDF. Its `app_main` entry point uses C linkage (`extern "C"`) so ESP-IDF can call it.

The default broker is `192.168.50.3` on port `1883`. Configure its hostname and
port separately. The server must expose a plain MQTT listener; an HTTP endpoint
alone cannot accept MQTT protocol messages.

## Features

- Managed components `espressif/mqtt` and `espressif/servo`
- Unencrypted MQTT over TCP, subscribing to `CatStatus` at QoS 1
- Queued gate commands, startup-open behavior, and reopening on MQTT loss
- HTTP controls and JSON status on port 80
- Wi-Fi/MQTT recovery and NTP synchronization

### Hardware Required

This example can be executed on any ESP32 board with a Wi-Fi connection
to the network hosting the MQTT broker, plus a PWM-controlled RC servo.
The servo signal is on GPIO3 by default. Use the servo's specified power supply
and a common ground with the ESP32; GPIO3 carries the PWM signal.

### Configure the project

* Open the project configuration menu (`idf.py menuconfig`)
* Set broker and Wi-Fi credentials under **GofkuCamEmbedded Configuration**.
* `ConnectionHandler` uses Wi-Fi station mode directly. There is no transport
  selection or example connection component.

| Setting | Generated configuration symbol | Default |
| --- | --- | --- |
| MQTT broker address | `CONFIG_GOFKUCAM_MQTT_BROKER_ADDRESS` | `192.168.50.3` |
| MQTT broker port | `CONFIG_GOFKUCAM_MQTT_BROKER_PORT` | `1883` |
| AccessPoint name | `CONFIG_GOFKUCAM_ACCESS_POINT_NAME` | Empty |
| AccessPoint password | `CONFIG_GOFKUCAM_ACCESS_POINT_PASSWORD` | Empty |
| NTP server | `CONFIG_GOFKUCAM_NTP_SERVER` | `pool.ntp.org` |
| Log timezone | `CONFIG_GOFKUCAM_LOG_TIME_ZONE` | Amsterdam POSIX TZ rule |
| Servo PWM GPIO | `CONFIG_GOFKUCAM_SERVO_GPIO` | `3` |
| Pulse width at 0 degrees | `CONFIG_GOFKUCAM_SERVO_MIN_PULSE_US` | `500` µs |
| Pulse width at 180 degrees | `CONFIG_GOFKUCAM_SERVO_MAX_PULSE_US` | `2500` µs |
| Servo min position (closed) | `CONFIG_GOFKUCAM_SERVO_MIN_POSITION` | `0` degrees |
| Servo max position (open) | `CONFIG_GOFKUCAM_SERVO_MAX_POSITION` | `180` degrees |
| Gate opening velocity | `CONFIG_GOFKUCAM_GATE_OPENING_VELOCITY_DPS` | `60` degrees/second |

The broker address is a hostname or IP address, without a scheme or port.
Servo positions are angles between 0 and 180 degrees; the maximum pulse width
must be greater than the minimum. Adjust the pulse range and gate positions to
match the servo and linkage.

### Gate control

`ServoMotor` wraps Espressif's managed `espressif/servo` 1.0.0 component using
`iot_servo_new()` and `iot_servo_write_angle()`. Its output is 50 Hz on LEDC
low-speed timer 0/channel 0. See the [Espressif servo documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/motor/servo.html).

`GateController` exclusively owns the servo. Its event loop performs all servo
calls; MQTT and HTTP only submit commands. The gate is commanded open at startup
and while MQTT is disconnected. Connecting MQTT does not close it: it stays open
until an explicit `close` command. MQTT loss clears pending close requests and
commands the gate open. Reconnecting leaves it open until another command.

The MQTT handler subscribes to the exact, case-sensitive topic `CatStatus` on
every connection. The complete payload must be exactly `open` or `close`.
Fragmented payloads are assembled in a bounded buffer before dispatch; unrelated
topics, malformed fragments, and other payloads are ignored. Retained messages
are handled as commands too. The old sample subscriptions/publications are gone.

The public `open()` and `close()` methods are asynchronous. Open is always allowed
after the gate task starts; close requires an MQTT connection. Commands use one
atomic request mailbox plus queued wakeups: the latest request wins, and MQTT
loss still reopens the gate if the notification queue is full. Driver failures
enter `Recovering` and retry with bounded delays.

Gate status reports the **commanded** state and angle, not physical feedback from
the servo. There is no position sensor, so it cannot confirm mechanical travel.

Opening from a known closed or intermediate position uses a constant-velocity
trajectory. A 20 ms state timer wakes the gate task to update the servo angle;
all servo writes still run in that task. The angle is calculated from elapsed
FreeRTOS ticks, supports fractional degrees, and is clamped at the configured open
position. Repeated open requests do not restart the movement. MQTT loss uses the
same controlled opening trajectory. Closing cancels an opening and commands the
closed position immediately. Startup still initializes directly to open because
there is no known starting position to interpolate from.

Set **Gate opening velocity (degrees/second)** in menuconfig for the persistent
default, or call `setOpeningVelocity(value)` / `open(value)` on `GateController`
for a runtime value between 1 and 360. Velocity can change during opening without
a position jump. At 60 degrees/second, a 180-degree opening takes about three
seconds. This controls the commanded trajectory; actual travel depends on the
servo and load.

### Static Wi-Fi address and HTTP server

The ESP32 joins the configured access point as a station with a fixed IPv4
address. The DHCP client is stopped before Wi-Fi starts and before each recovery
attempt. Network settings are defined in `main/inc/NetworkConfig.hpp`:

| Setting | Value |
| --- | --- |
| Device address | `192.168.50.4` |
| Subnet mask | `255.255.255.0` (`/24`) |
| Gateway | `192.168.50.1` |
| DNS server | `192.168.50.1` |

Once Wi-Fi connects, open **http://192.168.50.4/** from the same network.
The root page links to **http://192.168.50.4/status**, which returns JSON with the
device IP and current Wi-Fi/MQTT states. The HTTP listener runs on port 80,
independently of MQTT, and stays running across Wi-Fi recovery. The application
uses IPv4 only; IPv6 support is disabled. Configure the router so it does not assign
`192.168.50.4` to another device.

| Method | Path | Behavior |
| --- | --- | --- |
| GET | `/` | Page with Open/Close buttons and status links |
| GET | `/status` | Wi-Fi and MQTT state |
| POST | `/open` | Submit the same open command as MQTT `open`; optional `?velocity=30` sets degrees/second |
| POST | `/close` | Submit the same close command as MQTT `close` |
| GET | `/gate_status` | Gate state (including `opening`), target, commanded angle, opening velocity, MQTT availability, and last driver error |

Open/close return `202 Accepted`; the worker applies the command asynchronously.
Close returns `409 Conflict` while MQTT is offline. The root page uses POST forms
so simply fetching a link does not move the gate.

```sh
curl -X POST http://192.168.50.4/open
curl -X POST 'http://192.168.50.4/open?velocity=30'
curl -X POST http://192.168.50.4/close
curl http://192.168.50.4/gate_status
```

Example gate status after an open command:

```json
{"state":"open","target":"open","commanded_position":180.00,"opening_velocity_dps":30,"mqtt_connected":true,"last_error":"ESP_OK"}
```

The optional velocity persists in RAM for subsequent HTTP and MQTT open commands
until changed again or the device restarts. Invalid values return `400 Bad Request`
without changing the current target or velocity.

### NTP and log timestamps

`TimeSync` starts SNTP requests when Wi-Fi becomes ready, using the **NTP server**
setting under **GofkuCamEmbedded Configuration**. Requests use UDP port 123 and
the configured DNS server for hostname resolution. A local NTP server's IPv4
address can also be used. SNTP retries unanswered requests and periodically
resynchronizes using ESP-IDF's SNTP settings (one hour by default).

Normal ESP-IDF application logs use system-clock timestamps in `HH:MM:SS.sss`
format, instead of elapsed milliseconds. The clock is updated when NTP responds.
Before the first synchronization, timestamps are provisional; a confirmation log
prints the synchronized full date, time, UTC offset, and timezone.

The default POSIX timezone is `CET-1CEST,M3.5.0/2,M10.5.0/3`, which follows
Amsterdam winter/summer time. Set **Log time zone** to `UTC0` to use UTC.
Time synchronization never waits for a response in the Wi-Fi handler, creates
no synchronization semaphore, and does not block HTTP or MQTT. Wi-Fi loss stops
SNTP requests while the system clock keeps running; reconnection starts a fresh
synchronization. Recovery deadlines still use monotonic FreeRTOS ticks.

Bootloader/ROM and some precompiled Wi-Fi driver messages may still show elapsed
milliseconds because their timestamp format is controlled by the SDK binaries.

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

```
I (01:00:00.060) time_sync: Waiting for NTP; log timestamps are provisional until synchronization
I (01:00:01.245) connection: Connected to access point "your-network"
I (01:00:01.246) connection: - IPv4 address: 192.168.50.4,
I (01:00:01.248) time_sync: Synchronizing time with NTP server pool.ntp.org
I (14:25:30.120) time_sync: NTP time synchronized: 2026-11-07 14:25:30 +0100 (CET)
I (14:25:30.125) mqtt_example: Connected to MQTT broker 192.168.50.3:1883
```

## Active objects

`ActiveObject<Event, StateType, MaxEvents>` owns a FreeRTOS task, a typed state
machine, and a queue for up to `MaxEvents` pending events. Events must be default
constructible and trivially copyable because FreeRTOS copies their bytes.
`StateType` must be an enum; pass the initial state to the base constructor.
`loop()` blocks on the queue without polling and dispatches each event through
the nonvirtual `handle_event()` Template Method:

1. Call the derived `handle_state_event(current, event)` to choose the next state.
2. If the state changes, call `on_state_exit(current)`.
3. Store the next state, then call `on_state_enter(next)`.

Returning the current state handles an event without a transition. Entry and exit
hooks default to doing nothing and run only on state changes. The constructor sets
the initial state without calling an entry hook. Events and hooks execute sequentially in the worker;
`state()` provides an atomic snapshot for other tasks.

```cpp
#include "ActiveObject.hpp"

enum class WorkerEvent { Tick };
enum class WorkerState { Idle, Active };

class Worker final : public ActiveObject<WorkerEvent, WorkerState, 8> {
public:
    Worker() : ActiveObject(WorkerState::Idle) {}

protected:
    WorkerState handle_state_event(WorkerState state, const WorkerEvent &event) override
    {
        switch (state)
        {
            case WorkerState::Idle:
                switch (event)
                {
                    case WorkerEvent::Tick:
                        return WorkerState::Active;
                }
                break;
            case WorkerState::Active:
                switch (event)
                {
                    case WorkerEvent::Tick:
                        return WorkerState::Idle;
                }
                break;
        }
        return state;
    }
};

// In an application task, after the worker is fully constructed:
static Worker worker;
if (worker.start("worker", 4096, tskIDLE_PRIORITY + 1)) {
    if (!worker.postEvent(WorkerEvent::Tick)) {
        // Handle a full queue.
    }
}
```

`start()` returns false on allocation failure; its stack size is in bytes.
Calling it again after a successful start returns true without creating another
task or clearing queued events. Call it from one owning task after construction.
The task runs until device restart, so the object must remain alive for that
entire time; use static storage as above. Unstarted objects may be destroyed.

`postEvent()` is for task context and returns false before a successful start or
when the queue cannot accept an event within the optional timeout (FreeRTOS ticks,
default zero). Events can be posted from multiple tasks. The default `loop()` waits
indefinitely for events; state machines can override `event_wait_ticks()` and
`on_timeout()` to give an attempt or a recovery delay a deadline without polling,
semaphores, or a separate timer task.

`MqttHandler` and `ConnectionHandler` have their own event and state enums and
16-slot queues. Both use `Idle`, `WaitingForConnection`, `Connected`, and
`Recovering`. Their asynchronous `connect()` methods start their active-object
tasks and queue a start request; success means the request was queued, not that
a network or broker connection has already been established.

`app_main()` initializes NVS, netif, and the default event loop, wires the network
readiness listener to MQTT, starts the HTTP server, and starts both handlers.
It starts `GateController` before the network clients so servo initialization
requests the open position first.
Wi-Fi uses nonblocking driver setup and static addressing. Wi-Fi association and MQTT connection attempts have
30-second deadlines. A disconnect, initialization error, or expired deadline
enters `Recovering`. Retry delays grow through 1, 2, 4, 8, 16, and 30 seconds and
stay at 30 seconds until connection succeeds. There is no retry count limit;
a successful connection resets the delay to one second.

The connection state machine owns Wi-Fi reconnects, instead of using the SDK
example's retry handler or its old 20-attempt limit. Link loss and lost-IP events
clear Wi-Fi readiness and enter recovery. Readiness requires Wi-Fi association
and the configured IPv4 address. No other network transports are supported.

MQTT's built-in auto-reconnect is disabled. Its state machine waits for the
connection handler's readiness signal before attempting a broker connection,
pauses retries while the network is unavailable, and restores subscriptions on
every successful broker reconnection. The SDK socket timeout is five seconds and
MQTT keepalive is 30 seconds. SDK callbacks consume transient payloads immediately
and post state notifications; retry decisions and reconnect API calls run in the
active-object task. Atomic status snapshots keep recovery notifications effective
even if a queue is full.

Both handlers dispatch events using a state switch with a nested event switch.
Successful Wi-Fi connections log the access-point name; successful MQTT
connections log the broker hostname and port, including after recovery.

Both handlers have static storage and run until device restart. No active-object
stop API or semaphore is introduced.

Host tests cover queue behavior, state transitions, more than 20 consecutive
failures, retry delays, association and broker deadlines, queue saturation, network
loss, subscription restoration, static IP configuration, connection logging, and
HTTP routes, gate startup, MQTT loss, queued close cancellation, servo driver
failures, and fragmented `CatStatus` messages using SDK test doubles and a
controlled clock. Motion tests cover velocity changes, fractional steps, delayed
updates, endpoint clamping, interruption, tick wrap, and driver recovery. NTP tests cover network
gating, reconnects, nonblocking configuration, and winter/summer timezone formatting:

```sh
cmake -S tests/active_object -B /tmp/gofkucam-active-object-tests
cmake --build /tmp/gofkucam-active-object-tests
ctest --test-dir /tmp/gofkucam-active-object-tests --output-on-failure
```

`pytest_mqtt_tcp.py` is a read-only hardware startup check. It verifies the gate's
startup-open log, MQTT subscription, and `/gate_status`; it does not publish gate
commands. Normal firmware startup itself commands the servo open.
