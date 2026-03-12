#include "VideoCapture.h"

#include "Utils.h"

#include <mfapi.h>
#include <mferror.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace corebs {

namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;

template <typename Interface, typename Object>
winrt::com_ptr<Interface> GetDxgiInterfaceFromObject(Object const& object)
{
    auto access = object.template as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<Interface> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<Interface>(), result.put_void()));
    return result;
}

uint32_t ComputeVideoBitrate(uint32_t width, uint32_t height, int fps)
{
    const uint64_t pixelsPerSecond = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * static_cast<uint64_t>(fps);
    const uint64_t estimate = pixelsPerSecond / 6;
    return static_cast<uint32_t>(std::clamp<uint64_t>(estimate, 8000000ULL, 40000000ULL));
}

void ResampleBgraToFixedOutput(
    const std::byte* source,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t sourcePitch,
    std::byte* destination,
    uint32_t destinationWidth,
    uint32_t destinationHeight,
    uint32_t destinationPitch)
{
    for (uint32_t y = 0; y < destinationHeight; ++y) {
        const auto sampleY = static_cast<uint32_t>((static_cast<uint64_t>(y) * sourceHeight) / destinationHeight);
        const auto* sourceRow = source + static_cast<size_t>(sampleY) * sourcePitch;
        auto* destinationRow = destination + static_cast<size_t>(y) * destinationPitch;
        for (uint32_t x = 0; x < destinationWidth; ++x) {
            const auto sampleX = static_cast<uint32_t>((static_cast<uint64_t>(x) * sourceWidth) / destinationWidth);
            const auto* sourcePixel = sourceRow + static_cast<size_t>(sampleX) * 4;
            auto* destinationPixel = destinationRow + static_cast<size_t>(x) * 4;
            destinationPixel[0] = sourcePixel[0];
            destinationPixel[1] = sourcePixel[1];
            destinationPixel[2] = sourcePixel[2];
            destinationPixel[3] = sourcePixel[3];
        }
    }
}

}  // namespace

VideoCapture::VideoCapture() = default;

VideoCapture::~VideoCapture()
{
    Stop();
}

void VideoCapture::Start(const StartOptions& options)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_running.load()) {
        utils::Fail(L"Video capture has already started.");
    }

    m_options = options;
    m_frameIntervalQpc = utils::SecondsToQpcTicks(1.0 / static_cast<double>(options.fps));
    m_lastWrittenQpc = 0;
    m_firstSampleTimeHns = -1;
    m_stats = {};

    CreateD3DDevice();
    CreateCaptureItem(options.targetWindow);

    const auto size = m_captureItem.Size();
    if (size.Width <= 0 || size.Height <= 0) {
        utils::Fail(L"Target window has an invalid capture size.");
    }

    m_stats.width = static_cast<uint32_t>(size.Width);
    m_stats.height = static_cast<uint32_t>(size.Height);
    m_outputBuffer.resize(static_cast<size_t>(m_stats.width) * static_cast<size_t>(m_stats.height) * 4ULL);

    CreateSinkWriter();
    CreateFramePool();

    m_running.store(true);
    m_frameArrivedToken = m_framePool.FrameArrived({this, &VideoCapture::HandleFrameArrived});
    m_session.IsCursorCaptureEnabled(options.captureCursor);
    m_session.StartCapture();
}

void VideoCapture::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_session && !m_framePool && !m_sinkWriter) {
        return;
    }

    m_running.store(false);

    if (m_framePool) {
        m_framePool.FrameArrived(m_frameArrivedToken);
    }
    if (m_session) {
        m_session.Close();
    }
    if (m_framePool) {
        m_framePool.Close();
    }

    if (m_sinkWriter) {
        m_sinkWriter->Finalize();
        m_sinkWriter.Reset();
    }

    m_session = nullptr;
    m_framePool = nullptr;
    m_captureItem = nullptr;
    m_stagingTexture.Reset();
    m_deviceManager.Reset();
}

VideoCapture::Stats VideoCapture::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void VideoCapture::CreateD3DDevice()
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

    utils::ThrowIfFailed(hr, L"D3D11CreateDevice failed");

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    m_d3dDevice.As(&dxgiDevice);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    m_winrtDevice = inspectable.as<wgdx11::IDirect3DDevice>();

    utils::ThrowIfFailed(MFCreateDXGIDeviceManager(&m_deviceResetToken, &m_deviceManager), L"MFCreateDXGIDeviceManager failed");
    utils::ThrowIfFailed(m_deviceManager->ResetDevice(m_d3dDevice.Get(), m_deviceResetToken), L"IMFDXGIDeviceManager::ResetDevice failed");
}

void VideoCapture::CreateCaptureItem(HWND hwnd)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    utils::ThrowIfFailed(
        interop->CreateForWindow(hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(m_captureItem)),
        L"CreateForWindow failed");
}

void VideoCapture::CreateFramePool()
{
    const auto size = m_captureItem.Size();
    m_framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_winrtDevice,
        wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        size);
    m_session = m_framePool.CreateCaptureSession(m_captureItem);
}

