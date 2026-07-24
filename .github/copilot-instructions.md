# Copilot Instructions

## Project Guidelines
- When adding diagnostics to UE SDK wrapper functions in this project, use SPDLOG_INFO/WARN/ERROR macros (spdlog) consistent with existing files like AHUD.cpp, and log relevant pointers (e.g., uclass, this, result) to help pinpoint failure branches.
- In WuWa/UEVR Lua scripting, dynamically locate the active instance of LGUI's LGUIEventSystem by finding the one with a non-nil selection, rather than assuming a singleton. Use its InputTrigger/InputNavigationUp/InputNavigationDown methods for native gamepad confirm/back/navigation instead of emulating mouse/cursor input.