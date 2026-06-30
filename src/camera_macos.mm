// macOS camera backend via AVFoundation (Objective-C++).
//
// AVFoundation delivers frames asynchronously on a dispatch queue; we convert
// the latest one to top-down 32-bit BGRA (the format the rest of the pipeline
// expects) and hand it to grab() through a small mutex/condition-variable
// handoff. We request kCVPixelFormatType_32BGRA from the capture output so no
// manual color conversion is needed. Compiled only on the Apple build.

#include "camera.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>

namespace dd {
namespace {

// Shared state written by the capture delegate, read by grab().
struct FrameState {
    std::mutex              m;
    std::condition_variable cv;
    Frame                   latest;
    bool                    hasNew = false;
    bool                    flip   = false;
};

} // namespace
} // namespace dd

// Capture delegate: converts each sample buffer into the shared FrameState.
@interface DDFrameGrabber : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) dd::FrameState* state;
@end

@implementation DDFrameGrabber
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    (void)output; (void)connection;
    CVImageBufferRef img = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!img) return;

    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    const int w = (int)CVPixelBufferGetWidth(img);
    const int h = (int)CVPixelBufferGetHeight(img);
    const size_t stride = CVPixelBufferGetBytesPerRow(img);
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(img);

    dd::FrameState* st = self.state;
    if (base && w > 0 && h > 0 && st) {
        std::lock_guard<std::mutex> lk(st->m);
        dd::Frame& f = st->latest;
        f.width = w; f.height = h;
        f.bgra.resize((size_t)w * h * 4);
        const size_t rowBytes = (size_t)w * 4;
        for (int y = 0; y < h; ++y) {
            int dy = st->flip ? (h - 1 - y) : y;
            std::memcpy(&f.bgra[(size_t)dy * rowBytes],
                        base + (size_t)y * stride, rowBytes);
        }
        st->hasNew = true;
        st->cv.notify_one();
    }
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}
@end

namespace dd {
namespace {

class AvfCamera : public ICamera {
public:
    explicit AvfCamera(bool flip) { state_.flip = flip; }

    ~AvfCamera() override {
        if (session_) {
            [session_ stopRunning];
            [session_ release];
        }
        [delegate_ release];
        if (queue_) dispatch_release(queue_);
    }

