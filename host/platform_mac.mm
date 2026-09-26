// platform_mac.mm - macOS backend (Objective-C++).
//   Capture:   ScreenCaptureKit SCStream (hardware-accelerated, delivers only
//              when something changed). Needs the Screen Recording permission.
//   Input:     CGEvent injection at the HID event tap. Needs Accessibility.
//   Clipboard: NSPasteboard.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "platform.h"
#include "rd_proto.h"

namespace td {
class MacCapturer;
}

@interface TDStreamSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) td::FrameSlot *slot;
@end

@implementation TDStreamSink
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sb)) return;
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!atts || CFArrayGetCount(atts) == 0) return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(atts, 0);
    NSNumber *status = info[SCStreamFrameInfoStatus];
    // "Idle" frames mean nothing changed; the previous frame stays current.
    if (!status || status.integerValue != SCFrameStatusComplete) return;
    CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) return;

    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
    const size_t src_stride = CVPixelBufferGetBytesPerRow(pb);
    const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddress(pb);
    auto f = std::make_shared<td::Frame>();
    f->width = w;
    f->height = h;
    f->stride = w * 4;
    f->pixels.resize((size_t)f->stride * h);
    for (int y = 0; y < h; y++) memcpy(f->pixels.data() + (size_t)y * f->stride, base + y * src_stride, (size_t)w * 4);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    self.slot->publish(std::move(f));
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    std::fprintf(stderr, "[capture] stream stopped: %s\n", error.localizedDescription.UTF8String);
}
@end

namespace td {

namespace {

NSString *display_name(CGDirectDisplayID id) {
    for (NSScreen *s in [NSScreen screens]) {
        NSNumber *n = s.deviceDescription[@"NSScreenNumber"];
        if (n && n.unsignedIntValue == id) return s.localizedName;
    }
    return [NSString stringWithFormat:@"Display %u", id];
}

}  // namespace

class MacCapturer : public ScreenCapturer {
public:
    explicit MacCapturer(bool native_res) : native_res_(native_res) { sink_ = [TDStreamSink new]; sink_.slot = &slot_; }
    ~MacCapturer() override { stop(); }

    std::vector<DisplayInfo> displays() override {
        refresh_content();
        std::vector<DisplayInfo> out;
        for (SCDisplay *d in displays_) {
            auto [w, h] = capture_size(d);
            out.push_back({display_name(d.displayID).UTF8String, w, h});
        }
        return out;
    }

    int current_display() const override { return current_; }
    FramePtr latest() override { return slot_.get(); }

    bool start(int display, int max_fps, std::string &err) override {
        stop();
        if (!refresh_content()) {
            err = "ScreenCaptureKit returned no displays - grant Screen Recording permission to this app in "
                  "System Settings > Privacy & Security, then restart it";
            return false;
        }
        if (display < 0 || display >= (int)displays_.count) {
            err = "no such display";
            return false;
        }
        SCDisplay *d = displays_[display];
        auto [w, h] = capture_size(d);
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:d excludingWindows:@[]];
        SCStreamConfiguration *cfg = [SCStreamConfiguration new];
        cfg.width = w;
        cfg.height = h;
        cfg.pixelFormat = kCVPixelFormatType_32BGRA;
        cfg.minimumFrameInterval = CMTimeMake(1, std::max(1, std::min(120, max_fps)));
        cfg.showsCursor = YES;
        cfg.queueDepth = 4;
        cfg.colorSpaceName = kCGColorSpaceSRGB;

