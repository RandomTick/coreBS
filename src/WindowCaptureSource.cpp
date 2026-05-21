#include "WindowCaptureSource.h"

#include "Utils.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <winrt/base.h>

namespace corebs {

namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;

void EnsureThreadApartment()
{
    static thread_local bool initialized = false;
    if (initialized) {
        return;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& ex) {
        if (ex.code() != RPC_E_CHANGED_MODE) {
            throw;
        }
    }

    initialized = true;
}

template <typename Interface, typename Object>
winrt::com_ptr<Interface> GetDxgiInterfaceFromObject(Object const& object)
{
    auto access = object.template as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<Interface> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<Interface>(), result.put_void()));
    return result;
}

void CopyClientRegion(
    const std::byte* source,
    uint32_t sourcePitch,
    const RECT& cropRect,
    std::vector<std::byte>& destination)
{
    const uint32_t width = static_cast<uint32_t>(std::max<LONG>(1, cropRect.right - cropRect.left));
    const uint32_t height = static_cast<uint32_t>(std::max<LONG>(1, cropRect.bottom - cropRect.top));
    destination.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4ULL);

    for (uint32_t y = 0; y < height; ++y) {
        const auto* sourceRow = source + static_cast<size_t>(cropRect.top + static_cast<LONG>(y)) * sourcePitch + static_cast<size_t>(cropRect.left) * 4U;
        auto* destinationRow = destination.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4ULL;
        memcpy(destinationRow, sourceRow, static_cast<size_t>(width) * 4ULL);

        for (uint32_t x = 0; x < width; ++x) {
            destinationRow[static_cast<size_t>(x) * 4ULL + 3ULL] = std::byte{0xFF};
        }
    }
}

}  // namespace

WindowCaptureSource::WindowCaptureSource() = default;

WindowCaptureSource::~WindowCaptureSource()
{
    Stop();
}

void WindowCaptureSource::Start(const StartOptions& options)
{
    if (m_running.load()) {
        Stop();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    EnsureThreadApartment();
    m_options = options;
    m_latestSnapshot = {};
    m_framePoolSize = {};

    CreateD3DDevice();
    CreateCaptureItem(options.targetWindow);
    CreateFramePool(m_captureItem.Size());

    m_running.store(true);
    m_frameArrivedToken = m_framePool.FrameArrived({this, &WindowCaptureSource::HandleFrameArrived});
    m_session.IsCursorCaptureEnabled(options.captureCursor);
    if (const auto session3 = m_session.try_as<wgc::IGraphicsCaptureSession3>()) {
        session3.IsBorderRequired(false);
    }
    m_session.StartCapture();
}

void WindowCaptureSource::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_session && !m_framePool) {
        m_running.store(false);
        return;
    }

    m_running.store(false);

    if (m_framePool) {
        m_framePool.FrameArrived(m_frameArrivedToken);
        m_framePool.Close();
    }
    if (m_session) {
        m_session.Close();
    }

    m_session = nullptr;
    m_framePool = nullptr;
    m_captureItem = nullptr;
    m_framePoolSize = {};
    m_winrtDevice = nullptr;
    m_stagingTexture.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_stagingWidth = 0;
    m_stagingHeight = 0;
    m_latestSnapshot = {};
}

bool WindowCaptureSource::IsRunning() const
{
    return m_running.load();
}

HWND WindowCaptureSource::GetTargetWindow() const
{
    return m_options.targetWindow;
}

WindowCaptureSource::Snapshot WindowCaptureSource::GetLatestSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latestSnapshot;
}

void WindowCaptureSource::CreateD3DDevice()
{
    constexpr std::array<D3D_FEATURE_LEVEL, 4> featureLevels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    const HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        m_d3dDevice.GetAddressOf(),
        &selectedFeatureLevel,
        m_d3dContext.GetAddressOf());

    utils::ThrowIfFailed(hr, L"D3D11CreateDevice failed for window capture source");

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    m_d3dDevice.As(&dxgiDevice);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    m_winrtDevice = inspectable.as<wgdx11::IDirect3DDevice>();
}

void WindowCaptureSource::CreateCaptureItem(HWND hwnd)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    utils::ThrowIfFailed(
        interop->CreateForWindow(hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(m_captureItem)),
        L"CreateForWindow failed for window capture source");
}

