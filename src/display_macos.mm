// macOS preview display via Cocoa/AppKit (Objective-C++).
//
// Mirrors the Windows GDI preview: a live window showing the camera frame with
// the target box, center crosshair, and an FPS/error HUD drawn on top. The app
// runs a simple polling loop (main.cpp), so present() pumps any pending AppKit
// events non-blocking and redraws. `--headless` skips the window entirely.
// Press ESC or close the window to quit. Compiled only on the Apple build.

#include "display.hpp"

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <memory>

// ---- Custom view: holds the latest frame as a CGImage + overlay, draws both.
@interface DDPreviewView : NSView {
    CGImageRef  image_;
    dd::Overlay ov_;
    int         imgW_, imgH_;
}
@property (nonatomic, assign) bool* closed;  // set on ESC
- (void)setImage:(CGImageRef)img width:(int)w height:(int)h overlay:(const dd::Overlay&)ov;
@end

@implementation DDPreviewView

- (BOOL)isFlipped { return YES; }            // top-left origin, matches frame coords
- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 53 && self.closed) *self.closed = true;  // ESC
}

- (void)setImage:(CGImageRef)img width:(int)w height:(int)h overlay:(const dd::Overlay&)ov {
    if (image_) CGImageRelease(image_);
    image_ = img;                            // takes ownership (already retained by caller)
    imgW_ = w; imgH_ = h;
    ov_ = ov;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = self.bounds;
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, b);
    if (!image_ || imgW_ <= 0 || imgH_ <= 0) return;

    // The view is flipped (top-left origin) so overlay coords map to frame
    // pixels directly, but CGContextDrawImage would then render the top-down
    // image upside down. Flip the CTM around just the image draw.
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, b.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, b, image_);
    CGContextRestoreGState(ctx);

    const CGFloat sx = b.size.width  / imgW_;
    const CGFloat sy = b.size.height / imgH_;

    // Center crosshair.
    CGContextSetRGBStrokeColor(ctx, 0.6, 0.6, 0.6, 0.8);
    CGContextSetLineWidth(ctx, 1.0);
    CGContextMoveToPoint(ctx, b.size.width / 2, 0);
    CGContextAddLineToPoint(ctx, b.size.width / 2, b.size.height);
    CGContextMoveToPoint(ctx, 0, b.size.height / 2);
    CGContextAddLineToPoint(ctx, b.size.width, b.size.height / 2);
    CGContextStrokePath(ctx);

    // Target box.
    if (ov_.hasTarget) {
        CGFloat x = (ov_.cx - ov_.w / 2) * sx;
        CGFloat y = (ov_.cy - ov_.h / 2) * sy;
        CGContextSetRGBStrokeColor(ctx, 0.1, 1.0, 0.2, 1.0);
        CGContextSetLineWidth(ctx, 2.0);
        CGContextStrokeRect(ctx, CGRectMake(x, y, ov_.w * sx, ov_.h * sy));
    }

    // HUD text.
    NSString* hud = ov_.hasTarget
        ? [NSString stringWithFormat:@"ex=%+.3f ey=%+.3f conf=%.2f  %.1f fps",
                                     ov_.ex, ov_.ey, ov_.conf, ov_.fps]
        : [NSString stringWithFormat:@"searching...  %.1f fps", ov_.fps];
    NSDictionary* attrs = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor greenColor]
    };
    [hud drawAtPoint:NSMakePoint(8, 6) withAttributes:attrs];
}

- (void)dealloc {
    if (image_) CGImageRelease(image_);
    [super dealloc];
}
@end

namespace dd {
namespace {

// Headless: no window, present() is a no-op (main() still prints the signal).
class NullDisplay : public IDisplay {
public:
    bool isOpen() const override { return true; }
    void present(const Frame&, const Overlay&) override {}
};

class CocoaDisplay : public IDisplay {
public:
    bool open() {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

            NSRect rect = NSMakeRect(0, 0, 960, 540);
            window_ = [[NSWindow alloc]
                initWithContentRect:rect
                          styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                     NSWindowStyleMaskResizable)
                            backing:NSBackingStoreBuffered
                              defer:NO];
            [window_ setTitle:@"Drone Detect"];
            [window_ setReleasedWhenClosed:NO];

            view_ = [[DDPreviewView alloc] initWithFrame:rect];
            view_.closed = &closed_;
            [window_ setContentView:view_];
            [window_ makeFirstResponder:view_];
            [window_ center];
            [window_ makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            return true;
        }
    }

    ~CocoaDisplay() override {
        @autoreleasepool {
            if (window_) { [window_ close]; [window_ release]; }
            [view_ release];
        }
    }

    bool isOpen() const override {
        return !closed_ && window_ && [window_ isVisible];
    }

    void present(const Frame& frame, const Overlay& ov) override {
        @autoreleasepool {
            if (!frame.empty()) {
                CGImageRef img = makeImage(frame);
                if (img) [view_ setImage:img width:frame.width height:frame.height overlay:ov];
            }
            [view_ setNeedsDisplay:YES];
            pumpEvents();
            [view_ displayIfNeeded];
        }
    }

private:
    static CGImageRef makeImage(const Frame& f) {
        // Copy pixels so the CGImage owns its data independent of `f`.
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, f.bgra.data(),
                                      (CFIndex)f.bgra.size());
        if (!data) return nullptr;
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        // BGRA bytes in memory == ARGB little-endian word, alpha ignored.
        CGImageRef img = CGImageCreate(
            (size_t)f.width, (size_t)f.height, 8, 32, (size_t)f.width * 4, cs,
            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
            prov, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(prov);
        CFRelease(data);
        return img;
    }

    void pumpEvents() {
        NSEvent* e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
            if (e.type == NSEventTypeKeyDown && e.keyCode == 53) closed_ = true; // ESC
            [NSApp sendEvent:e];
        }
    }

    NSWindow*       window_ = nil;
    DDPreviewView*  view_   = nil;
    bool            closed_ = false;
};

} // namespace

std::unique_ptr<IDisplay> CreateDisplay(bool headless) {
    if (headless) return std::make_unique<NullDisplay>();
    auto d = std::make_unique<CocoaDisplay>();
    if (!d->open()) {
        std::fprintf(stderr, "[display] Window init failed; running headless.\n");
        return std::make_unique<NullDisplay>();
    }
    return d;
}

} // namespace dd
