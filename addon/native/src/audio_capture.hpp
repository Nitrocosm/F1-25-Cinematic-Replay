// Records this process's audio (the game's own sound, nothing else on the PC) to a WAV file, for the sound pass of
// a render. Windows 10 2004+ / 11 per-process loopback: ActivateAudioInterfaceAsync on "VAD\Process_Loopback" with
// the process id. The API's parameter struct isn't in the MinGW headers, so it is declared here, and the function
// is loaded from Mmdevapi.dll at run time.
//
// Timing: each packet carries the QPC time (100 ns units) of its first sample. Gaps (the game silent, or packets
// missing) are filled with silence from those times, so sample n of the file is at first_qpc + n / rate. The plugin
// records the QPC time at which the replay reached the edit's start and cuts the file there.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace audiocap {

// audioclientactivationparams.h (not in MinGW)
struct ProcessLoopbackParams {
    DWORD target_process_id;
    int mode;  // 0 = include the target process tree
};
struct ActivationParams {
    int type;  // 1 = process loopback
    ProcessLoopbackParams loopback;
};
using ActivateAsync_t = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *,
                                          IActivateAudioInterfaceAsyncOperation **);

// The completion handler ActivateAudioInterfaceAsync calls (agile: called on any thread).
class Handler : public IActivateAudioInterfaceCompletionHandler {
  public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    IAudioClient *client = nullptr;
    ~Handler() { CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        static const GUID agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler) || id == agile) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // owned by the Recorder (not heap-freed here)
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override
    {
        IUnknown *unk = nullptr;
        HRESULT ahr = E_FAIL;
        if (SUCCEEDED(op->GetActivateResult(&ahr, &unk)) && SUCCEEDED(ahr) && unk) {
            hr = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client));
            unk->Release();
        } else {
            hr = FAILED(ahr) ? ahr : E_FAIL;
        }
        SetEvent(done);
        return S_OK;
    }

  private:
    std::atomic<ULONG> refs_{1};
};

class Recorder {
  public:
    static constexpr int kRate = 48000, kChannels = 2;  // 32-bit float

    ~Recorder() { stop(); }

