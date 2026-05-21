#include "VideoCapture.h"

#include "Utils.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>

namespace corebs {

namespace {

uint32_t ComputeVideoBitrate(uint32_t width, uint32_t height, int fps)
{
    const uint64_t pixelsPerSecond = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * static_cast<uint64_t>(fps);
    const uint64_t estimate = pixelsPerSecond / 6;
    return static_cast<uint32_t>(std::clamp<uint64_t>(estimate, 8000000ULL, 40000000ULL));
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

    if (m_running) {
        utils::Fail(L"Video encoder has already started.");
    }

    m_options = options;
    m_stats = {};
    m_stats.width = options.width;
    m_stats.height = options.height;

    CreateSinkWriter();
    m_running = true;
}

void VideoCapture::WriteFrame(const std::vector<std::byte>& frameData, int64_t frameQpc)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running) {
        return;
    }

    const DWORD expectedBytes = m_stats.width * m_stats.height * 4U;
    if (frameData.size() != expectedBytes) {
        utils::Fail(L"Video encoder received a frame buffer with an unexpected size.");
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    utils::ThrowIfFailed(MFCreateMemoryBuffer(expectedBytes, &buffer), L"MFCreateMemoryBuffer failed for session video");

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    utils::ThrowIfFailed(buffer->Lock(&destination, &maxLength, nullptr), L"IMFMediaBuffer::Lock failed for session video");
    memcpy(destination, frameData.data(), expectedBytes);
    utils::ThrowIfFailed(buffer->Unlock(), L"IMFMediaBuffer::Unlock failed for session video");
    utils::ThrowIfFailed(buffer->SetCurrentLength(expectedBytes), L"IMFMediaBuffer::SetCurrentLength failed for session video");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    utils::ThrowIfFailed(MFCreateSample(&sample), L"MFCreateSample failed for session video");
    utils::ThrowIfFailed(sample->AddBuffer(buffer.Get()), L"IMFSample::AddBuffer failed for session video");

    const LONGLONG sampleDuration = 10000000LL / static_cast<LONGLONG>(m_options.fps);
    const int64_t relativeFrameQpc = std::max<int64_t>(0, frameQpc - m_options.baseQpc);
    const LONGLONG sampleTime = static_cast<LONGLONG>(utils::QpcTicksToHundredsOfNanoseconds(relativeFrameQpc));

    utils::ThrowIfFailed(sample->SetSampleDuration(sampleDuration), L"IMFSample::SetSampleDuration failed for session video");
    utils::ThrowIfFailed(sample->SetSampleTime(sampleTime), L"IMFSample::SetSampleTime failed for session video");
    utils::ThrowIfFailed(m_sinkWriter->WriteSample(m_streamIndex, sample.Get()), L"IMFSinkWriter::WriteSample failed for session video");

    ++m_stats.writtenFrames;
}

void VideoCapture::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running) {
        return;
    }

    if (m_sinkWriter) {
        m_sinkWriter->Finalize();
        m_sinkWriter.Reset();
    }

    m_running = false;
}

VideoCapture::Stats VideoCapture::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void VideoCapture::CreateSinkWriter()
{
    utils::ThrowIfFailed(
        MFCreateSinkWriterFromURL(m_options.outputPath.c_str(), nullptr, nullptr, &m_sinkWriter),
        L"MFCreateSinkWriterFromURL failed for session video");

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    utils::ThrowIfFailed(MFCreateMediaType(&outputType), L"MFCreateMediaType failed for session video output");
    utils::ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"Set session video output major type failed");
    utils::ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), L"Set session video output subtype failed");
    utils::ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, ComputeVideoBitrate(m_options.width, m_options.height, m_options.fps)), L"Set session video bitrate failed");
    utils::ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), L"Set session video interlace failed");
    utils::ThrowIfFailed(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, m_options.width, m_options.height), L"Set session video frame size failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, m_options.fps, 1), L"Set session video frame rate failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), L"Set session video pixel aspect failed");
    utils::ThrowIfFailed(m_sinkWriter->AddStream(outputType.Get(), &m_streamIndex), L"AddStream failed for session video");

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    utils::ThrowIfFailed(MFCreateMediaType(&inputType), L"MFCreateMediaType failed for session video input");
    utils::ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"Set session video input major type failed");
    utils::ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), L"Set session video input subtype failed");
    utils::ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), L"Set session video input interlace failed");
    utils::ThrowIfFailed(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_options.width, m_options.height), L"Set session video input frame size failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, m_options.fps, 1), L"Set session video input frame rate failed");
    utils::ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), L"Set session video input pixel aspect failed");
    utils::ThrowIfFailed(m_sinkWriter->SetInputMediaType(m_streamIndex, inputType.Get(), nullptr), L"SetInputMediaType failed for session video");
    utils::ThrowIfFailed(m_sinkWriter->BeginWriting(), L"BeginWriting failed for session video");
}

}  // namespace corebs
