// td_probe.cpp - headless TetherDesk client for health checks and tests.
//
//   tetherdesk-probe HOST[:PORT] --password PW [--seconds N] [--snapshot out.bmp]
//                    [--type TEXT] [--quality 0-3] [--send-file PATH] [--copy] [--expect-fail]
//
// Connects with the native transport, authenticates, decodes the stream for
// N seconds, optionally types TEXT into the remote machine, prints stats and
// writes the final screen as a BMP. Exit code 0 on success.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../client/transport.h"
#include "rd_bytes.h"
#include "rd_codec.h"
#include "rd_crypto.h"
#include "rd_net.h"
#include "rd_proto.h"

namespace {

bool write_bmp(const char *path, const std::vector<uint32_t> &px, int w, int h) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t row = uint32_t(w) * 3 + 3 & ~3u, size = 54 + row * uint32_t(h);
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { std::memcpy(hdr + off, &v, 4); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * uint32_t(h));
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[size_t(y) * w + x];
            line[x * 3] = uint8_t(p);
            line[x * 3 + 1] = uint8_t(p >> 8);
            line[x * 3 + 2] = uint8_t(p >> 16);
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
    return true;
}

uint16_t ascii_to_hid(char c, bool &shift) {
    shift = false;
    if (c >= 'a' && c <= 'z') return uint16_t(4 + c - 'a');
    if (c >= 'A' && c <= 'Z') return shift = true, uint16_t(4 + c - 'A');
    if (c >= '1' && c <= '9') return uint16_t(30 + c - '1');
    if (c == '0') return 39;
    if (c == ' ') return 44;
    if (c == '\n') return 40;
    if (c == '.') return 55;
    if (c == ',') return 54;
    if (c == '-') return 45;
    if (c == '!') return shift = true, 30;
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::string host = "localhost", password, snapshot, type_text, send_file;
    int port = RD_DEFAULT_PORT, quality = 255;
    double seconds = 3;
    bool expect_fail = false, copy = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--password") password = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--snapshot") snapshot = next();
        else if (a == "--type") type_text = next();
        else if (a == "--quality") quality = std::atoi(next().c_str());
        else if (a == "--send-file") send_file = next();
        else if (a == "--copy") copy = true;  // press Ctrl+C after typing
        else if (a == "--expect-fail") expect_fail = true;
        else if (a[0] != '-') {
            size_t c = a.rfind(':');
            host = c == std::string::npos ? a : a.substr(0, c);
            if (c != std::string::npos) port = std::atoi(a.substr(c + 1).c_str());
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    auto t = td::Transport::create();
    t->connect(host, port);
    auto send = [&](rd_buf &b) {
        t->send(b.data, b.len);
        rd_buf_clear(&b);
    };
    rd_buf out;
    rd_buf_init(&out);

    enum { CONNECTING, HELLO, AUTH, LIVE } phase = CONNECTING;
    int w = 0, h = 0, frames = 0, tiles = 0, full_frames = 0, enc_count[4] = {0, 0, 0, 0}, decode_errors = 0;
    uint64_t bytes = 0;
    std::vector<uint32_t> fb;
    std::string auth_msg;
    bool typed = false, file_sent = send_file.empty(), file_ok = send_file.empty();
    std::string clipboard;
    const auto start = std::chrono::steady_clock::now();
    auto live_since = start;

    for (;;) {
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (phase == LIVE && std::chrono::duration<double>(std::chrono::steady_clock::now() - live_since).count() > seconds)
            break;
        if (el > seconds + 10) {
            std::fprintf(stderr, "timeout (phase %d)\n", phase);
            return 1;
        }
        std::vector<std::vector<uint8_t>> msgs;
        t->poll(msgs);
        if (phase == CONNECTING && t->state() == td::Transport::State::Open) {
            rd_buf_put_u8(&out, RD_C_HELLO);
            rd_buf_put_u16(&out, RD_PROTO_VERSION);
            rd_buf_put_str(&out, "probe");
            send(out);
            phase = HELLO;
        }
        for (auto &m : msgs) {
            rd_reader r;
            rd_reader_init(&r, m.data(), m.size());
            uint8_t type = rd_get_u8(&r);
            if (type == RD_S_HELLO) {
                rd_get_u16(&r);
                rd_get_u8(&r);
                const uint8_t *nonce = rd_get_bytes(&r, RD_NONCE_LEN);
                uint8_t mac[32];
                rd_hmac_sha256(password.data(), password.size(), nonce, RD_NONCE_LEN, mac);
                rd_buf_put_u8(&out, RD_C_AUTH);
                rd_buf_put(&out, mac, 32);
                send(out);
                phase = AUTH;
            } else if (type == RD_S_AUTH_RESULT) {
                bool ok = rd_get_u8(&r);
                rd_get_u8(&r);
                char text[256];
                rd_get_str(&r, text, sizeof text);
                auth_msg = text;
                if (!ok) {
                    std::printf("auth rejected: %s\n", text);
                    return expect_fail ? 0 : 1;
                }
                phase = LIVE;
                live_since = std::chrono::steady_clock::now();
                rd_buf_put_u8(&out, RD_C_SETTINGS);
                rd_buf_put_u8(&out, uint8_t(quality));
                rd_buf_put_u8(&out, 30);
                rd_buf_put_u8(&out, 0);
                send(out);
            } else if (type == RD_S_DISPLAY_INFO) {
                w = rd_get_u16(&r);
                h = rd_get_u16(&r);
                fb.assign(size_t(w) * h, 0xFF000000u);
            } else if (type == RD_S_FILE_RESULT) {
                rd_get_u32(&r);
                file_ok = rd_get_u8(&r) != 0;
                char text[1024];
                rd_get_str(&r, text, sizeof text);
                std::printf("file transfer: %s\n", text);
            } else if (type == RD_S_CLIPBOARD) {
                uint32_t len;
                const uint8_t *p = rd_get_blob(&r, &len);
                clipboard.assign(reinterpret_cast<const char *>(p), len);
            } else if (type == RD_S_FRAME) {
                uint32_t id = rd_get_u32(&r);
                uint8_t flags = rd_get_u8(&r);
                int n = rd_get_u16(&r);
                for (int i = 0; i < n; i++) {
                    int x = rd_get_u16(&r), y = rd_get_u16(&r), tw = rd_get_u16(&r), th = rd_get_u16(&r);
                    int enc = rd_get_u8(&r);
                    uint32_t len;
                    const uint8_t *p = rd_get_blob(&r, &len);
                    if (r.err || x + tw > w || y + th > h ||
                        rd_decode_tile(enc, p, len, tw, th, fb.data() + size_t(y) * w + x, size_t(w))) {
                        decode_errors++;
                        break;
                    }
                    enc_count[enc & 3]++;
                }
                frames++;
                tiles += n;
                bytes += m.size();
                full_frames += flags & RD_FRAME_FULL;
                rd_buf_put_u8(&out, RD_C_FRAME_ACK);
                rd_buf_put_u32(&out, id);
                send(out);
            }
        }
        if (phase == LIVE && !typed && (!type_text.empty() || copy) && frames > 0) {
            for (char c : type_text) {
                bool shift;
                uint16_t hid = ascii_to_hid(c, shift);
                if (!hid) continue;
                for (int down = 1; down >= 0; down--) {
                    if (shift && down) {
                        rd_buf_put_u8(&out, RD_C_KEY), rd_buf_put_u16(&out, RD_HID_LSHIFT), rd_buf_put_u8(&out, 1);
                        send(out);
                    }
                    rd_buf_put_u8(&out, RD_C_KEY);
                    rd_buf_put_u16(&out, hid);
                    rd_buf_put_u8(&out, uint8_t(down));
                    send(out);
                    if (shift && !down) {
                        rd_buf_put_u8(&out, RD_C_KEY), rd_buf_put_u16(&out, RD_HID_LSHIFT), rd_buf_put_u8(&out, 0);
                        send(out);
                    }
                }
            }
            if (copy)
                for (auto [hid, down] : {std::pair<int, int>{RD_HID_LCTRL, 1}, {6, 1}, {6, 0}, {RD_HID_LCTRL, 0}}) {
                    rd_buf_put_u8(&out, RD_C_KEY);
                    rd_buf_put_u16(&out, uint16_t(hid));
                    rd_buf_put_u8(&out, uint8_t(down));
                    send(out);
                }
            typed = true;
        }
        if (phase == LIVE && !file_sent && frames > 0) {
            FILE *f = std::fopen(send_file.c_str(), "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open %s\n", send_file.c_str());
                return 1;
            }
            std::vector<uint8_t> data;
            uint8_t chunk[RD_FILE_CHUNK];
            size_t n;
            while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + n);
            std::fclose(f);
            const char *slash = std::strrchr(send_file.c_str(), '/');
            rd_buf_put_u8(&out, RD_C_FILE_BEGIN);
            rd_buf_put_u32(&out, 1);
            rd_buf_put_u64(&out, data.size());
            rd_buf_put_str(&out, slash ? slash + 1 : send_file.c_str());
            send(out);
            for (size_t off = 0; off < data.size(); off += RD_FILE_CHUNK) {
                rd_buf_put_u8(&out, RD_C_FILE_CHUNK);
                rd_buf_put_u32(&out, 1);
                rd_buf_put(&out, data.data() + off, std::min<size_t>(RD_FILE_CHUNK, data.size() - off));
                send(out);
            }
            rd_buf_put_u8(&out, RD_C_FILE_END);
            rd_buf_put_u32(&out, 1);
            send(out);
            file_sent = true;
        }
        if (t->state() == td::Transport::State::Closed) {
            std::fprintf(stderr, "connection closed: %s\n", t->error().c_str());
            return expect_fail ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::printf("%s: %dx%d, %d frames (%d full), %d tiles [solid %d, palette %d, zrgb %d, raw %d], %.2f MB, "
                "%.1f fps, %d decode errors\n",
                auth_msg.c_str(), w, h, frames, full_frames, tiles, enc_count[0], enc_count[1], enc_count[2],
                enc_count[3], bytes / 1e6, frames / seconds, decode_errors);
    if (!clipboard.empty()) std::printf("host clipboard: \"%s\"\n", clipboard.substr(0, 80).c_str());
    if (!snapshot.empty() && w && write_bmp(snapshot.c_str(), fb, w, h)) std::printf("snapshot: %s\n", snapshot.c_str());
    t->close();
    rd_buf_free(&out);
    return (expect_fail || decode_errors || !frames || !file_ok) ? 1 : 0;
}
