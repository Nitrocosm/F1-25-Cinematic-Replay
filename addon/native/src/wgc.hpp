// Windows Graphics Capture of one window (Windows 10 1903+): the window's own image, also while other windows cover
// it. The MinGW headers have no WinRT capture headers, so the few ABI interfaces used are declared here (their
// vtables in order, IInspectable's three methods first) and combase / d3d11 functions are loaded at run time.
// Use from one thread (the recorder's worker), with the Windows Runtime initialised there.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>

#include <cstdint>
#include <string>

namespace wgc {

struct SizeInt32 {
    int32_t Width, Height;
};
using HSTR = void *;  // HSTRING
using winrt_bool = unsigned char;

// IIDs (Windows SDK)
constexpr GUID IID_ItemInterop = {0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
constexpr GUID IID_Item = {0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
constexpr GUID IID_PoolStatics2 = {0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
constexpr GUID IID_Session2 = {0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
constexpr GUID IID_Session3 = {0xf2cdd966, 0x22ae, 0x5ea1, {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
constexpr GUID IID_D3DDevice = {0xa37624ab, 0x8d5f, 0x4650, {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};
constexpr GUID IID_DxgiAccess = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
constexpr GUID IID_Closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};

struct IItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND, REFIID, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR, REFIID, void **) = 0;
};
struct IItem : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void *, int64_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(int64_t) = 0;
};
struct IFrame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(int64_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32 *) = 0;
};
struct ISession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct ISession2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(winrt_bool *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(winrt_bool) = 0;
};
struct ISession3 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(winrt_bool *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(winrt_bool) = 0;
};
struct IPool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable *, int32_t, int32_t, SizeInt32) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IFrame **) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void *, int64_t *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(int64_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IItem *, ISession **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(void **) = 0;
};
struct IPoolStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable *device, int32_t pixel_format, int32_t buffers,
                                                         SizeInt32 size, IPool **result) = 0;
};
struct IDxgiAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID, void **) = 0;
};
struct IClosable : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

constexpr int32_t kBGRA8 = 87;  // DirectXPixelFormat.B8G8R8A8UIntNormalized

template <class T> void rel(T *&p)
{
    if (p)
        p->Release();
    p = nullptr;
}

class Capture {
  public:
    ~Capture() { stop(); }

