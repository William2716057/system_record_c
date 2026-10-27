#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static volatile LONG g_stop = 0;

static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static void write_zeros(FILE *f, uint64_t frames, uint32_t block_align)
{
    static const char zeros[4096];
    uint64_t bytes = frames * block_align;
    while (bytes) {
        size_t n = bytes > sizeof zeros ? sizeof zeros : (size_t)bytes;
        fwrite(zeros, 1, n, f);
        bytes -= n;
    }
}

static void pad_gap(FILE *f, uint64_t *frames, uint64_t expected,
                    uint32_t rate, uint32_t block_align)
{
    if (expected > *frames + rate / 10) {
        uint64_t gap = expected - *frames;
        write_zeros(f, gap, block_align);
        *frames += gap;
    }
}

#define CHECK(call, msg)                                                  \
    do {                                                                  \
        hr = (call);                                                      \
        if (FAILED(hr)) {                                                 \
            fprintf(stderr, "%s failed (0x%08lX)\n", msg, (unsigned long)hr); \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)
 
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "output.wav";
    HRESULT hr;
    int exit_code = 1;
    int com_ok = 0;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioCaptureClient *capture = NULL;
    WAVEFORMATEX *wf = NULL;
    FILE *f = NULL;
    int started = 0;
 
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { fprintf(stderr, "CoInitializeEx failed\n"); return 1; }
    com_ok = 1;
 
    CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                           &IID_IMMDeviceEnumerator, (void **)&enumerator),
          "CoCreateInstance(MMDeviceEnumerator)");
    CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device),
          "GetDefaultAudioEndpoint");
    CHECK(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client),
          "Activate(IAudioClient)");
    CHECK(IAudioClient_GetMixFormat(client, &wf), "GetMixFormat");
 

    CHECK(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  10000000, 0, wf, NULL),
          "IAudioClient_Initialize");
    CHECK(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&capture),
          "GetService(IAudioCaptureClient)");
 
    uint32_t rate = wf->nSamplesPerSec;
    uint32_t block_align = wf->nBlockAlign;
    uint32_t fmt_size = (uint32_t)sizeof(WAVEFORMATEX) + wf->cbSize;
 
    printf("Format: %lu Hz, %u ch, %u bit (tag 0x%04X)\n",
           (unsigned long)rate, wf->nChannels, wf->wBitsPerSample, wf->wFormatTag);
 
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); goto cleanup; }
 

    fwrite("RIFF", 1, 4, f); write_u32(f, 0);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); write_u32(f, fmt_size);
    fwrite(wf, 1, fmt_size, f);
    fwrite("data", 1, 4, f); write_u32(f, 0);
 
    SetConsoleCtrlHandler(on_ctrl, TRUE);
 
    LARGE_INTEGER qpf, t0, now;
    QueryPerformanceFrequency(&qpf);
    uint64_t frames = 0;
 
    CHECK(IAudioClient_Start(client), "IAudioClient_Start");
    started = 1;
    QueryPerformanceCounter(&t0);
    printf("Recording to %s... Press Ctrl+C to stop.\n", path);

    //for timer
    uint64_t shown = (uint64_t)-1;
    while (!g_stop) {
        Sleep(10);

	QueryPerformanceCounter(&now);
	uint64_t secs = (uint64_t)(now.QuadPart - t0.QuadPart) / qpf.QuadPart;

	if (secs != shown) {
    		shown = secs;
    		printf("\rRecording: %02u:%02u:%02u",
           		(unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
    	fflush(stdout);
}

        UINT32 packet = 0;
        while (!g_stop || packet) {
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture, &packet)) || packet == 0)
                break;
 
            BYTE *data = NULL;
            UINT32 n = 0;
            DWORD flags = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &n, &flags, NULL, NULL)))
                break;
 
            QueryPerformanceCounter(&now);
            uint64_t expected = (uint64_t)(now.QuadPart - t0.QuadPart) * rate / qpf.QuadPart;
            if (expected > n)
                pad_gap(f, &frames, expected - n, rate, block_align);
 
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                write_zeros(f, n, block_align);
            else
                fwrite(data, block_align, n, f);
            frames += n;
 
            IAudioCaptureClient_ReleaseBuffer(capture, n);
        }
    }

    QueryPerformanceCounter(&now);
    pad_gap(f, &frames,
            (uint64_t)(now.QuadPart - t0.QuadPart) * rate / qpf.QuadPart,
            rate, block_align);
 

    uint32_t data_bytes = (uint32_t)(frames * block_align);
    fseek(f, 4, SEEK_SET);
    write_u32(f, 20 + fmt_size + data_bytes);
    fseek(f, 24 + fmt_size, SEEK_SET);
    write_u32(f, data_bytes);
 
    printf("\nStopped. %.2f seconds written.\n", (double)frames / rate);
    exit_code = 0;
 
cleanup:
    if (started) IAudioClient_Stop(client);
    if (f) fclose(f);
    if (wf) CoTaskMemFree(wf);
    if (capture) IAudioCaptureClient_Release(capture);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (com_ok) CoUninitialize();
    return exit_code;
}
