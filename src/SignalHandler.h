#pragma once

#include <windows.h>

#include <functional>

namespace corebs {

class SignalHandler {
public:
    using Callback = std::function<void()>;

    static void Install(Callback callback);
    static void Uninstall();

private:
    static BOOL WINAPI HandleCtrlEvent(DWORD ctrlType);
};

}  // namespace corebs