// Windows GDI preview window with target overlay. Also provides a headless
// no-op display for when --headless is passed.

#include <windows.h>
#include "display.hpp"
#include <algorithm>
#include <string>

namespace dd {
namespace {

class NullDisplay : public IDisplay {
public:
    bool isOpen() const override { return true; }
    void present(const Frame&, const Overlay&) override {}
};

class WinDisplay : public IDisplay {
public:
    bool create() {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = &WinDisplay::WndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DroneDetectPreview";
        RegisterClassW(&wc);

        hwnd_ = CreateWindowExW(
            0, wc.lpszClassName, L"Drone Detect - press ESC to quit",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
            nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOW);
        return true;
    }

    bool isOpen() const override { return open_; }

    void present(const Frame& frame, const Overlay& ov) override {
        pump();
        if (!open_ || frame.empty()) return;

        RECT rc; GetClientRect(hwnd_, &rc);
        int cw = rc.right, ch = rc.bottom;
        if (cw <= 0 || ch <= 0) return;

        HDC dc = GetDC(hwnd_);
        ensureBackbuffer(dc, cw, ch);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = frame.width;
        bmi.bmiHeader.biHeight      = -frame.height;   // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(memDC_, COLORONCOLOR);
        StretchDIBits(memDC_, 0, 0, cw, ch, 0, 0, frame.width, frame.height,
                      frame.bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

        drawOverlay(memDC_, cw, ch, frame, ov);
        BitBlt(dc, 0, 0, cw, ch, memDC_, 0, 0, SRCCOPY);
        ReleaseDC(hwnd_, dc);
    }

    ~WinDisplay() override {
        if (memBmp_) DeleteObject(memBmp_);
        if (memDC_)  DeleteDC(memDC_);
        if (hwnd_)   DestroyWindow(hwnd_);
    }

private:
    void pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void ensureBackbuffer(HDC dc, int cw, int ch) {
        if (memDC_ && cw == memW_ && ch == memH_) return;
        if (memBmp_) DeleteObject(memBmp_);
        if (memDC_)  DeleteDC(memDC_);
        memDC_ = CreateCompatibleDC(dc);
        memBmp_ = CreateCompatibleBitmap(dc, cw, ch);
        SelectObject(memDC_, memBmp_);
        memW_ = cw; memH_ = ch;
    }

    void drawOverlay(HDC dc, int cw, int ch, const Frame& frame, const Overlay& ov) {
        const float sx = static_cast<float>(cw) / frame.width;
        const float sy = static_cast<float>(ch) / frame.height;

        // Center crosshair (gray).
        HPEN gray = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
        HGDIOBJ old = SelectObject(dc, gray);
        MoveToEx(dc, cw / 2 - 12, ch / 2, nullptr); LineTo(dc, cw / 2 + 12, ch / 2);
        MoveToEx(dc, cw / 2, ch / 2 - 12, nullptr); LineTo(dc, cw / 2, ch / 2 + 12);
        SelectObject(dc, old);
        DeleteObject(gray);

        SetBkMode(dc, TRANSPARENT);

        if (ov.hasTarget) {
            int x = static_cast<int>((ov.cx - ov.w / 2) * sx);
            int y = static_cast<int>((ov.cy - ov.h / 2) * sy);
            int w = static_cast<int>(ov.w * sx);
            int h = static_cast<int>(ov.h * sy);
            HPEN green = CreatePen(PS_SOLID, 2, RGB(0, 230, 0));
            old = SelectObject(dc, green);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, x, y, x + w, y + h);
            // Line from screen center to target center.
            int tcx = static_cast<int>(ov.cx * sx), tcy = static_cast<int>(ov.cy * sy);
            MoveToEx(dc, cw / 2, ch / 2, nullptr); LineTo(dc, tcx, tcy);
            SelectObject(dc, old);
            DeleteObject(green);
            SetTextColor(dc, RGB(0, 230, 0));
        } else {
            SetTextColor(dc, RGB(230, 60, 60));
        }

        wchar_t buf[256];
        if (ov.hasTarget)
            swprintf(buf, 256,
                     L"TARGET  ex=%+.3f  ey=%+.3f  conf=%.2f  %.1f fps",
                     ov.ex, ov.ey, ov.conf, ov.fps);
        else
            swprintf(buf, 256, L"searching...  %.1f fps", ov.fps);
        TextOutW(dc, 10, 8, buf, static_cast<int>(wcslen(buf)));
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        WinDisplay* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<WinDisplay*>(
                reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<WinDisplay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        switch (msg) {
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE && self) self->open_ = false;
                return 0;
            case WM_CLOSE:
                if (self) self->open_ = false;
                return 0;
            case WM_DESTROY:
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HDC  memDC_ = nullptr;
    HBITMAP memBmp_ = nullptr;
    int  memW_ = 0, memH_ = 0;
    bool open_ = true;
};

} // namespace

std::unique_ptr<IDisplay> CreateDisplay(bool headless) {
    if (headless) return std::make_unique<NullDisplay>();
    auto d = std::make_unique<WinDisplay>();
    if (!d->create()) return std::make_unique<NullDisplay>();
    return d;
}

} // namespace dd
