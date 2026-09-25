# TetherDesk

A remote desktop app written in C and C++. One host shares its screen, and viewers can connect in two ways:

- **In a browser.** The host serves the viewer itself: open `http://host:5980/`. The viewer is C++ compiled to WebAssembly with Emscripten.
- **With a native app.** This is the same C++ viewer built against SDL2, for macOS, Linux and Windows.

Both viewers use the same protocol, which runs over WebSocket.

```
tetherdesk/
  common/   C11: wire protocol, tile codec + LZ compressor, SHA-1/SHA-256/HMAC,
            WebSocket framing, portable sockets, embedded font (no dependencies)
  host/     C++17 host: poll() event loop, HTTP + WebSocket server, sessions,
            parallel tile encoder, platform backends:
              platform_mac.mm   ScreenCaptureKit + CGEvent + NSPasteboard (Objective-C++)
              platform_win.cpp  GDI BitBlt + SendInput + Win32 clipboard
              platform_x11.cpp  XGetImage + XTest (Linux/X11)
              platform_demo.cpp synthetic interactive desktop (no permissions needed)
  client/   C++17 viewer (SDL2), shared by native and web builds
              transport_native.cpp  TCP + our own WebSocket client
              transport_web.cpp     browser WebSocket via Emscripten
  tools/    tetherdesk-probe (headless test client), gen_font.py (build-time only)
  tests/    codec / crypto / WebSocket self-tests
```

## Build

You need CMake and a C/C++ compiler. The native viewer also needs SDL2 (`brew install sdl2`). The web viewer needs Emscripten (`brew install emscripten`).

```bash
./build.sh          # host + native viewer + web viewer -> build/
./build.sh test     # native build + unit tests
```

## Run

```bash
build/tetherdesk-host                    # share this screen (prints a generated password + URLs)
build/tetherdesk-host --demo             # share a synthetic demo desktop instead
build/tetherdesk 192.168.1.20 --password abcde-fghjk     # native viewer
```

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
- **Security:** the password is checked with HMAC-SHA256 challenge/response, so it never crosses the network. An address is locked out after 5 wrong passwords, with the lockout growing each time. Uploaded file names are sanitised, and all decoders check bounds on untrusted input.

## Security note

The screen stream is **not encrypted**. Only authentication is protected. Use TetherDesk on a trusted network. To use it across the internet, tunnel it:

```bash
ssh -L 5980:localhost:5980 you@host
```

Then start the host with `--bind 127.0.0.1` and connect the viewer to `localhost`.

## Known limitations

- **Linux:** only X11 is supported (not Wayland). Clipboard sync is not implemented on Linux.
- **Web viewer:** it cannot read or write the browser's system clipboard (that would need JavaScript). Some browser shortcuts, such as Cmd+W, are caught by the browser and never reach the remote machine.
- **Windows:** Ctrl+Alt+Del cannot be injected with SendInput.
- **Untested platforms:** the Windows backend has not been compiled or run. The X11 backend has only been compile-checked.
