#include <obs-module.h>
#include <objbase.h>
#include <winrt/base.h>

obs_source_info* corebs_get_game_capture_source_info();

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("corebs-obs-game-capture", "en-US")

MODULE_EXPORT const char* obs_module_description(void)
{
    return "Experimental coreBS gameplay capture source using Windows.Graphics.Capture.";
}

bool obs_module_load(void)
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& ex) {
        if (ex.code() != RPC_E_CHANGED_MODE) {
            blog(LOG_WARNING, "[coreBS] Failed to initialize WinRT apartment in OBS module load: 0x%08lX", static_cast<unsigned long>(ex.code().value));
        }
    }

    obs_register_source(corebs_get_game_capture_source_info());
    return true;
}
