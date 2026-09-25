# TetherDesk

A remote desktop app written in C and C++. One host shares its screen, and viewers can connect in two ways:

- **In a browser.** The host serves the viewer itself: open `http://host:5980/`. The viewer is C++ compiled to WebAssembly with Emscripten.
- **With a native app.** This is the same C++ viewer built against SDL2, for macOS, Linux and Windows.

Both viewers use the same protocol, which runs over WebSocket.

```
tetherdesk/
  common/   C11: wire protocol, tile codec + LZ compressor, SHA-1/SHA-256/HMAC,
            X25519 + ChaCha20-Poly1305 + HKDF (rd_secure.c), WebSocket framing,
            portable sockets, embedded font (no dependencies)
  host/     C++17 host: poll() event loop, HTTP + WebSocket server, sessions,
            parallel tile encoder, platform backends:
              platform_mac.mm   ScreenCaptureKit + CGEvent + NSPasteboard (Objective-C++)
              platform_win.cpp  GDI BitBlt + SendInput + Win32 clipboard
              platform_x11.cpp  XGetImage + XTest (Linux/X11)
              platform_demo.cpp synthetic interactive desktop (no permissions needed)
  client/   C++17 viewer (SDL2), shared by native and web builds
              store.cpp            saved PCs, pinned host identities, thumbnails
              transport_native.cpp  TCP + our own WebSocket client
              transport_web.cpp     browser WebSocket via Emscripten
  tools/    tetherdesk-probe (headless test client), gen_font.py (build-time only)
  tests/    codec / crypto / WebSocket self-tests
```

## Install

Download from the [Releases page](../../releases):

- **macOS 12.3+:** `TetherDesk-<version>-macOS.dmg`. Open it and drag TetherDesk to Applications. The app isn't notarized, so the first time you open it, right-click it and choose **Open**.
- **Windows 10/11:** `TetherDesk-Setup-<version>.exe`. SmartScreen may warn about an unknown publisher; choose **More info → Run anyway**.
- **Linux (X11):** a zip containing the binaries.

The app has two tabs:

- **Connect to a PC:** control another computer.
- **Share this PC:** turn on the switch to let someone connect to yours. It shows your address, a password and this PC's identity. On macOS, the first time you share, allow TetherDesk under Screen Recording and Accessibility; the tab has buttons that open those settings.

## Build from source

You need CMake and a C/C++ compiler. For the native viewer you also need either:

- SDL2 installed (`brew install sdl2` or `apt install libsdl2-dev`), or
- the `-DTD_STATIC_SDL=ON` option, which downloads SDL and builds it into the app.

For the web viewer you need Emscripten (`brew install emscripten`).

```bash
./build.sh                         # host + native viewer + web viewer -> build/
./build.sh test                    # native build + unit tests
./packaging/macos/make_dmg.sh      # self-contained dist/TetherDesk-<version>-macOS.dmg
```

To build a Windows installer, see the `Package (Windows installer)` step in `.github/workflows/build.yml`. It uses Inno Setup with `packaging/windows/tetherdesk.iss`.

## Run

```bash
build/tetherdesk-host                    # share this screen (prints a generated password + URLs)
build/tetherdesk-host --demo             # share a synthetic demo desktop instead
open build/TetherDesk.app                 # native viewer (build/TetherDesk on Linux/Windows)
build/TetherDesk.app/Contents/MacOS/TetherDesk 192.168.1.20 --password abcde-fghjk  # connect straight away
```

Prebuilt Windows, Linux and macOS zips are attached to each
[GitHub release](../../releases) (built by `.github/workflows/build.yml`).

### Controlling another computer

1. On the computer you want to control, download the zip for its OS and run
   `tetherdesk-host` (Windows: `tetherdesk-host.exe`). It prints a password
   and an **Identity** fingerprint.
2. On your computer, open `tetherdesk`, type the other computer's address into
   **Quick connect**, and enter the password.
3. The first time, TetherDesk shows the host's identity. Check that it matches
   what the host printed, then choose **Trust and connect**. After that, the
   PC is saved on the home screen with a preview.

In a browser, go to `http://<host>:5980/`. To connect automatically, add the password and your name to the URL: `http://<host>:5980/#password=...&name=Sam`.