        stream_ = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:sink_];
        NSError *addErr = nil;
        queue_ = dispatch_queue_create("tetherdesk.capture", DISPATCH_QUEUE_SERIAL);
        if (![stream_ addStreamOutput:sink_ type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&addErr]) {
            err = std::string("addStreamOutput failed: ") + addErr.localizedDescription.UTF8String;
            stream_ = nil;
            return false;
        }
        __block NSError *startErr = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError *e) {
          startErr = e;
          dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        if (startErr) {
            err = std::string("capture failed to start: ") + startErr.localizedDescription.UTF8String;
            stream_ = nil;
            return false;
        }
        current_ = display;
        bounds_ = CGDisplayBounds(d.displayID);
        cap_w_ = w;
        cap_h_ = h;
        return true;
    }

    void stop() override {
        if (stream_) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [stream_ stopCaptureWithCompletionHandler:^(NSError *) {
              dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
            stream_ = nil;
        }
        slot_.clear();
    }

    // Global (points) rectangle of the captured display + capture size.
    void geometry(CGRect &bounds, int &w, int &h) const {
        bounds = bounds_;
        w = cap_w_;
        h = cap_h_;
    }

private:
    std::pair<int, int> capture_size(SCDisplay *d) {
        if (!native_res_) return {(int)d.width, (int)d.height};
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(d.displayID);
        int w = mode ? (int)CGDisplayModeGetPixelWidth(mode) : (int)d.width;
        int h = mode ? (int)CGDisplayModeGetPixelHeight(mode) : (int)d.height;
        if (mode) CGDisplayModeRelease(mode);
        return {w, h};
    }

    bool refresh_content() {
        __block SCShareableContent *content = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                                   onScreenWindowsOnly:YES
                                                     completionHandler:^(SCShareableContent *c, NSError *) {
                                                       content = c;
                                                       dispatch_semaphore_signal(sem);
                                                     }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        displays_ = content.displays ?: @[];
        return displays_.count > 0;
    }

    bool native_res_;
    TDStreamSink *sink_;
    SCStream *stream_ = nil;
    dispatch_queue_t queue_ = nil;
    NSArray<SCDisplay *> *displays_ = @[];
    FrameSlot slot_;
    int current_ = 0;
    CGRect bounds_ = CGRectZero;
    int cap_w_ = 1, cap_h_ = 1;
};

class MacInput : public InputInjector {
public:
    explicit MacInput(MacCapturer *cap) : cap_(cap) {
        src_ = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    }
    ~MacInput() override {
        release_all();
        if (src_) CFRelease(src_);
    }

    void pointer(int x, int y, uint8_t buttons, int wheel_x, int wheel_y) override {
        CGRect b;
        int cw, ch;
        cap_->geometry(b, cw, ch);
        CGPoint p = CGPointMake(b.origin.x + (x + 0.5) * b.size.width / std::max(1, cw),
                                b.origin.y + (y + 0.5) * b.size.height / std::max(1, ch));

        struct Btn {
            uint8_t bit;
            CGEventType down, up, drag;
            CGMouseButton num;
        };
        static const Btn kBtns[] = {
            {RD_BTN_LEFT, kCGEventLeftMouseDown, kCGEventLeftMouseUp, kCGEventLeftMouseDragged, kCGMouseButtonLeft},
            {RD_BTN_RIGHT, kCGEventRightMouseDown, kCGEventRightMouseUp, kCGEventRightMouseDragged,
             kCGMouseButtonRight},
            {RD_BTN_MIDDLE, kCGEventOtherMouseDown, kCGEventOtherMouseUp, kCGEventOtherMouseDragged,
             kCGMouseButtonCenter},
        };

        bool moved = p.x != last_.x || p.y != last_.y;
        if (moved) {
            CGEventType t = kCGEventMouseMoved;
            CGMouseButton num = kCGMouseButtonLeft;
            for (const Btn &bt : kBtns)
                if (buttons_ & bt.bit) {
                    t = bt.drag;
                    num = bt.num;
                    break;
                }
            post_mouse(t, p, num, 0);
            last_ = p;
        }
        for (const Btn &bt : kBtns) {
            bool was = buttons_ & bt.bit, now = buttons & bt.bit;
            if (was == now) continue;
            if (now) {
                // Track multi-clicks so double/triple-click selection works.
                double t = CFAbsoluteTimeGetCurrent();
                bool again = bt.bit == click_btn_ && t - click_time_ < 0.45 && fabs(p.x - click_pos_.x) < 5 &&
                             fabs(p.y - click_pos_.y) < 5;
                click_count_ = again ? click_count_ + 1 : 1;
                click_btn_ = bt.bit;
                click_time_ = t;
                click_pos_ = p;
            }
            post_mouse(now ? bt.down : bt.up, p, bt.num, click_count_);
        }
        buttons_ = buttons;

        if (wheel_x || wheel_y) {
            CGEventRef e = CGEventCreateScrollWheelEvent2(src_, kCGScrollEventUnitLine, 2, wheel_y, wheel_x, 0);
            if (e) {
                CGEventSetFlags(e, flags_);
                CGEventPost(kCGHIDEventTap, e);
                CFRelease(e);
            }
        }
    }

