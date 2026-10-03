// dllmain
#include <windows.h>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <utility/Thread.hpp>

#include "Framework.hpp"

void startup_thread(HMODULE poc_module) {
    g_framework = std::make_unique<Framework>(poc_module);
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, handle, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        // reserved == nullptr means we were unloaded via FreeLibrary (explicit unload).
        // reserved != nullptr means the process is terminating (DLL is never actually unloaded).
        if (spdlog::default_logger_raw() != nullptr) {
            spdlog::error(
                "DllMain: DLL_PROCESS_DETACH (reserved={}, meaning: {}), g_framework={}",
                (void*)reserved,
                reserved == nullptr ? "explicit FreeLibrary unload" : "process terminating",
                (void*)g_framework.get()
            );
            spdlog::default_logger()->flush();
        }
        break;
    default:
        break;
    }

    return TRUE;
}