void WindowCaptureSource::CreateFramePool(const winrt::Windows::Graphics::SizeInt32& size)
{
    m_framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_winrtDevice,
        wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size);
    m_session = m_framePool.CreateCaptureSession(m_captureItem);
    m_framePoolSize = size;
}

void WindowCaptureSource::EnsureStagingTexture(uint32_t sourceWidth, uint32_t sourceHeight)
{
    if (m_stagingTexture && sourceWidth == m_stagingWidth && sourceHeight == m_stagingHeight) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = sourceWidth;
    desc.Height = sourceHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    utils::ThrowIfFailed(m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture), L"CreateTexture2D failed for window capture source");
    m_stagingWidth = sourceWidth;
    m_stagingHeight = sourceHeight;
}

RECT WindowCaptureSource::GetClientCaptureRect(uint32_t sourceWidth, uint32_t sourceHeight) const
{
    RECT fullRect{};
    fullRect.left = 0;
    fullRect.top = 0;
    fullRect.right = static_cast<LONG>(sourceWidth);
    fullRect.bottom = static_cast<LONG>(sourceHeight);

    if (m_options.targetWindow == nullptr || !m_options.cropToClient) {
        return fullRect;
    }

    RECT windowRect{};
    RECT clientRect{};
    if (!GetWindowRect(m_options.targetWindow, &windowRect) || !GetClientRect(m_options.targetWindow, &clientRect)) {
        return fullRect;
    }

    POINT topLeft{clientRect.left, clientRect.top};
    POINT bottomRight{clientRect.right, clientRect.bottom};
    if (!ClientToScreen(m_options.targetWindow, &topLeft) || !ClientToScreen(m_options.targetWindow, &bottomRight)) {
        return fullRect;
    }

    RECT cropRect{};
    cropRect.left = std::clamp<LONG>(topLeft.x - windowRect.left, 0, static_cast<LONG>(sourceWidth));
    cropRect.top = std::clamp<LONG>(topLeft.y - windowRect.top, 0, static_cast<LONG>(sourceHeight));
    cropRect.right = std::clamp<LONG>(bottomRight.x - windowRect.left, cropRect.left + 1, static_cast<LONG>(sourceWidth));
    cropRect.bottom = std::clamp<LONG>(bottomRight.y - windowRect.top, cropRect.top + 1, static_cast<LONG>(sourceHeight));

    if (cropRect.right <= cropRect.left || cropRect.bottom <= cropRect.top) {
        return fullRect;
    }

    return cropRect;
}

void WindowCaptureSource::HandleFrameArrived(
    const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender,
    const winrt::Windows::Foundation::IInspectable&)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running.load()) {
        return;
    }

    try {
        const auto frame = sender.TryGetNextFrame();
        if (!frame) {
            return;
        }

        const auto contentSize = frame.ContentSize();
        if (contentSize.Width != m_framePoolSize.Width || contentSize.Height != m_framePoolSize.Height) {
            m_framePool.Recreate(m_winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
            m_framePoolSize = contentSize;
        }

        auto texture = GetDxgiInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

        D3D11_TEXTURE2D_DESC sourceDesc{};
        texture->GetDesc(&sourceDesc);

        EnsureStagingTexture(sourceDesc.Width, sourceDesc.Height);
        m_d3dContext->CopyResource(m_stagingTexture.Get(), texture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        utils::ThrowIfFailed(m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped), L"Map failed for window capture source");

        const auto cropRect = GetClientCaptureRect(sourceDesc.Width, sourceDesc.Height);
        const uint32_t width = static_cast<uint32_t>(std::max<LONG>(1, cropRect.right - cropRect.left));
        const uint32_t height = static_cast<uint32_t>(std::max<LONG>(1, cropRect.bottom - cropRect.top));
        auto* sourceBytes = reinterpret_cast<const std::byte*>(mapped.pData);

        m_latestSnapshot.width = width;
        m_latestSnapshot.height = height;
        m_latestSnapshot.hasFrame = true;
        CopyClientRegion(sourceBytes, mapped.RowPitch, cropRect, m_latestSnapshot.pixels);

        m_d3dContext->Unmap(m_stagingTexture.Get(), 0);
    } catch (const std::exception& ex) {
        utils::LogError(L"Window capture source failed: " + utils::Utf8ToWide(ex.what()));
        m_running.store(false);
    }
}

}  // namespace corebs