    bool start(const std::wstring &wav, std::string &err)
    {
        stop();
        HMODULE mm = LoadLibraryW(L"Mmdevapi.dll");
        auto activate = mm ? reinterpret_cast<ActivateAsync_t>(GetProcAddress(mm, "ActivateAudioInterfaceAsync")) : nullptr;
        if (!activate) {
            err = "no per-process audio capture on this Windows";
            return false;
        }
        ActivationParams ap{1, {GetCurrentProcessId(), 0}};
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof ap;
        pv.blob.pBlobData = reinterpret_cast<BYTE *>(&ap);
        Handler h;
        IActivateAudioInterfaceAsyncOperation *op = nullptr;
        HRESULT hr = activate(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, &h, &op);
        if (SUCCEEDED(hr))
            WaitForSingleObject(h.done, 5000);
        if (op)
            op->Release();
        if (FAILED(hr) || FAILED(h.hr) || !h.client) {
            char b[96];
            snprintf(b, sizeof b, "audio capture unavailable (0x%08lx)", (unsigned long)(FAILED(hr) ? hr : h.hr));
            err = b;
            return false;
        }
        client_ = h.client;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = kChannels;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = kChannels * 4;
        wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 2000000, 0, &wf, nullptr);
        if (SUCCEEDED(hr))
            hr = client_->SetEventHandle(event_);
        if (SUCCEEDED(hr))
            hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture_));
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof b, "audio capture would not start (0x%08lx)", (unsigned long)hr);
            err = b;
            stop();
            return false;
        }
        file_ = _wfopen(wav.c_str(), L"wb");
        if (!file_) {
            err = "could not write the audio file";
            stop();
            return false;
        }
        write_header(0);  // sizes filled in at stop()
        frames_ = 0;
        first_qpc_ = 0;
        running_ = true;
        client_->Start();
        worker_ = std::thread([this] { run(); });
        return true;
    }

    // The QPC time (100 ns units) of the file's first sample, 0 until the first packet arrived.
    int64_t first_qpc() const { return first_qpc_.load(); }
    bool active() const { return running_.load(); }

    void stop()
    {
        if (running_.exchange(false) && worker_.joinable()) {
            SetEvent(event_);
            worker_.join();
        }
        if (client_)
            client_->Stop();
        if (file_) {
            write_header(frames_);
            fclose(file_);
            file_ = nullptr;
        }
        if (capture_)
            capture_->Release();
        capture_ = nullptr;
        if (client_)
            client_->Release();
        client_ = nullptr;
        if (event_)
            CloseHandle(event_);
        event_ = nullptr;
    }

    // Now, in the units of first_qpc() (100 ns on the QPC clock).
    static int64_t now_qpc100()
    {
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        return int64_t((long double)c.QuadPart * 10000000.0L / (long double)f.QuadPart);
    }

  private:
    IAudioClient *client_ = nullptr;
    IAudioCaptureClient *capture_ = nullptr;
    HANDLE event_ = nullptr;
    FILE *file_ = nullptr;
    uint64_t frames_ = 0;  // sample frames written
    std::atomic<int64_t> first_qpc_{0};

  public:
    std::atomic<int64_t> first_wall_{0};  // when the first packet was read (now_qpc100), for checking first_qpc_

  private:
    std::atomic<bool> running_{false};
    std::thread worker_;

    void write_header(uint64_t frames)
    {
        uint32_t data = uint32_t(frames * kChannels * 4);
        uint32_t riff = 4 + (8 + 18) + 8 + data;
        uint16_t fmt_tag = 3, ch = kChannels, block = kChannels * 4, bits = 32, cb = 0;
        uint32_t rate = kRate, bps = kRate * block, fmt_len = 18;
        fseek(file_, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, file_);
        fwrite(&riff, 4, 1, file_);
        fwrite("WAVEfmt ", 1, 8, file_);
        fwrite(&fmt_len, 4, 1, file_);
        fwrite(&fmt_tag, 2, 1, file_);
        fwrite(&ch, 2, 1, file_);
        fwrite(&rate, 4, 1, file_);
        fwrite(&bps, 4, 1, file_);
        fwrite(&block, 2, 1, file_);
        fwrite(&bits, 2, 1, file_);
        fwrite(&cb, 2, 1, file_);
        fwrite("data", 1, 4, file_);
        fwrite(&data, 4, 1, file_);
        fseek(file_, 0, SEEK_END);
    }

    void write_silence(uint64_t frames)
    {
        static const float zero[1024 * kChannels] = {};
        while (frames) {
            uint64_t n = std::min<uint64_t>(frames, 1024);
            fwrite(zero, sizeof(float) * kChannels, size_t(n), file_);
            frames_ += n;
            frames -= n;
        }
    }

    void run()
    {
        while (running_) {
            WaitForSingleObject(event_, 100);
            UINT32 next = 0;
            while (running_ && SUCCEEDED(capture_->GetNextPacketSize(&next)) && next) {
                BYTE *data = nullptr;
                UINT32 n = 0;
                DWORD flags = 0;
                UINT64 pos = 0, qpc = 0;
                if (FAILED(capture_->GetBuffer(&data, &n, &flags, &pos, &qpc)))
                    break;
                if (first_qpc_ == 0) {
                    first_qpc_ = int64_t(qpc);
                    first_wall_ = now_qpc100();  // diagnostic: when the first packet reached us
                }
                // keep the file on the clock: a packet later than the samples written so far means a gap
                int64_t expect = int64_t((long double)(int64_t(qpc) - first_qpc_) * kRate / 10000000.0L);
                if (expect > int64_t(frames_) + kRate / 50)  // more than 20 ms missing
                    write_silence(uint64_t(expect) - frames_);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data)
                    write_silence(n);
                else {
                    fwrite(data, sizeof(float) * kChannels, n, file_);
                    frames_ += n;
                }
                capture_->ReleaseBuffer(n);
            }
            // nothing coming (the game silent): silence up to now
            if (first_qpc_ != 0) {
                int64_t expect = int64_t((long double)(now_qpc100() - first_qpc_) * kRate / 10000000.0L);
                if (expect > int64_t(frames_) + kRate / 5)  // 200 ms behind the clock
                    write_silence(uint64_t(expect) - frames_ - kRate / 10);
            }
        }
    }
};

} // namespace audiocap
