#include "SignalHandler.h"

#include <windows.h>

#include <mutex>

namespace corebs {

namespace {

std::mutex g_callbackMutex;
SignalHandler::Callback g_callback;

}  // namespace

void SignalHandler::Install(Callback callback)
{
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_callback = std::move(callback);
    SetConsoleCtrlHandler(&SignalHandler::HandleCtrlEvent, TRUE);
}

void SignalHandler::Uninstall()
{
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    SetConsoleCtrlHandler(&SignalHandler::HandleCtrlEvent, FALSE);
    g_callback = nullptr;
}

BOOL WINAPI SignalHandler::HandleCtrlEvent(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        if (g_callback) {
            g_callback();
        }
        return TRUE;
    }
    return FALSE;
}

}  // namespace corebs