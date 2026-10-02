# RemotePad NG WebSocket protocol

This page is for people writing their own RemotePad NG client: an app, a PSP homebrew, a script. The web page in
`client/index.html` is the reference client.

The plugin runs one server on port **4263**, and only while a game is running. A plain HTTP `GET` returns the
web page. A WebSocket upgrade on any path opens a connection: `ws://<ps4-ip>:4263`. Messages are JSON-RPC style
text frames.

This describes protocol version **2**. Version 1 clients (the original web page) keep working: they only use
`u`, `info` and `notify`.

## Who may connect

Browsers send an `Origin` header with every WebSocket handshake, and the server uses it:

| Client | May connect | May use `settings.set` and `notify` |
|---|---|---|
| No `Origin` (apps, scripts, homebrew) | yes | yes |
| The console's own page, opened by IP address (`Origin` matches `Host`) | yes | yes |
| The console's page reached through a domain name | yes | no (403) |
| A page saved as a file (`Origin: null`) | yes | no (403) |
| A page from any other site | no (HTTP 403) | |

The server accepts at most 16 connections at once, and at most 64 KB of buffered data per connection.

## Requests

A request has a `method` and `params`. Add an `id` to get a reply. A reply carries the same `id` and either a
`result` or an `error` (`{"code": 400, "message": "..."}`).

```json
{"id": 1, "method": "claim", "params": [0]}
{"id": 1, "result": { ...status object... }}
```

### `u`: controller state

Sends the full state of one pad. There is no reply, even with an `id`.

```json
{"method": "u", "params": [pad, buttons, lx, ly, rx, ry, l2, r2, touches, id1, x1, y1, id2, x2, y2]}
```

| Param | Range | Meaning |
|---|---|---|
| `pad` | 0-3 | Pad 1-4 |
| `buttons` | bit mask | See the table below. Other bits are ignored. |
| `lx`, `ly`, `rx`, `ry` | 0-255 | Stick positions; 128 is the center, 0 is left or up |
| `l2`, `r2` | 0-255 | Analog triggers |
| `touches` | 0-2 | Fingers on the touchpad; each one adds `id, x, y` |
| `id` | 0-127 | Finger ID, the same while the finger stays down |
| `x`, `y` | 0-1919, 0-941 | Finger position on the touchpad |

| Button | Bit | Button | Bit |
|---|---|---|---|
| L3 | `0x0002` | R3 | `0x0004` |
| OPTIONS | `0x0008` | Up | `0x0010` |
| Right | `0x0020` | Down | `0x0040` |
| Left | `0x0080` | L2 | `0x0100` |
| R2 | `0x0200` | L1 | `0x0400` |
| R1 | `0x0800` | Triangle | `0x1000` |
| Circle | `0x2000` | Cross | `0x4000` |
| Square | `0x8000` | Touchpad click | `0x100000` |

How to send:
- **Send on change, plus at least once a second.** Each `u` replaces the whole state: a button stays pressed
  until a `u` without it arrives.
- **Hold short presses.** The game reads its controllers once per frame, so keep a press for at least about
  50 ms or the game can miss it.
- **Keep the stream small.** Samples that only move sticks or triggers replace the newest unread one in the
  plugin. Presses, releases and touches are queued in order, so a game never misses one.

Sending `u` for a pad also claims it for this connection, as if it had sent `claim`.

### `claim` (v2): which pads this connection controls

```json
{"id": 2, "method": "claim", "params": [0, 2]}
```

The params are the exact set of pads this connection controls; `[]` means none. The reply is the status
object.

- A pad's first device makes its guest player join the game.
- The game sees a pad's controller as connected only while a device controls it, unless `always_connected=1`.
- Pads dropped from the set are released: their buttons let go.
- Connections that use `claim` must send something at least every 3 seconds, or the plugin releases their
  pads. A phone that goes to sleep with a button held is the typical case. The web page sends `u` every second.
- Closing the connection releases its pads.

### `leave` (v2): the guest player leaves the game

```json
{"id": 3, "method": "leave", "params": [2]}
```

