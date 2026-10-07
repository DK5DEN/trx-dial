# TRX-Dial

A rotary controller for radio software, built on the [M5Stack Dial](https://docs.m5stack.com/en/core/M5Dial)
(ESP32-S3, round 240×240 touch display, rotary encoder with push button).

TRX-Dial knows nothing about the system it controls. The connected host (an app or
a web page) describes a home screen and a menu over Bluetooth LE; the dial draws
them and reports turns, clicks and chosen values back. One firmware therefore
works with every host that speaks the protocol.

| Host | What the dial does |
|---|---|
| [JaYTrX](https://dj1jay.de/jaytrx/) (Android, FM-Funknetz), integration proposed to the app author | Home: volume, current talkgroup. Menu: talkgroup list, TG mute, hold |
| [afu-remote](https://afu.tools/remote) (web page, remote operation of transceivers) | Home: VFO frequency, tuning step, S-meter. Menu: band, mode, step, memories, page volume and every control the radio reports |

The dial never transmits: hosts leave out every control that keys the transmitter
or needs a confirmation.

## Operation

- **Home screen**: turn and press go to the host (volume/mute, tune/step, ...).
- **Menu**: long press, or tap the screen. Turn to choose, press (or tap the item) to open it.
  Lists (talkgroups, bands, modes) open as a scrolling list, values as a gauge,
  switches toggle in place.
- The dial adds its own entries after the host's: brightness, confirmation beeps, back.
- Every screen falls back to the home screen after 8 s without input.
- Outer ring: blue = connected, green = receiving, red = transmitting, grey = not connected.

## Repository

| Path | Content |
|---|---|
| `firmware/` | PlatformIO project for the M5Stack Dial (Arduino, M5Unified, NimBLE, ArduinoJson) |
| `docs/protocol.md` | The protocol (v2): GATT service, messages in both directions |
| `tools/shot.py` | Grab the dial's screen over USB and simulate knob input, for development |

The afu-remote side lives in the afu.tools repository (`site/js/remote-dial.js`).

## Firmware

Build with PlatformIO:

```sh
cd firmware
pio run
pio run -t upload        # or flash firmware/.pio/build/m5dial/*.bin with esptool
```

The dial advertises as `TRX-Dial`. Brightness and the beep setting are kept in flash.

Over the USB serial port (115200 Bd) the firmware accepts `shot` (dump the frame
buffer), `rot N`, `click` and `long` (simulated input):

```sh
python tools/shot.py COM5 screen.png "rot 1" click
```

## Adding a host

Connect as GATT central to service `4a415954-5258-4449-414c-000000000000`, enable
notifications, send `menu`, `val` and `home`, react to `turn`, `click`, `set` and
`press`. Details and examples: [docs/protocol.md](docs/protocol.md).

## License

MIT, see [LICENSE](LICENSE).
