// Offline render output: grabs the game window's image from the screen (DXGI desktop duplication, the API OBS's
// display capture uses) and streams the frames into ffmpeg as raw BGRA, which encodes them to a video file.
//
// The plugin drives it frame by frame: it holds the replay still, calls request(n) and steps the replay only once
// done() reaches n. The capture thread takes the first desktop frame presented after the request, so the image is
// one rendered at the held replay time. Everything here runs on the recorder's own thread except the calls below.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wgc.hpp"

namespace render {

// A file that really opens: a link (WinGet's Links\ffmpeg.exe is one) is followed, so a link whose target is gone
// doesn't count (GetFileAttributes reports the link itself).
inline bool file_opens(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

// ffmpeg.exe: the configured path, else PATH, else WinGet's links folder, else a WinGet package folder. Empty when
// not found.
inline std::wstring find_ffmpeg(const std::wstring &configured)
{
    if (!configured.empty() && file_opens(configured))
        return configured;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr) && file_opens(buf))
        return buf;
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        std::wstring p = std::wstring(la) + L"\\Microsoft\\WinGet\\Links\\ffmpeg.exe";
        if (file_opens(p))
            return p;
        // a WinGet package folder (Gyan.FFmpeg ...\bin\ffmpeg.exe)
        WIN32_FIND_DATAW fd;
        std::wstring pk = std::wstring(la) + L"\\Microsoft\\WinGet\\Packages\\";
        HANDLE h = FindFirstFileW((pk + L"*FFmpeg*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring dir = pk + fd.cFileName + L"\\";
                WIN32_FIND_DATAW fd2;
                HANDLE h2 = FindFirstFileW((dir + L"ffmpeg-*").c_str(), &fd2);
                if (h2 != INVALID_HANDLE_VALUE) {
                    std::wstring exe = dir + fd2.cFileName + L"\\bin\\ffmpeg.exe";
                    FindClose(h2);
                    if (file_opens(exe)) {
                        FindClose(h);
                        return exe;
                    }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return {};
}

template <class T> void release(T *&p)
{
    if (p)
        p->Release();
    p = nullptr;
}

class Recorder {
  public:
    ~Recorder() { stop(); }

    // Starts capturing `hwnd`'s client area (Windows Graphics Capture: the window's own image, even when covered;
    // else, or with `screen_only`, the monitor it is on) and ffmpeg writing `out`. All capture work runs on the
    // recorder's own thread. blur > 1: every `blur` captures are averaged into one output frame (motion blur).
    // `format`: 0 H.264 (crf), 1 ProRes 422 HQ, 2 ProRes 4444 XQ (both 10-bit, for grading in post), 3 H.265 (crf).
    bool start(HWND hwnd, int fps, int crf, const std::wstring &ffmpeg, const std::wstring &out, std::string &err,
               int blur = 1, bool screen_only = false, int format = 0)
    {
        stop();
        format_ = format;
        hwnd_ = hwnd;
        blur_ = std::max(1, blur);
        acc_n_ = 0;
        screen_only_ = screen_only;
        running_ = true;
        requested_ = -1;
        done_ = -1;
        failed_ = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            ready_ = open_ok_ = false;
        }
        worker_ = std::thread([this] { run(); });
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait_for(lk, std::chrono::seconds(10), [&] { return ready_; });
            if (!open_ok_) {
                err = ready_ ? open_err_ : std::string("the capture did not start");
                lk.unlock();
                running_ = false;
                cv_.notify_all();
                worker_.join();
                return false;
            }
        }
        if (!start_ffmpeg(fps, crf, ffmpeg, out, err)) {
            running_ = false;
            cv_.notify_all();
            worker_.join();
            return false;
        }
        return true;
    }

    // The window capture (true) works while other windows cover the game; the screen capture needs it in front.
    bool window_capture() const { return use_wgc_; }

    // Captures the first frame presented after this call as frame n (asynchronously).
    void request(int n)
    {
        {
            std::lock_guard<std::mutex> lk(mx_);
            requested_ = n;
        }
        cv_.notify_all();
    }
    int done() const { return done_.load(); }
    bool failed() const { return failed_.load(); }
    bool active() const { return running_.load(); }
    int width() const { return w_; }
    int height() const { return h_; }
    std::string error()
    {
        std::lock_guard<std::mutex> lk(mx_);
        return error_;
    }

    // Ends the stream and waits for ffmpeg to finish the file (up to `wait_ms`). Returns ffmpeg's exit code (-1 if
    // it was still running or never started).
    int stop(DWORD wait_ms = 120000)
    {
        bool was = running_.exchange(false);
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
        if (pipe_) {
            CloseHandle(pipe_);
            pipe_ = nullptr;
        }
        int code = -1;
        if (proc_) {
            if (WaitForSingleObject(proc_, wait_ms) == WAIT_OBJECT_0) {
                DWORD c = 0;
                GetExitCodeProcess(proc_, &c);
                code = int(c);
            }
            CloseHandle(proc_);
            proc_ = nullptr;
        }
        (void)was;  // the capture objects are released by the worker as it ends
        return code;
    }

  private:
    HWND hwnd_ = nullptr;
    ID3D11Device *dev_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    IDXGIOutputDuplication *dup_ = nullptr;
    ID3D11Texture2D *staging_ = nullptr;
    RECT crop_{};  // in the output's desktop texture
    int w_ = 0, h_ = 0;
    HANDLE pipe_ = nullptr, proc_ = nullptr;
    std::vector<uint8_t> frame_;  // the last captured frame (BGRA, w * h * 4)
    std::vector<uint16_t> acc_;   // motion blur: the sum of this output frame's captures so far
    int blur_ = 1, acc_n_ = 0, format_ = 0;
    std::thread worker_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false}, failed_{false};
    int requested_ = -1;  // guarded by mx_
    std::atomic<int> done_{-1};
    std::string error_;  // guarded by mx_
    bool ready_ = false, open_ok_ = false;  // guarded by mx_: the worker opened the capture (or failed: open_err_)
    std::string open_err_;
    bool screen_only_ = false, use_wgc_ = false;
    wgc::Capture wgc_;

