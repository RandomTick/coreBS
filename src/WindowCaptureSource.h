#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace corebs {

class WindowCaptureSource {
public:
    struct StartOptions {
        HWND targetWindow = nullptr;
        bool captureCursor = false;
        bool cropToClient = true;
    };

    struct Snapshot {
        bool hasFrame = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::byte> pixels;
    };

    WindowCaptureSource();
    ~WindowCaptureSource();

    void Start(const StartOptions& options);
    void Stop();
    bool IsRunning() const;
    HWND GetTargetWindow() const;
    Snapshot GetLatestSnapshot() const;

private:
    void CreateD3DDevice();
    void CreateCaptureItem(HWND hwnd);
    void CreateFramePool(const winrt::Windows::Graphics::SizeInt32& size);
    void EnsureStagingTexture(uint32_t sourceWidth, uint32_t sourceHeight);
    RECT GetClientCaptureRect(uint32_t sourceWidth, uint32_t sourceHeight) const;
    void HandleFrameArrived(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender,
        const winrt::Windows::Foundation::IInspectable&);

    StartOptions m_options{};

    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_captureItem{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::event_token m_frameArrivedToken{};
    winrt::Windows::Graphics::SizeInt32 m_framePoolSize{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    uint32_t m_stagingWidth = 0;
    uint32_t m_stagingHeight = 0;

    mutable std::mutex m_mutex;
    Snapshot m_latestSnapshot{};
    std::atomic<bool> m_running{false};
};

}  // namespace corebs