Pad 3's guest signs out of the game, and this connection stops controlling that pad. Error 409 if another
device still uses the pad. Pads that play as a real PS4 user don't sign out. The reply is the status object.

### `status` (v2)

```json
{"id": 4, "method": "status", "params": []}
```

The reply is the status object.

### `info`

```json
{"id": 5, "method": "info", "params": []}
{"id": 5, "result": {"version": "v1.0.0", "protocol": 2, "pads": 4}}
```

Version 1 servers only reply with `version`.

### `notify`

```json
{"method": "notify", "params": ["Hello from the phone"]}
```

Shows a notification on the PS4 with this text.

### `settings` (v2)

```json
{"id": 6, "method": "settings", "params": []}
```

Replies with the settings in effect and the console's users:

```json
{
  "title": "CUSA00001", "locked": false, "gameOnly": false,
  "joinOnStart": false, "alwaysConnected": false,
  "pads": [{"enabled": true, "id": "1A2B3C4D", "share": false, "name": "Alex"}, ...],
  "users": [{"id": "1A2B3C4D", "name": "Alex", "signedIn": true}, ...]
}
```

| Field | Meaning |
|---|---|
| `title` | The running game's title ID |
| `locked` | `lock_settings=1`: `settings.set` is refused |
| `gameOnly` | The settings come from the game's own section of `remote_pad.ini` |
| `pads[].id` | The pad's PS4 user in hex, `""` for a guest |
| `pads[].name` | The pad's name, `""` for the default |
| `users` | The registered users if the system lets a game list them, and at least the signed-in ones |

### `settings.set` (v2)

```json
{"id": 7, "method": "settings.set", "params": {
  "gameOnly": false, "joinOnStart": false, "alwaysConnected": false,
  "pads": [{"enabled": true, "id": "0x1A2B3C4D", "share": true, "name": "Alex"}, ...]}}
```

Applies the settings right away and saves `remote_pad.ini`: for all games, or with `gameOnly` for the running
game only. Leaving a field out keeps its value.

- Send IDs with `0x`. Without it, an ID made only of digits is read as decimal.
- The reply is `{"saved": true, "settings": {...}}`. `saved` is false when the file couldn't be written.
- Error 400, with a message, for:
  - a name that isn't 3 to 16 characters or contains `;`;
  - an ID that isn't a user ID;
  - two pads with the same user;
  - locked settings.

## Notifications from the server

Messages without an `id`:

| Method | Params | When |
|---|---|---|
| `s` (v2) | status object | Pads or players changed: sent to every connection |
| `v` | `[pad, large, small]` | The game set vibration (0-255 per motor); `[pad, 0, 0]` stops it |
| `l` | `[pad, r, g, b]` | The game set the light bar color |
| `rl` | `[pad, r, g, b]` | The game reset the light bar; the color is the player color (v1 sent `[pad]`) |

Vibration only goes to connections that control that pad, and to version 1 connections that haven't sent input
yet.

## Status object

```json
{"version": "v1.0.0", "protocol": 2, "pads": [
  {"pad": 0, "open": true, "enabled": true, "real": false, "logged": true, "joined": true,
   "connected": true, "full": false, "shared": false, "player": 2, "uid": "20000000", "cfg": "",
   "name": "Remote1", "color": [255, 48, 64], "clients": 1}, ...]}
```

| Field | Meaning |
|---|---|
| `open` | The game opened this pad (or the shared real controller) |
| `enabled` | The pad is on (`userN_enabled`) |
| `real` | The pad plays as a real PS4 user (configured and signed in) |
| `logged` | The game sees the player: a real user, or a guest that got a player slot |
| `joined` | The player wants to play: picked, or from the start with `join_on_start` |
| `connected` | What the game is told about the controller |
| `full` | A guest wants to play but every player slot is taken |
| `shared` | "Use with the DualShock": the pad adds to the user's real controller |
| `player` | Player number, 0 while the game doesn't see the player |
| `uid` | The user ID the game sees, in hex |
| `cfg` | The configured PS4 user in hex, `""` when none (it may be signed out, see `real`) |
| `name` | The player's name |
| `color` | The light bar color the game set, otherwise the player color |
| `clients` | How many connections control the pad |
