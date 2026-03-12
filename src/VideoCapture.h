#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <vector>

namespace corebs {

class VideoCapture {
public:
    struct StartOptions {
        HWND targetWindow = nullptr;
        std::filesystem::path outputPath;
        int fps = 60;
        bool captureCursor = true;
        bool verbose = false;
        int64_t baseQpc = 0;
    };

    struct Stats {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t writtenFrames = 0;
        uint64_t droppedFrames = 0;
    };

    VideoCapture();
    ~VideoCapture();

    void Start(const StartOptions& options);
    void Stop();
    Stats GetStats() const;

private:
    void CreateD3DDevice();
    void CreateCaptureItem(HWND hwnd);
    void CreateFramePool();
    void CreateSinkWriter();
    void EnsureStagingTexture(uint32_t sourceWidth, uint32_t sourceHeight);
    void HandleFrameArrived(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender,
        const winrt::Windows::Foundation::IInspectable&);
    void WriteFrame(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame,
        int64_t arrivalQpc);

    StartOptions m_options{};
    Stats m_stats{};

    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_captureItem{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::event_token m_frameArrivedToken{};

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    Microsoft::WRL::ComPtr<IMFSinkWriter> m_sinkWriter;
    UINT m_deviceResetToken = 0;
    DWORD m_streamIndex = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    uint32_t m_stagingWidth = 0;
    uint32_t m_stagingHeight = 0;
    std::vector<std::byte> m_outputBuffer;

    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    int64_t m_frameIntervalQpc = 0;
    int64_t m_lastWrittenQpc = 0;
    int64_t m_firstSampleTimeHns = -1;
};

}  // namespace corebs