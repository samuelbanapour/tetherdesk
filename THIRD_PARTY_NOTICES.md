# Third-party notices

TetherDesk's own code is proprietary (see `LICENSE`). The following
third-party components are used under their own licenses. These licenses
apply only to those components.

| Component | Used for | License |
|---|---|---|
| [SDL 2](https://libsdl.org) | windowing, input and rendering in the viewer | zlib License |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | TLS connections to the relay | Apache License 2.0 |
| [Emscripten](https://emscripten.org) runtime | the browser viewer | MIT / University of Illinois NCSA |
| Aileron font (via Pillow) | the rasterized UI font in `common/rd_font_data.h` | CC0 1.0 (public domain) |
| TweetNaCl / poly1305-donna | reference designs for `common/rd_secure.c` | Public domain |

The Mbed TLS license text is at
https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-3.6.2/LICENSE, and the SDL
license text is at https://github.com/libsdl-org/SDL/blob/release-2.30.9/LICENSE.txt.