    bool open(const CameraConfig& cfg) {
        @autoreleasepool {
            if (!requestAccess()) return false;

            AVCaptureDevice* device = pickDevice(cfg);
            if (!device) {
                std::cerr << "[camera] No video capture device found.\n";
                return false;
            }
            std::cout << "Camera: " << [[device localizedName] UTF8String] << "\n";

            NSError* err = nil;
            AVCaptureDeviceInput* input =
                [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
            if (!input || err) {
                std::cerr << "[camera] Cannot open device input: "
                          << (err ? [[err localizedDescription] UTF8String] : "unknown") << "\n";
                return false;
            }

            session_ = [[AVCaptureSession alloc] init];
            applyPreset(cfg);
            if (![session_ canAddInput:input]) {
                std::cerr << "[camera] Cannot add camera input to session.\n";
                return false;
            }
            [session_ addInput:input];

            AVCaptureVideoDataOutput* out = [[[AVCaptureVideoDataOutput alloc] init] autorelease];
            out.videoSettings = @{
                (NSString*)kCVPixelBufferPixelFormatTypeKey :
                    @(kCVPixelFormatType_32BGRA)
            };
            out.alwaysDiscardsLateVideoFrames = YES;

            delegate_ = [[DDFrameGrabber alloc] init];
            delegate_.state = &state_;
            queue_ = dispatch_queue_create("dd.camera.queue", DISPATCH_QUEUE_SERIAL);
            [out setSampleBufferDelegate:delegate_ queue:queue_];

            if (![session_ canAddOutput:out]) {
                std::cerr << "[camera] Cannot add video output to session.\n";
                return false;
            }
            [session_ addOutput:out];

            [session_ startRunning];
            std::cout << "Capture started (32BGRA); waiting for first frame...\n";
            return waitFirstFrame();
        }
    }

    bool grab(Frame& out) override {
        std::unique_lock<std::mutex> lk(state_.m);
        if (!state_.cv.wait_for(lk, std::chrono::seconds(5),
                                [&] { return state_.hasNew; })) {
            std::cerr << "[camera] Timed out waiting for a frame.\n";
            return false;
        }
        out = state_.latest;        // copy current frame out
        state_.hasNew = false;
        w_ = out.width; h_ = out.height;
        return !out.empty();
    }

    int width()  const override { return w_; }
    int height() const override { return h_; }

private:
    bool requestAccess() {
        AVAuthorizationStatus st =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (st == AVAuthorizationStatusAuthorized) return true;
        if (st == AVAuthorizationStatusDenied || st == AVAuthorizationStatusRestricted) {
            std::cerr << "[camera] Camera access denied. Grant it in "
                         "System Settings > Privacy & Security > Camera "
                         "(for your terminal app).\n";
            return false;
        }
        // NotDetermined: ask, and block until the user responds.
        __block BOOL granted = NO;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL g) {
            granted = g;
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        dispatch_release(sem);
        if (!granted)
            std::cerr << "[camera] Camera access was not granted.\n";
        return granted;
    }

    AVCaptureDevice* pickDevice(const CameraConfig& cfg) {
        NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray array];
        [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
        if (@available(macOS 14.0, *))
            [types addObject:AVCaptureDeviceTypeExternal];
        AVCaptureDeviceDiscoverySession* disc =
            [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:types
                                      mediaType:AVMediaTypeVideo
                                       position:AVCaptureDevicePositionUnspecified];
        NSArray<AVCaptureDevice*>* devices = disc.devices;

        if (cfg.deviceIndexOverride >= 0) {
            if ((NSUInteger)cfg.deviceIndexOverride < devices.count)
                return devices[cfg.deviceIndexOverride];
            std::cerr << "[camera] --device " << cfg.deviceIndexOverride
                      << " out of range (" << (int)devices.count << " found).\n";
            return nil;
        }
        // Default to the system's built-in camera.
        AVCaptureDevice* def = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (def) return def;
        return devices.count ? devices.firstObject : nil;
    }

    void applyPreset(const CameraConfig& cfg) {
        NSString* preset = AVCaptureSessionPresetHigh;
        if (cfg.requestHeight >= 1080 && cfg.requestWidth >= 1920)
            preset = AVCaptureSessionPreset1920x1080;
        else if (cfg.requestHeight >= 720 && cfg.requestWidth >= 1280)
            preset = AVCaptureSessionPreset1280x720;
        else if (cfg.requestHeight >= 480)
            preset = AVCaptureSessionPreset640x480;
        if ([session_ canSetSessionPreset:preset])
            session_.sessionPreset = preset;
    }

    bool waitFirstFrame() {
        std::unique_lock<std::mutex> lk(state_.m);
        if (!state_.cv.wait_for(lk, std::chrono::seconds(10),
                                [&] { return state_.hasNew; })) {
            std::cerr << "[camera] No frame within 10s of starting capture.\n";
            return false;
        }
        w_ = state_.latest.width; h_ = state_.latest.height;
        std::cout << "First frame: " << w_ << "x" << h_ << "\n";
        return true;
    }

    AVCaptureSession* session_ = nil;
    DDFrameGrabber*   delegate_ = nil;
    dispatch_queue_t  queue_    = nullptr;
    FrameState        state_;
    int w_ = 0, h_ = 0;
};

} // namespace

std::unique_ptr<ICamera> CreateCamera(const CameraConfig& cfg) {
    auto cam = std::make_unique<AvfCamera>(cfg.flipVertical);
    if (!cam->open(cfg)) return nullptr;
    return cam;
}

} // namespace dd