    void key(uint16_t hid, bool down) override {
        int code = hid_to_mac_keycode(hid);
        if (code < 0) return;
        CGEventFlags bit = 0;
        switch (hid) {
        case RD_HID_LSHIFT: case RD_HID_RSHIFT: bit = kCGEventFlagMaskShift; break;
        case RD_HID_LCTRL: case RD_HID_RCTRL: bit = kCGEventFlagMaskControl; break;
        case RD_HID_LALT: case RD_HID_RALT: bit = kCGEventFlagMaskAlternate; break;
        case RD_HID_LGUI: case RD_HID_RGUI: bit = kCGEventFlagMaskCommand; break;
        }
        if (bit) flags_ = down ? (flags_ | bit) : (flags_ & ~bit);
        CGEventRef e = CGEventCreateKeyboardEvent(src_, (CGKeyCode)code, down);
        if (!e) return;
        if (bit) CGEventSetType(e, kCGEventFlagsChanged);
        CGEventSetFlags(e, flags_);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        if (down) held_.push_back(hid);
        else held_.erase(std::remove(held_.begin(), held_.end(), hid), held_.end());
    }

    void release_all() override {
        auto keys = held_;
        for (uint16_t k : keys) key(k, false);
        held_.clear();
        if (buttons_) {
            CGRect b;
            int cw, ch;
            cap_->geometry(b, cw, ch);
            pointer(int((last_.x - b.origin.x) * cw / std::max(1.0, b.size.width)),
                    int((last_.y - b.origin.y) * ch / std::max(1.0, b.size.height)), 0, 0, 0);
        }
        flags_ = 0;
    }

private:
    void post_mouse(CGEventType t, CGPoint p, CGMouseButton num, int clicks) {
        CGEventRef e = CGEventCreateMouseEvent(src_, t, p, num);
        if (!e) return;
        if (clicks) CGEventSetIntegerValueField(e, kCGMouseEventClickState, clicks);
        CGEventSetFlags(e, flags_);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    MacCapturer *cap_;
    CGEventSourceRef src_ = nullptr;
    CGPoint last_ = CGPointMake(-1, -1);
    uint8_t buttons_ = 0;
    CGEventFlags flags_ = 0;
    std::vector<uint16_t> held_;
    uint8_t click_btn_ = 0;
    int click_count_ = 1;
    double click_time_ = 0;
    CGPoint click_pos_ = CGPointZero;
};

class MacClipboard : public Clipboard {
public:
    uint64_t change_count() override {
        @autoreleasepool {
            return (uint64_t)[NSPasteboard generalPasteboard].changeCount;
        }
    }
    bool get_text(std::string &out) override {
        @autoreleasepool {
            NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
            if (!s) return false;
            out = s.UTF8String ?: "";
            return true;
        }
    }
    void set_text(const std::string &text) override {
        @autoreleasepool {
            NSPasteboard *pb = [NSPasteboard generalPasteboard];
            [pb clearContents];
            [pb setString:[NSString stringWithUTF8String:text.c_str()] ?: @"" forType:NSPasteboardTypeString];
        }
    }
};

bool create_native_platform(Platform &out, const PlatformOptions &opts, std::string &err) {
    (void)CGMainDisplayID();  // initialise the window-server connection
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
        err = "Screen Recording permission is required. macOS just asked for it - enable TetherDesk (or your "
              "terminal) in System Settings > Privacy & Security > Screen Recording, then run the host again.\n"
              "Tip: `tetherdesk-host --demo` works without any permissions.";
        return false;
    }
    // CGPreflightPostEventAccess is the check that matches what we actually do
    // (post CGEvents). AXIsProcessTrusted can report false for the helper
    // process the app spawns even when Accessibility is granted.
    if (!CGPreflightPostEventAccess()) {
        CGRequestPostEventAccess();
        std::fprintf(stderr,
                     "[warn] Accessibility permission missing: viewers can watch but mouse/keyboard input will be "
                     "ignored by macOS until you allow it in Privacy & Security > Accessibility.\n");
    }
    auto cap = std::make_unique<MacCapturer>(opts.native_resolution);
    out.input = std::make_unique<MacInput>(cap.get());
    out.capturer = std::move(cap);
    out.clipboard = std::make_unique<MacClipboard>();
    NSOperatingSystemVersion v = [NSProcessInfo processInfo].operatingSystemVersion;
    char os[64];
    std::snprintf(os, sizeof os, "macOS %ld.%ld", (long)v.majorVersion, (long)v.minorVersion);
    out.os_name = os;
    return true;
}

}  // namespace td