On macOS, the host needs two permissions in System Settings > Privacy & Security:

- **Screen Recording**, so it can capture the screen.
- **Accessibility**, so it can control the mouse and keyboard.

The `--demo` mode needs neither.

Host options: `--port`, `--bind`, `--password`, `--view-only`, `--no-files`, `--fps`, `--display`, `--max-viewers`, `--native-res` (full Retina resolution), `--threads`. While the host is running, you can type `/help` in its terminal for commands: `/list`, `/kick`, `/viewonly`, `/password`, chat.

## Features

- **Screen updates:** the screen is split into 64×64 tiles and only changed tiles are sent. Each tile uses whichever encoding is smallest: solid colour, a palette of up to 256 colours with 1/2/4/8-bit indices, or predicted RGB with LZ compression. Tiles are encoded in parallel across CPU cores.
- **Quality:** each viewer gets flow control (at most 2 frames unacknowledged). Quality is chosen automatically: when the network backs up, colour depth drops, and when it clears, the host resends a sharp full frame. You can also pick Lossless, High, Medium or Low and a frame rate from 5 to 60 fps.
- **Input:** mouse (including double-clicks and drags), wheel and keyboard. Keys are sent as USB HID codes and mapped to macOS, Windows or Linux key codes, so they work regardless of keyboard layout. Held keys are released when a viewer leaves or the viewer window loses focus.
- **Several viewers at once:** there is a viewer list, a view-only mode, and the operator can kick a viewer or remove their control.
- **Clipboard, chat and files:** clipboard text syncs both ways. Viewers and the host operator can chat. Dropping a file on the native viewer uploads it to `~/Downloads/TetherDesk` on the host, with a progress bar.
- **Display and view options:** you can switch between displays. The viewer can fit the screen to the window or show it 1:1 with panning, and has a stats overlay (fps, Mbit/s, round-trip time) and fullscreen.
- **Reconnects:** if the connection drops, the viewer reconnects automatically.
- **Remote Desktop-style viewer:**
  - a home screen of saved PCs with session thumbnails, plus quick connect;
  - remembered passwords (optional);
  - sessions that open full screen, with an auto-hiding connection bar (Pin, Menu, Hide, Window/Full, Disconnect).
- **Security:** end-to-end encryption and host identity pinning (see below). Uploaded file names are sanitised, and all decoders check bounds on untrusted input.

## Security and internet use

Every session is end-to-end encrypted:

- **Key exchange:** X25519, which mixes in the host's long-term identity key.
- **Encryption:** ChaCha20-Poly1305 with per-message counters, so traffic is private, tamper-evident and replay-proof.

All of this is implemented in `common/rd_secure.c` and checked against the RFC test vectors in `tests/`.

- **Password:** it never crosses the network, even in encrypted form. The viewer proves it knows the password with an HMAC over the handshake.
- **Host identity:** the host keeps its identity key in your user config folder. Viewers pin its fingerprint the first time they connect, like SSH does. If the key ever changes, they show a warning.
- **Guessing protection:** after 5 wrong passwords, an address is locked out, and the lockout grows each time. More than 30 failures a minute from anywhere pauses all logins.
- **Password length:** the host insists on at least 8 characters when it's reachable from the network.

To reach a host from outside your home network, do one of these:

- **Forward a port:** forward TCP port 5980 on your router to the host computer. Then connect to your public IP or a dynamic-DNS name.
- **Use a VPN:** for example Tailscale. Connect to the host's VPN address; no port forwarding is needed.

The browser viewer is itself downloaded from the host over plain HTTP. If you need protection against someone tampering with your network, use the native viewer, which verifies the host's identity.

## Known limitations

- **Linux:** only X11 is supported (not Wayland). Clipboard sync is not implemented on Linux.
- **Web viewer:** it cannot read or write the browser's system clipboard (that would need JavaScript). Some browser shortcuts, such as Cmd+W, are caught by the browser and never reach the remote machine.
- **Windows:** Ctrl+Alt+Del cannot be injected with SendInput.
- **Windows and Linux:** CI compiles these hosts and runs an encrypted end-to-end test against the demo desktop. Real-screen capture and input on them haven't been tried on actual hardware yet.