    // Starts capturing `hwnd` with `dev`. False with `err` when Windows Graphics Capture isn't available.
    bool start(HWND hwnd, ID3D11Device *dev, std::string &err)
    {
        HMODULE combase = LoadLibraryW(L"combase.dll"), d3d = GetModuleHandleW(L"d3d11.dll");
        using Factory_t = HRESULT(WINAPI *)(HSTR, REFIID, void **);
        using CreateStr_t = HRESULT(WINAPI *)(const wchar_t *, UINT32, HSTR *);
        using DeleteStr_t = HRESULT(WINAPI *)(HSTR);
        using FromDxgi_t = HRESULT(WINAPI *)(IDXGIDevice *, IInspectable **);
        auto get_factory = combase ? reinterpret_cast<Factory_t>(GetProcAddress(combase, "RoGetActivationFactory")) : nullptr;
        auto create_str = combase ? reinterpret_cast<CreateStr_t>(GetProcAddress(combase, "WindowsCreateString")) : nullptr;
        auto delete_str = combase ? reinterpret_cast<DeleteStr_t>(GetProcAddress(combase, "WindowsDeleteString")) : nullptr;
        auto from_dxgi = d3d ? reinterpret_cast<FromDxgi_t>(GetProcAddress(d3d, "CreateDirect3D11DeviceFromDXGIDevice")) : nullptr;
        if (!get_factory || !create_str || !delete_str || !from_dxgi) {
            err = "Windows Graphics Capture is not available on this Windows";
            return false;
        }
        auto factory = [&](const wchar_t *cls, REFIID iid, void **out) {
            HSTR s = nullptr;
            HRESULT hr = create_str(cls, UINT32(wcslen(cls)), &s);
            if (SUCCEEDED(hr))
                hr = get_factory(s, iid, out);
            if (s)
                delete_str(s);
            return hr;
        };
        IItemInterop *interop = nullptr;
        HRESULT hr = factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", IID_ItemInterop,
                             reinterpret_cast<void **>(&interop));
        if (SUCCEEDED(hr))
            hr = interop->CreateForWindow(hwnd, IID_Item, reinterpret_cast<void **>(&item_));
        rel(interop);
        if (FAILED(hr) || !item_)
            return fail(err, "window capture unavailable", hr);
        item_->get_Size(&size_);
        IDXGIDevice *dxgi = nullptr;
        hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi));
        IInspectable *dev_insp = nullptr;
        if (SUCCEEDED(hr))
            hr = from_dxgi(dxgi, &dev_insp);
        rel(dxgi);
        if (SUCCEEDED(hr))
            hr = dev_insp->QueryInterface(IID_D3DDevice, reinterpret_cast<void **>(&device_));
        rel(dev_insp);
        if (FAILED(hr))
            return fail(err, "capture device", hr);
        IPoolStatics2 *statics = nullptr;
        hr = factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", IID_PoolStatics2,
                     reinterpret_cast<void **>(&statics));
        if (SUCCEEDED(hr))
            hr = statics->CreateFreeThreaded(device_, kBGRA8, 2, size_, &pool_);
        rel(statics);
        if (SUCCEEDED(hr))
            hr = pool_->CreateCaptureSession(item_, &session_);
        if (FAILED(hr))
            return fail(err, "capture session", hr);
        ISession2 *s2 = nullptr;  // no mouse cursor in the video
        if (SUCCEEDED(session_->QueryInterface(IID_Session2, reinterpret_cast<void **>(&s2))))
            s2->put_IsCursorCaptureEnabled(0);
        rel(s2);
        ISession3 *s3 = nullptr;  // no yellow border where Windows allows it (11)
        if (SUCCEEDED(session_->QueryInterface(IID_Session3, reinterpret_cast<void **>(&s3))))
            s3->put_IsBorderRequired(0);
        rel(s3);
        hr = session_->StartCapture();
        if (FAILED(hr))
            return fail(err, "capture start", hr);
        return true;
    }

    int width() const { return size_.Width; }
    int height() const { return size_.Height; }

    // Drops the frames already waiting (they may be older than the caller's request).
    void drain()
    {
        IFrame *f = nullptr;
        while (pool_ && SUCCEEDED(pool_->TryGetNextFrame(&f)) && f)
            close(f);
    }

    // The next frame's texture (AddRef'd) and its content size, or nullptr when none came within `ms`.
    ID3D11Texture2D *next(DWORD ms, SizeInt32 &content)
    {
        ULONGLONG t0 = GetTickCount64();
        while (pool_ && GetTickCount64() - t0 < ms) {
            IFrame *f = nullptr;
            if (SUCCEEDED(pool_->TryGetNextFrame(&f)) && f) {
                ID3D11Texture2D *tex = nullptr;
                IInspectable *surf = nullptr;
                f->get_ContentSize(&content);
                if (SUCCEEDED(f->get_Surface(&surf)) && surf) {
                    IDxgiAccess *acc = nullptr;
                    if (SUCCEEDED(surf->QueryInterface(IID_DxgiAccess, reinterpret_cast<void **>(&acc))))
                        acc->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
                    rel(acc);
                }
                rel(surf);
                close(f);
                if (tex)
                    return tex;
            }
            Sleep(1);
        }
        return nullptr;
    }

    void stop()
    {
        if (session_)
            close(session_);
        if (pool_)
            close(pool_);
        rel(device_);
        rel(item_);
    }

  private:
    IItem *item_ = nullptr;
    IInspectable *device_ = nullptr;
    IPool *pool_ = nullptr;
    ISession *session_ = nullptr;
    SizeInt32 size_{};

    bool fail(std::string &err, const char *what, HRESULT hr)
    {
        char b[96];
        snprintf(b, sizeof b, "%s (0x%08lx)", what, (unsigned long)hr);
        err = b;
        stop();
        return false;
    }
    template <class T> void close(T *&o)  // IClosable::Close, then release
    {
        IClosable *c = nullptr;
        if (o && SUCCEEDED(o->QueryInterface(IID_Closable, reinterpret_cast<void **>(&c)))) {
            c->Close();
            c->Release();
        }
        rel(o);
    }
};

} // namespace wgc
