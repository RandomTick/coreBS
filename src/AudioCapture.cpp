#include "AudioCapture.h"

#include "Utils.h"

namespace corebs {

void AudioCapture::Start(const StartOptions&)
{
    utils::Fail(L"Application-only audio capture is not available in the video-only MVP commit.");
}

void AudioCapture::Stop()
{
}

}  // namespace corebs