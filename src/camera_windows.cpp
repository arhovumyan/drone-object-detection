// Windows camera backend using Media Foundation.
// Selects the BUILT-IN (integrated) camera only: it skips any device whose
// Windows removal policy is "removable" (i.e. plug-in USB / external).

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

#include "camera.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace dd {
namespace {

// Is this capture device physically built into the machine (non-removable)?
bool IsBuiltInCamera(const wchar_t* symbolicLink) {
    WCHAR instanceId[600];
    ULONG size = sizeof(instanceId);
    DEVPROPTYPE propType = 0;
    CONFIGRET cr = CM_Get_Device_Interface_PropertyW(
        symbolicLink, &DEVPKEY_Device_InstanceId, &propType,
        reinterpret_cast<PBYTE>(instanceId), &size, 0);
    if (cr != CR_SUCCESS) return false;

    DEVINST devinst;
    cr = CM_Locate_DevNodeW(&devinst, instanceId, CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS) return false;

    DWORD policy = 0;
    ULONG psize = sizeof(policy);
    DEVPROPTYPE ptype = 0;
    cr = CM_Get_DevNode_PropertyW(devinst, &DEVPKEY_Device_RemovalPolicy, &ptype,
                                  reinterpret_cast<PBYTE>(&policy), &psize, 0);
    if (cr != CR_SUCCESS) return false;

    // EXPECT_NO_REMOVAL == soldered-in / integrated.
    return policy == CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL;
}

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

class WinMfCamera : public ICamera {
public:
    explicit WinMfCamera(bool flipVertical) : flip_(flipVertical) {}

    ~WinMfCamera() override {
        reader_.Reset();
        source_.Reset();
        if (mfStarted_) MFShutdown();
        if (comStarted_) CoUninitialize();
    }

    bool open(const CameraConfig& cfg) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        comStarted_ = SUCCEEDED(hr);
        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) { std::cerr << "MFStartup failed\n"; return false; }
        mfStarted_ = true;

        if (!selectAndActivate(cfg)) return false;
        if (!createReader(cfg)) return false;
        return updateFormat();
    }

    bool grab(Frame& out) override {
        for (int tries = 0; tries < 16; ++tries) {
            DWORD streamIndex = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader_->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                &streamIndex, &flags, &ts, sample.GetAddressOf());
            if (FAILED(hr)) return false;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) updateFormat();
            if (!sample) continue;            // no frame ready yet
            return copySample(sample.Get(), out);
        }
        return false;
    }

    int width()  const override { return w_; }
    int height() const override { return h_; }

private:
    bool selectAndActivate(const CameraConfig& cfg) {
        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 1))) return false;
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count)) || count == 0) {
            std::cerr << "No video capture devices found.\n";
            return false;
        }

        int chosen = -1;
        std::cout << "Video devices:\n";
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = nullptr; UINT32 nlen = 0;
            WCHAR* sym  = nullptr; UINT32 slen = 0;
            devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nlen);
            devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &sym, &slen);
            bool builtIn = sym && IsBuiltInCamera(sym);
            std::cout << "  [" << i << "] " << Narrow(name)
                      << (builtIn ? "  (built-in)" : "  (removable/external)") << "\n";

            if (cfg.deviceIndexOverride >= 0) {
                if (static_cast<int>(i) == cfg.deviceIndexOverride) chosen = i;
            } else if (cfg.builtInOnly) {
                if (builtIn && chosen < 0) chosen = i;
            } else if (chosen < 0) {
                chosen = i;
            }
            CoTaskMemFree(name);
            CoTaskMemFree(sym);
        }

        bool ok = false;
        if (chosen < 0) {
            std::cerr << "No built-in camera found. "
                         "Use --no-builtin or --device N to pick one of the above.\n";
        } else {
            ok = SUCCEEDED(devices[chosen]->ActivateObject(IID_PPV_ARGS(source_.GetAddressOf())));
            if (ok) std::cout << "Using device [" << chosen << "]\n";
            else    std::cerr << "Failed to activate device [" << chosen << "]\n";
        }

        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        return ok;
    }

    bool createReader(const CameraConfig& cfg) {
        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 1);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (FAILED(MFCreateSourceReaderFromMediaSource(source_.Get(), ra.Get(),
                                                       reader_.GetAddressOf()))) {
            std::cerr << "MFCreateSourceReaderFromMediaSource failed\n";
            return false;
        }

        // Ask for RGB32 at the requested size; fall back to RGB32 at any size.
        auto trySet = [&](bool withSize) -> bool {
            ComPtr<IMFMediaType> mt;
            MFCreateMediaType(&mt);
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (withSize && cfg.requestWidth > 0 && cfg.requestHeight > 0)
                MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE,
                                   cfg.requestWidth, cfg.requestHeight);
            return SUCCEEDED(reader_->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.Get()));
        };
        if (!trySet(true) && !trySet(false)) {
            std::cerr << "Could not set RGB32 output format\n";
            return false;
        }
        return true;
    }

    bool updateFormat() {
        ComPtr<IMFMediaType> cur;
        if (FAILED(reader_->GetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.GetAddressOf())))
            return false;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
        w_ = static_cast<int>(w);
        h_ = static_cast<int>(h);
        UINT32 s = 0;
        stride_ = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))
                      ? static_cast<LONG>(s) : static_cast<LONG>(w_) * 4;
        if (stride_ == 0) stride_ = static_cast<LONG>(w_) * 4;
        std::cout << "Capture format: " << w_ << "x" << h_
                  << " RGB32 stride=" << stride_ << "\n";
        return w_ > 0 && h_ > 0;
    }

    bool copySample(IMFSample* sample, Frame& out) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) return false;
        BYTE* data = nullptr; DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen))) return false;

        out.width = w_;
        out.height = h_;
        out.bgra.resize(static_cast<size_t>(w_) * h_ * 4);
        const LONG absStride = stride_ < 0 ? -stride_ : stride_;
        const bool bottomUp = (stride_ < 0) ^ flip_;
        const size_t rowBytes = static_cast<size_t>(w_) * 4;
        for (int y = 0; y < h_; ++y) {
            int srcY = bottomUp ? (h_ - 1 - y) : y;
            const BYTE* srcRow = data + static_cast<size_t>(srcY) * absStride;
            std::memcpy(&out.bgra[static_cast<size_t>(y) * rowBytes], srcRow, rowBytes);
        }
        buf->Unlock();
        return true;
    }

    ComPtr<IMFMediaSource>  source_;
    ComPtr<IMFSourceReader> reader_;
    int  w_ = 0, h_ = 0;
    LONG stride_ = 0;
    bool flip_ = false;
    bool comStarted_ = false;
    bool mfStarted_ = false;
};

} // namespace

std::unique_ptr<ICamera> CreateCamera(const CameraConfig& cfg) {
    auto cam = std::make_unique<WinMfCamera>(cfg.flipVertical);
    if (!cam->open(cfg)) return nullptr;
    return cam;
}

} // namespace dd