    void fail(const std::string &e)
    {
        {
            std::lock_guard<std::mutex> lk(mx_);
            error_ = e;
        }
        failed_ = true;
    }

    // The window capture: a device on the default adapter, the window's frames, cropped to its client area (a
    // windowed game's frame includes its border and title bar).
    bool open_wgc(std::string &err)
    {
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &dev_, &fl, &ctx_);
        if (FAILED(hr)) {
            err = "no capture device";
            return false;
        }
        if (!wgc_.start(hwnd_, dev_, err)) {
            close_duplication();
            return false;
        }
        // the client area inside the captured frame (the window's visible bounds)
        RECT frame{}, rc{};
        POINT tl{0, 0};
        using DwmAttr_t = HRESULT(WINAPI *)(HWND, DWORD, void *, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        auto attr = dwm ? reinterpret_cast<DwmAttr_t>(GetProcAddress(dwm, "DwmGetWindowAttribute")) : nullptr;
        LONG ox = 0, oy = 0;
        if (attr && SUCCEEDED(attr(hwnd_, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &frame, sizeof frame)) &&
            GetClientRect(hwnd_, &rc) && ClientToScreen(hwnd_, &tl)) {
            ox = std::max(0L, tl.x - frame.left);
            oy = std::max(0L, tl.y - frame.top);
        } else {
            GetClientRect(hwnd_, &rc);
        }
        LONG fw = wgc_.width(), fh = wgc_.height();
        crop_ = {ox, oy, std::min(fw, ox + rc.right), std::min(fh, oy + rc.bottom)};
        w_ = int(crop_.right - crop_.left) & ~1;
        h_ = int(crop_.bottom - crop_.top) & ~1;
        crop_.right = crop_.left + w_;
        crop_.bottom = crop_.top + h_;
        if (w_ < 64 || h_ < 64 || !make_staging()) {
            err = "the game window is too small to capture";
            wgc_.stop();
            close_duplication();
            return false;
        }
        return true;
    }

