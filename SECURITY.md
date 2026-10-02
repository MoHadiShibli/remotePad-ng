# Security

## Reporting a vulnerability

Please report security problems privately: go to the
[Security tab](https://github.com/MoHadiShibli/remotePad-ng/security), choose **Report a vulnerability**, and
describe the problem and how to reproduce it. Please don't open a public issue for it. You'll get an answer as
soon as possible, and credit in the release notes if you want it.

## How RemotePad NG is exposed

While a game runs, the plugin runs a web server on port **4263** of the PS4. It serves the controller page and
a WebSocket for the controls and settings. Anyone who can reach that port can use it, so it's meant for your
home network.

What's protected:
- **Other websites** can't connect. A page you visit can't open the WebSocket to your console in the
  background.
- **Settings** (`settings.set`) and console notifications (`notify`) only work from:
  - the console's own page opened by its IP address;
  - clients that aren't browsers.

  A site that reaches the console through its own domain name (DNS rebinding) can't change them, and neither
  can a page saved as a file.
- **`lock_settings=1`** in `remote_pad.ini` refuses every change from the page.
- **Resource limits**: at most 16 connections, and at most 64 KB of buffered data per connection.
- **Input checks**: every value from the network is range-checked; names are validated before they're
  written to `remote_pad.ini`.
- **The page** is sent with a content security policy, `X-Content-Type-Options: nosniff` and
  `Referrer-Policy: no-referrer`, and it can't be framed by other sites.

What isn't protected:
- There's no password. Anyone on your network can use the pads.
- Traffic isn't encrypted (plain `http` and `ws`).

To play with friends over the internet, use a VPN or a tunnel instead of opening port 4263 on your router,
and set `lock_settings=1`.