void VideoCapture::CreateSinkWriter()
{
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    utils::ThrowIfFailed(MFCreateAttributes(&attributes, 3), L"MFCreateAttributes failed");
    utils::ThrowIfFailed(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), L"Set hardware transforms failed");
    utils::ThrowIfFailed(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), L"Set low latency failed");
    utils::ThrowIfFailed(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, m_deviceManager.Get()), L"Set D3D manager failed");

    utils::ThrowIfFailed(
        MFCreateSinkWriterFromURL(m_options.outputPath.c_str(), nullptr, attributes.Get(), &m_sinkWriter),
        L"MFCreateSinkWriterFromURL failed");

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    utils::ThrowIfFailed(MFCreateMediaType(&outputType), L"MFCreateMediaType for H.264 output failed");
    utils::ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"Set output major type failed");
    utils::ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), L"Set output subtype failed");
    utils::ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, ComputeVideoBitrate(m_stats.width, m_stats.height, m_options.fps)), L"Set output bitrate failed");
    utils::ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), L"Set output interlace failed");
    utils::ThrowIfFailed(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, m_stats.width, m_stats.height), L"Set output frame size failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, m_options.fps, 1), L"Set output frame rate failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), L"Set output pixel aspect ratio failed");
    utils::ThrowIfFailed(m_sinkWriter->AddStream(outputType.Get(), &m_streamIndex), L"AddStream failed");

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    utils::ThrowIfFailed(MFCreateMediaType(&inputType), L"MFCreateMediaType for RGB input failed");
    utils::ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"Set input major type failed");
    utils::ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), L"Set input subtype failed");
    utils::ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), L"Set input interlace failed");
    utils::ThrowIfFailed(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_stats.width, m_stats.height), L"Set input frame size failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, m_options.fps, 1), L"Set input frame rate failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), L"Set input pixel aspect ratio failed");
    utils::ThrowIfFailed(m_sinkWriter->SetInputMediaType(m_streamIndex, inputType.Get(), nullptr), L"SetInputMediaType failed");
    utils::ThrowIfFailed(m_sinkWriter->BeginWriting(), L"BeginWriting failed");
}

void VideoCapture::EnsureStagingTexture(uint32_t sourceWidth, uint32_t sourceHeight)
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

    utils::ThrowIfFailed(m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture), L"CreateTexture2D for staging texture failed");
    m_stagingWidth = sourceWidth;
    m_stagingHeight = sourceHeight;
}

void VideoCapture::HandleFrameArrived(
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

        const auto nowQpc = utils::QueryPerformanceCounterValue();
        if (m_lastWrittenQpc != 0 && (nowQpc - m_lastWrittenQpc) < m_frameIntervalQpc) {
            ++m_stats.droppedFrames;
            return;
        }

        WriteFrame(frame, nowQpc);
        m_lastWrittenQpc = nowQpc;
    } catch (const std::exception& ex) {
        utils::LogError(L"Video frame handling failed: " + utils::Utf8ToWide(ex.what()));
        m_running.store(false);
    }
}

void VideoCapture::WriteFrame(
    const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame,
    int64_t arrivalQpc)
{
    auto texture = GetDxgiInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);

    EnsureStagingTexture(sourceDesc.Width, sourceDesc.Height);
    m_d3dContext->CopyResource(m_stagingTexture.Get(), texture.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    utils::ThrowIfFailed(m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped), L"Map on capture texture failed");

    const auto* sourceBytes = reinterpret_cast<const std::byte*>(mapped.pData);
    auto* destinationBytes = m_outputBuffer.data();
    ResampleBgraToFixedOutput(
        sourceBytes,
        sourceDesc.Width,
        sourceDesc.Height,
        mapped.RowPitch,
        destinationBytes,
        m_stats.width,
        m_stats.height,
        m_stats.width * 4U);

    m_d3dContext->Unmap(m_stagingTexture.Get(), 0);

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    const DWORD byteCount = static_cast<DWORD>(m_outputBuffer.size());
    utils::ThrowIfFailed(MFCreateMemoryBuffer(byteCount, &buffer), L"MFCreateMemoryBuffer failed");

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    utils::ThrowIfFailed(buffer->Lock(&destination, &maxLength, nullptr), L"IMFMediaBuffer::Lock failed");
    memcpy(destination, m_outputBuffer.data(), byteCount);
    utils::ThrowIfFailed(buffer->Unlock(), L"IMFMediaBuffer::Unlock failed");
    utils::ThrowIfFailed(buffer->SetCurrentLength(byteCount), L"IMFMediaBuffer::SetCurrentLength failed");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    utils::ThrowIfFailed(MFCreateSample(&sample), L"MFCreateSample failed");
    utils::ThrowIfFailed(sample->AddBuffer(buffer.Get()), L"IMFSample::AddBuffer failed");

    if (m_firstSampleTimeHns < 0) {
        m_firstSampleTimeHns = utils::QpcTicksToHundredsOfNanoseconds(arrivalQpc - m_options.baseQpc);
    }

    const LONGLONG sampleDuration = 10000000LL / static_cast<LONGLONG>(m_options.fps);
    const LONGLONG sampleTime = static_cast<LONGLONG>(m_firstSampleTimeHns) + static_cast<LONGLONG>(m_stats.writtenFrames) * sampleDuration;

    utils::ThrowIfFailed(sample->SetSampleDuration(sampleDuration), L"IMFSample::SetSampleDuration failed");
    utils::ThrowIfFailed(sample->SetSampleTime(sampleTime), L"IMFSample::SetSampleTime failed");
    utils::ThrowIfFailed(m_sinkWriter->WriteSample(m_streamIndex, sample.Get()), L"IMFSinkWriter::WriteSample failed");

    ++m_stats.writtenFrames;
}

}  // namespace corebs