    bool make_staging()
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w_);
        td.Height = UINT(h_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &staging_));
    }

    // Copies `crop_` of a captured texture (clipped to `avail` width / height) into frame_.
    bool copy_tex(ID3D11Texture2D *tex, LONG avail_w, LONG avail_h)
    {
        LONG r = std::min(crop_.right, avail_w), b = std::min(crop_.bottom, avail_h);
        if (r <= crop_.left || b <= crop_.top)
            return false;
        D3D11_BOX box{UINT(crop_.left), UINT(crop_.top), 0, UINT(r), UINT(b), 1};
        ctx_->CopySubresourceRegion(staging_, 0, 0, 0, 0, tex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m)))
            return false;
        for (int y = 0; y < h_; y++)
            memcpy(frame_.data() + size_t(y) * size_t(w_) * 4, static_cast<uint8_t *>(m.pData) + size_t(y) * m.RowPitch,
                   size_t(w_) * 4);
        ctx_->Unmap(staging_, 0);
        return true;
    }

    bool open_duplication(std::string &err)
    {
        RECT rc;
        POINT tl{0, 0};
        if (!GetClientRect(hwnd_, &rc) || !ClientToScreen(hwnd_, &tl)) {
            err = "no game window";
            return false;
        }
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        IDXGIFactory1 *fac = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&fac)))) {
            err = "no DXGI factory";
            return false;
        }
        IDXGIAdapter1 *ad = nullptr;
        IDXGIOutput *out = nullptr;
        DXGI_OUTPUT_DESC od{};
        for (UINT a = 0; !out && fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
            IDXGIOutput *o = nullptr;
            for (UINT i = 0; ad->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; i++) {
                o->GetDesc(&od);
                if (od.Monitor == mon) {
                    out = o;
                    break;
                }
                release(o);
            }
            if (!out)
                release(ad);
        }
        release(fac);
        if (!out) {
            err = "the game's monitor was not found";
            return false;
        }
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev_,
                                       &fl, &ctx_);
        release(ad);
        IDXGIOutput1 *o1 = nullptr;
        if (SUCCEEDED(hr))
            hr = out->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&o1));
        release(out);
        if (SUCCEEDED(hr))
            hr = o1->DuplicateOutput(dev_, &dup_);
        release(o1);
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof b, "screen capture unavailable (0x%08lx)", (unsigned long)hr);
            err = b;
            close_duplication();
            return false;
        }
        DXGI_OUTDUPL_DESC dd{};
        dup_->GetDesc(&dd);
        if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            err = "the screen is not 8-bit BGRA (HDR on?): turn HDR off to render";
            close_duplication();
            return false;
        }
        // the client area in the output's texture, even-sized for yuv420p
        LONG x = tl.x - od.DesktopCoordinates.left, y = tl.y - od.DesktopCoordinates.top;
        LONG ow = LONG(dd.ModeDesc.Width), oh = LONG(dd.ModeDesc.Height);
        crop_ = {std::max(0L, x), std::max(0L, y), std::min(ow, x + rc.right), std::min(oh, y + rc.bottom)};
        w_ = int(crop_.right - crop_.left) & ~1;
        h_ = int(crop_.bottom - crop_.top) & ~1;
        crop_.right = crop_.left + w_;
        crop_.bottom = crop_.top + h_;
        if (w_ < 64 || h_ < 64) {
            err = "the game window is off its screen";
            close_duplication();
            return false;
        }
        if (!make_staging()) {
            err = "no staging texture";
            close_duplication();
            return false;
        }
        return true;
    }

    void close_duplication()
    {
        release(staging_);
        release(dup_);
        release(ctx_);
        release(dev_);
    }

    bool start_ffmpeg(int fps, int crf, const std::wstring &ffmpeg, const std::wstring &out, std::string &err)
    {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 22)) {
            err = "no pipe";
            return false;
        }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        std::wstring log = out + L".ffmpeg.log";
        HANDLE lg = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        wchar_t enc[256];
        if (format_ == 1)
            swprintf(enc, 256, L"-c:v prores_ks -profile:v 3 -vendor apl0 -pix_fmt yuv422p10le");
        else if (format_ == 2)
            swprintf(enc, 256, L"-c:v prores_ks -profile:v 5 -vendor apl0 -pix_fmt yuv444p10le");
        else if (format_ == 3)  // H.265: x265's CRF scale sits about 4 above x264's for the same look; hvc1 for Apple
            swprintf(enc, 256,
                     L"-c:v libx265 -preset medium -crf %d -pix_fmt yuv420p -tag:v hvc1 -x265-params log-level=error "
                     L"-movflags +faststart",
                     std::min(51, crf + 4));
        else
            swprintf(enc, 256, L"-c:v libx264 -preset slow -crf %d -pix_fmt yuv420p -movflags +faststart", crf);
        wchar_t args[2048];
        swprintf(args, 2048,
                 L"\"%ls\" -y -hide_banner -loglevel warning -f rawvideo -pix_fmt bgra -s %dx%d -framerate %d -i - "
                 L"%ls \"%ls\"",
                 ffmpeg.c_str(), w_, h_, fps, enc, out.c_str());
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = lg != INVALID_HANDLE_VALUE ? lg : nullptr;
        si.hStdError = lg != INVALID_HANDLE_VALUE ? lg : nullptr;
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, args, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi);
        CloseHandle(rd);
        if (lg != INVALID_HANDLE_VALUE)
            CloseHandle(lg);
        if (!ok) {
            CloseHandle(wr);
            err = "ffmpeg did not start";
            return false;
        }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        pipe_ = wr;
        return true;
    }

    // Copies the acquired desktop frame's crop into frame_.
    bool copy_frame(IDXGIResource *res)
    {
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))))
            return false;
        bool ok = copy_tex(tex, crop_.right, crop_.bottom);
        release(tex);
        return ok;
    }

    bool write_frame()
    {
        const uint8_t *p = frame_.data();
        size_t left = frame_.size();
        while (left) {
            DWORD n = 0;
            if (!WriteFile(pipe_, p, DWORD(std::min<size_t>(left, 1 << 22)), &n, nullptr) || !n)
                return false;
            p += n;
            left -= n;
        }
        return true;
    }

    // The worker: opens the capture here (the window capture needs its own Windows Runtime thread), then captures
    // what is asked for until stopped, and releases everything on the way out.
    void run()
    {
        using RoInit_t = HRESULT(WINAPI *)(int);
        using RoUninit_t = void(WINAPI *)();
        HMODULE combase = LoadLibraryW(L"combase.dll");
        auto ro_init = combase ? reinterpret_cast<RoInit_t>(GetProcAddress(combase, "RoInitialize")) : nullptr;
        auto ro_uninit = combase ? reinterpret_cast<RoUninit_t>(GetProcAddress(combase, "RoUninitialize")) : nullptr;
        bool ro = ro_init && SUCCEEDED(ro_init(1 /* multithreaded */));
        std::string err, wgc_err;
        use_wgc_ = !screen_only_ && open_wgc(wgc_err);
        bool ok = use_wgc_ || open_duplication(err);
        if (ok) {
            frame_.assign(size_t(w_) * size_t(h_) * 4, 0);
            acc_.assign(blur_ > 1 ? frame_.size() : 0, 0);
        }
        {
            std::lock_guard<std::mutex> lk(mx_);
            ready_ = true;
            open_ok_ = ok;
            open_err_ = wgc_err.empty() ? err : err + " (window capture: " + wgc_err + ")";
        }
        cv_.notify_all();
        if (ok)
            loop();
        wgc_.stop();
        close_duplication();
        if (ro && ro_uninit)
            ro_uninit();
    }

    // The next new image after now, into frame_ (false: none came within a second, frame_ keeps the last one).
    bool grab()
    {
        if (use_wgc_) {
            while (running_ && IsIconic(hwnd_))  // minimized: the window has no image; wait until it is back
                Sleep(50);
            wgc_.drain();  // frames already waiting may predate the request
            wgc::SizeInt32 cs{};
            ID3D11Texture2D *tex = wgc_.next(1000, cs);
            if (!tex)
                return false;
            bool ok = copy_tex(tex, cs.Width, cs.Height);
            release(tex);
            return ok;
        }
        // drop a frame already waiting (it may predate the request), then take the next one presented
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource *res = nullptr;
        if (dup_->AcquireNextFrame(0, &fi, &res) == S_OK) {
            release(res);
            dup_->ReleaseFrame();
        }
        bool got = false;
        ULONGLONG t0 = GetTickCount64();
        while (running_ && !got && GetTickCount64() - t0 < 1000) {
            HRESULT hr = dup_->AcquireNextFrame(100, &fi, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                continue;
            if (FAILED(hr)) {
                fail("screen capture lost (another app went fullscreen, or the display mode changed)");
                break;
            }
            if (fi.LastPresentTime.QuadPart != 0)  // a presented image, not only a cursor move
                got = copy_frame(res);
            release(res);
            dup_->ReleaseFrame();
        }
        return got;
    }

    void loop()
    {
        int handled = -1;
        while (running_) {
            int want;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [&] { return !running_ || requested_ > handled; });
                if (!running_)
                    break;
                want = requested_;
            }
            grab();
            if (failed_)
                break;
            // no new image within a second: the screen didn't change, so the last image is this frame too
            if (blur_ > 1) {  // add to this output frame; write the average after the last capture of it
                for (size_t i = 0; i < frame_.size(); i++)
                    acc_[i] = uint16_t(acc_[i] + frame_[i]);
                if (++acc_n_ < blur_) {
                    handled = want;
                    done_ = want;
                    continue;
                }
                for (size_t i = 0; i < frame_.size(); i++) {
                    frame_[i] = uint8_t((acc_[i] + acc_n_ / 2) / acc_n_);
                    acc_[i] = 0;
                }
                acc_n_ = 0;
            }
            if (!write_frame()) {
                fail("ffmpeg stopped taking frames (see the .ffmpeg.log next to the video)");
                break;
            }
            handled = want;
            done_ = want;
        }
    }
};

} // namespace render
