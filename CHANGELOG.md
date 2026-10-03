# Changelog

## RemotePad NG 1.0.1 (2026-10-03)

### Fixed

- **Remote players can join Unity games that said "Sign in"**, like Tricky Towers. These games open a second,
  "special" controller port for every player, compare it with the normal one and close one of them. The plugin
  refused the special port of its pads, so the game kept no controller for those players. A pad's special port
  now works as the same pad.
- **Use with the DualShock** stopped working in those games right after they started: closing the special port
  ended the sharing. Only the normal port is shared now.
- Some messages in the PS4 log counted pads from 0. They all use the page's numbers now, Pad 1 to Pad 4.
- The page's footer had no link to RemotePad NG itself. "RemotePad NG" now opens its GitHub page, next to the
  link to the original remotePad.

## RemotePad NG 1.0.0 (2026-10-02)

The first release of RemotePad NG, a maintained fork of [remotePad](https://github.com/xfangfang/remotePad)
1.2.0 by xfangfang. Settings files from remotePad keep working, decimal user IDs included, and the plugin file
is still `remote_pad.prx`.

### New

- **New web page.**
  - A PS4-style home screen where you pick a pad before playing. It shows each pad's state live: free, joining
    the game, playing, no free player slot, or turned off. It also shows how many devices use each pad.
  - A controller screen with DualShock 4 shaped buttons, a large touchpad (two fingers, tap to click), the
    pad's light bar color, and vibration.
  - A **layout editor**: move, resize and hide every button, with "Reset to default". The layout is saved per
    device and per orientation.
  - L3 / R3 by double-tapping a stick, or with their own buttons when the double-tap is off.
  - The phone's screen stays on while you play.
  - Controllers connected to the phone or computer can each play as any pad. The on-screen controls show what
    they do.
  - Keyboard keys can be changed.
- **Settings from the page**: pad names, PS4 users, pads on or off, and how players join. They apply right away
  and are saved in `remote_pad.ini`, for all games or for one game.
- **Players join when a pad is picked.** A guest player only enters the game when someone picks its pad, and a
  pad looks switched off while nobody uses it. `join_on_start=1` and `always_connected=1` bring back the old
  behavior.
- **Guest fallback**: a pad set to a PS4 user who isn't signed in plays as a guest instead of not working.
- **Use with the DualShock** (`userN_share=1`): a pad adds its input to a signed-in user's own controller, so
  both work at once (xfangfang/remotePad#7).
- **Security on the network.**
  - Other websites can't connect: this blocks cross-site WebSocket hijacking.
  - Settings can only be changed from the console's own page opened by IP address, or from clients that
    aren't browsers. This blocks DNS rebinding.
  - At most 16 connections, and at most 64 KB of buffered data per connection.
  - The page is sent with a content security policy and can't be framed by other sites.
  - `lock_settings=1` makes the settings read-only.
- **User IDs in hex**, as Apollo Save Tool shows them (`1A2B3C4D` or `0x1A2B3C4D`). Decimal IDs still work.
- **A clearer notification** when a game starts: "Open http://<address>:4263 on your phone or PC".
- **WebSocket protocol 2**, with `claim`, `leave`, `status`, `settings` and `settings.set`, and live status
  notifications. See [docs/protocol.md](docs/protocol.md). Version 1 clients still work.
- The plugin writes its log to the kernel log, so GoldHEN's klog (port 3232) shows it even without the TTY
  redirect.
- Pushing a version tag publishes a GitHub release with the plugin attached.

### Fixed

- **Input delay that grew to seconds** in games that read one controller sample per frame
  (xfangfang/remotePad#7). Stick moves no longer pile up in the queue.
- **Crashes** (xfangfang/remotePad#9):
  - the page's `notify` message was used as a format string;
  - a heap overflow in the `remote_pad.ini` parser;
  - the web server thread's stack was too small;
  - crashes and leaks when the plugin unloads;
  - a race while the server starts;
  - missing checks for empty buffers from games.
- **Only one pad worked with several PS4 users**: hex user IDs were read as decimal, so pads got the wrong
  users (xfangfang/remotePad#2).
- Pads got real users that weren't signed in, and replaced their names and colors.
- `scePadRead` returned shifted data when more than one sample was queued.
- Held buttons looked released between updates.
- Buttons stayed pressed when a phone went to sleep or lost its connection.
- Clients that send lowercase HTTP headers (`connection: upgrade`) couldn't connect.
- Vibration and light bar messages were delayed: `TCP_NODELAY` is now set.
- Vibration went to every connected device instead of the pad's own.
- `remote_pad.ini` problems:
  - a game section placed before `[default]` was overridden by it;
  - a UTF-8 byte order mark broke the first section;
  - `]` inside a value started a new section.
- A settings problem no longer turns the whole plugin off.
- Guests are now named after their pad: Pad 1 plays as "Remote1" (it was "Remote0").
- Release builds no longer write every setting to the log.

### Documentation

- New README, and a step-by-step guide in the [wiki](https://github.com/MoHadiShibli/remotePad-ng/wiki):
  - installing;
  - playing;
  - players and PS4 users;
  - every `remote_pad.ini` key;
  - troubleshooting (xfangfang/remotePad#5, #8, #10);
  - a FAQ (xfangfang/remotePad#1, #7);
  - compatibility.
- New: [docs/protocol.md](docs/protocol.md), [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).

## remotePad 1.2.0 and older

See the [original project's releases](https://github.com/xfangfang/remotePad/releases).
