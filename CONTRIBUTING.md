# Contributing to RemotePad NG

Thanks for helping! Bug reports, game compatibility reports, fixes and reviews are all welcome.

## Reporting a bug

Use the [bug report form](https://github.com/MoHadiShibli/remotePad-ng/issues/new/choose). The most useful
details:
- the RemotePad NG version, shown at the bottom of the page;
- the firmware and GoldHEN versions;
- the game and its title ID (like `CUSA00001`);
- what you did and what happened;
- the plugin's log, if you can get it. GoldHEN's log server is on port 3232: `nc <ps4-ip> 3232` from a
  computer. The plugin's lines start with `[remote_pad]`.

Logs can contain your console's IP address and PS4 user IDs. Remove them before posting if you prefer.

Security problems: please follow [SECURITY.md](SECURITY.md) instead of opening a public issue.

## Building

The easiest way is Docker with the `xfangfang/goldhen_plugin` image, which has the OpenOrbis toolchain and
LLVM:

```shell
git clone --recursive https://github.com/MoHadiShibli/remotePad-ng
cd remotePad-ng
docker run --rm -v "$PWD:/src" -w /src xfangfang/goldhen_plugin:latest \
  sh -c "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
```

The plugin is `build/remote_pad.prx`. With `-DDEVICE_IP=<ps4-ip>`, the build also uploads it to the PS4 over
GoldHEN's FTP server.

The web page is one file with no dependencies: `client/index.html`. It's packed into the plugin when you
build. To try page changes without rebuilding:
- open the file from disk and enter the console's address, or add `?host=<ps4-ip>:4263` to the page's URL;
- `?pad=2` opens Pad 2 directly.

A page opened from a file can play but can't change the console's settings.

## Code style

Match the code around your change:
- C with GNU extensions (gnu11), 4-space indent, `camelCase` for functions;
- logs go through `final_printf`, and errors use `SCE_OK` and the `ORBIS_*` codes;
- hooks follow the existing pattern in `src/main.c`: `HOOK_DEFINE(fn, ...) { ...; return HOOK_PASS(fn, ...); }`,
  plus `HOOK32(fn)` in `plugin_load` and `UNHOOK(fn)` in `plugin_unload`;
- a new way to connect is a new `RemotePadDriver` in `src/pad/` (see `src/pad/ws.c`), added to `padDrivers[]`
  in `src/pad.c`;
- comments explain *why*, not *what*;
- the page has no build step and no libraries: keep it that way.

Build without new warnings. Test on a real console when you can, and say in your pull request which games
you tried.

## Reviewing AI-written code

Much of RemotePad NG 1.0.0 was written with the help of an AI model (Claude Opus 5.5). It has been tested, but
AI-written code can look right and still be wrong. Please read it critically: if you find dead code, wrong
assumptions, needless complexity or anything that smells like AI slop, open an issue or a pull request. Fixes
like that are as welcome as new features.

## License

By contributing, you agree that your contributions are licensed under the [GNU GPL v3](LICENSE), like the rest
of the project.
