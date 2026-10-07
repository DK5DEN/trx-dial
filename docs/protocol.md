# TRX-Dial protocol v2

TRX-Dial is a BLE rotary controller that knows nothing about the system it
controls. The host (an app, a web page) describes a home screen and a menu;
the dial draws them and reports what the user did. Hosts so far: the JaYTrX
app, the afu-remote web page.

## GATT

| Item | UUID | Properties |
|---|---|---|
| Service | `4a415954-5258-4449-414c-000000000000` | advertised |
| RX (host → dial) | `4a415954-5258-4449-414c-000000000001` | write, write without response |
| TX (dial → host) | `4a415954-5258-4449-414c-000000000002` | notify |

The host is GATT central: it connects, requests MTU 247 and enables
notifications on TX. Messages are UTF-8 JSON objects terminated by `\n`; one
message may span several writes or notifications. Unknown keys and message
types are ignored on both sides, so either side can add fields.

## Dial → host

| Message | Meaning |
|---|---|
| `{"c":"hello","v":2}` | dial (re)connected or restarted: send `menu`, `home` and all `val` again |
| `{"c":"turn","d":N}` | knob turned N detents on the home screen (+ = clockwise) |
| `{"c":"click"}` | knob pressed on the home screen |
| `{"c":"set","id":ID,"v":V}` | new value for a menu item (`list`, `range`, `toggle`) |
| `{"c":"press","id":ID}` | `button` item pressed |

What `turn` and `click` do on the home screen is up to the host (JaYTrX:
volume and mute; afu-remote: tune and change the step).

## Host → dial

### `menu`: the menu, sent on connect and whenever it changes

```json
{"t":"menu","items":[
  {"id":"vol","label":"Lautstärke","type":"range","min":0,"max":15,"icon":"speaker"},
  {"id":"tg","label":"Talkgroup","type":"list","icon":"list",
   "opts":[[96126,"96126","Haßberge"],[96450,"96450","Coburg"]]},
  {"id":"tgmute","label":"TG stumm","type":"toggle","icon":"mute"},
  {"id":"hold","label":"Hold","type":"toggle","icon":"lock"}
]}
```

| Key | Meaning |
|---|---|
| `id` | key used in `val`, `set`, `press` |
| `label` | menu text |
| `type` | `list` (pick one option), `range` (number), `toggle` (on/off), `button` (action) |
| `opts` | `list` only: `[value, title, subtitle]`; value is a number or a string |
| `min`, `max`, `step`, `unit` | `range` only; `step` defaults to 1 |
| `icon` | optional: `speaker`, `mute`, `list`, `lock`, `wave`, `mode`, `band`, `filter`, `memory`, `gear`; unknown or missing gives a neutral symbol |

The dial adds its own items (brightness, beeps, back) after the host's.
A host never puts items into the menu that transmit or that need a
confirmation.

### `val`: current values of menu items

```json
{"t":"val","v":{"vol":7,"tg":96126,"tgmute":0,"hold":0}}
```

Only changed keys need to be sent.

### `home`: the home screen

```json
{"t":"home","top":"DUAL · SCAN 5 TGs","big":"7","label":"Lautstärke",
 "gauge":[7,15],"pill":"96126 Haßberge","accent":"yellow",
 "ring":"idle","banner":"","muted":0}
```

| Key | Meaning |
|---|---|
| `top` | small status line at the top |
| `big` | large value in the centre (volume, frequency, ...) |
| `label` | small line under `big` |
| `gauge` | optional `[value, max]`: segmented ring around the screen |
| `pill`, `pill2` | box at the bottom: `pill` in the accent colour, `pill2` after it in white (TG number + name, step + mode) |
| `accent` | colour of `pill` and gauge: `green`, `yellow`, `orange`, `red`, `blue` |
| `ring` | outer ring: `off` (not connected), `idle`, `rx`, `tx` |
| `banner` | text in a coloured box at the top while `ring` is `rx` or `tx` (talker call, "SENDEN") |
| `muted` | 1: show the mute symbol instead of `big` |

Only changed keys need to be sent; missing keys keep their last value.
