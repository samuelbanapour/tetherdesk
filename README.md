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
  relay/    internet relay server (C++), deploy-ec2.sh, Dockerfile, QuickSupport download page
  tools/    tetherdesk-probe (headless test client), gen_font.py / gen_icon.py (build-time only)
  tests/    codec / crypto / WebSocket self-tests
```

## Install

Download from the [Releases page](../../releases):

- **macOS 12.3+:** `TetherDesk-macOS.dmg`. Open it and drag TetherDesk to Applications. The app isn't notarized, so the first time you open it, right-click it and choose **Open**. After each update, macOS asks you to allow Screen Recording and Accessibility again. Keeping those approvals across updates needs an Apple Developer ID certificate; `packaging/macos/sign.sh` supports one. Free self-signed certificates don't work: current macOS refuses permissions to them entirely.
- **Windows 10/11:** `TetherDesk-Setup.exe`. SmartScreen may warn about an unknown publisher; choose **More info → Run anyway**.
- **Linux (X11):** `TetherDesk-linux-x64.zip`, containing the binaries.

There is a single release (`v1.0.0`). CI replaces its files on every push to `main`, so the download links always get the newest build.

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

Prebuilt Windows, Linux and macOS downloads are on the
[release page](../../releases/latest) (built by `.github/workflows/build.yml`).

### Helping someone: nothing to install, no VPN

1. Send them **https://tetherdesk.54-151-75-113.nip.io/get**. They download **TetherDesk QuickSupport**:
   - on Windows, a single `.exe`;
   - on a Mac, a zip containing the app.
2. They run it. There's no installer, and closing it ends the session. It shows an **ID** (like `728 857 467`) and a **password**.
3. You connect in one of two ways:
   - open **https://tetherdesk.54-151-75-113.nip.io/** in any browser and enter the ID and password, or
   - type the ID into TetherDesk's **Quick connect**.

This works across the internet with no port forwarding or VPN. Both computers make *outgoing* connections to the relay (`relay/`), which pairs them by ID.

The session stays end-to-end encrypted between the two computers, so the relay only ever forwards ciphertext. It can't see the screen or keystrokes, and it can't learn the password.

The relay runs on a small Ubuntu server behind Caddy, which provides HTTPS. `relay/deploy-ec2.sh` installs or updates it from the latest release as a sandboxed systemd service. `relay/Dockerfile` can host it anywhere Docker runs; it builds the web viewer and the relay from source.

To use a different relay, pass `--relay your.host` to the host and the viewer. Port 443 uses TLS, verified against the operating system's trusted root certificates.

### Controlling your own computers

In the app, turn on **Share this PC** on the computer you want to reach. Note its ID, and connect to it from anywhere by that ID. On the same network, its local address works too.

Tick **Always on** to keep a computer reachable all the time. Sharing then keeps running after TetherDesk is closed, starts when the computer starts, and restarts itself if it ever stops:

- **macOS:** a per-user LaunchAgent.
- **Windows:** a startup entry plus a watchdog.

The host reads its password from a private file, so it never appears in process listings.

### Website

The relay also serves the TetherDesk website (`relay/site/home.html`) at `/`, the browser viewer at `/app`, and the QuickSupport page at `/get`.

## Features

- **Screen updates:** the screen is split into 64×64 tiles and only changed tiles are sent. Each tile uses whichever encoding is smallest: solid colour, a palette of up to 256 colours with 1/2/4/8-bit indices, or predicted RGB with LZ compression. Tiles are encoded in parallel across CPU cores.
- **Quality:** each viewer gets flow control (at most 2 frames unacknowledged). Auto quality starts lossless; on a congested network it lowers the frame rate first and reduces colours only as a last resort. Anything sent with reduced colours is automatically re-sent losslessly once that part of the screen stops changing. You can also pick Lossless, High, Medium or Low and a frame rate from 5 to 60 fps.
- **Input:** mouse (including double-clicks and drags), wheel and keyboard. Keys are sent as USB HID codes and mapped to macOS, Windows or Linux key codes, so they work regardless of keyboard layout. Held keys are released when a viewer leaves or the viewer window loses focus.
- **Several viewers at once:** there is a viewer list, a view-only mode, and the operator can kick a viewer or remove their control.
- **Clipboard, chat and files:** clipboard text syncs both ways. Viewers and the host operator can chat. Dropping a file on the native viewer uploads it to `~/Downloads/TetherDesk` on the host, with a progress bar.
- **Multiple monitors:** when the remote computer has several screens, the top bar shows **Screen 1 2 3…** buttons (or press **F7** to go to the next one).
- **Sharpness:** Macs are captured at full Retina resolution by default; **Sharp / Fast** in the menu trades detail for bandwidth.
- **Display and view options:** The viewer can fit the screen to the window or show it 1:1 with panning, and has a stats overlay (fps, Mbit/s, round-trip time) and fullscreen.
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

## License

TetherDesk is proprietary: **all rights reserved**. The source is published for viewing only. See [LICENSE](LICENSE), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the open-source components it uses.
