#include <chrono>
#include <spdlog/spdlog.h>
#include <utility/String.hpp>
#include <utility/Scan.hpp>

#include <SafetyHook.hpp>

#include "Framework.hpp"
#include "Mods.hpp"
#include "XInputHook.hpp"
#include "../mods/VR.hpp"

namespace detail {
// DIAGNOSTIC: dump the raw wButtons bitmask exactly as returned by the real XInputGetState,
// before any mod (Lua, LGUI routing, etc.) has a chance to touch it. Correlated with VR's
// synced/AFR mode so we can determine whether A/B/X/Y bits ever arrive at the OS-hook level
// during Synchronized Sequential mode, or whether they are already missing at this point.
void log_raw_xinput_diag(const char* tag, uint32_t user_index, uint32_t ret, const XINPUT_STATE* state) {
    if (ret != ERROR_SUCCESS || state == nullptr) {
        return;
    }

    static WORD last_buttons[4]{};

    const auto buttons = state->Gamepad.wButtons;

    if (user_index < 4 && buttons == last_buttons[user_index]) {
        return;
    }

    if (user_index < 4) {
        last_buttons[user_index] = buttons;
    }

    auto& vr = VR::get();

    if (!vr->is_diag_verbose_logging_enabled()) {
        return;
    }

    SPDLOG_INFO("[XInput][diag] {} user_index={} wButtons=0x{:04x} A={} B={} X={} Y={} DUP={} DDOWN={} DLEFT={} DRIGHT={} is_using_afr={} is_using_synchronized_afr={} is_using_2d_screen={} is_hmd_active={}",
        tag, user_index, buttons,
        (buttons & XINPUT_GAMEPAD_A) != 0,
        (buttons & XINPUT_GAMEPAD_B) != 0,
        (buttons & XINPUT_GAMEPAD_X) != 0,
        (buttons & XINPUT_GAMEPAD_Y) != 0,
        (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0,
        (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0,
        (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0,
        (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0,
        vr->is_using_afr(), vr->is_using_synchronized_afr(), vr->is_using_2d_screen(), vr->is_hmd_active());
}
}

XInputHook* g_hook{nullptr};

XInputHook::XInputHook() {
    g_hook = this;
    spdlog::info("[XInputHook] Entry");

    static auto find_dll = [](const std::string& name) -> HMODULE {
        const auto start_time = std::chrono::system_clock::now();
        auto found_dll = GetModuleHandleA(name.c_str());

        while (found_dll == nullptr) {
            if (found_dll = GetModuleHandleA(name.c_str()); found_dll != nullptr) {
                // Load it from the system directory instead, it might be hooked
                /*wchar_t system_dir[MAX_PATH]{};
                if (GetSystemDirectoryW(system_dir, MAX_PATH) != 0) {
                    const auto new_dir = (std::wstring{system_dir} + L"\\" + utility::widen(name));

                    spdlog::info("[XInputHook] Loading {} from {}", name, utility::narrow(new_dir));
                    const auto new_dll = LoadLibraryW(new_dir.c_str());

                    if (new_dll != nullptr) {
                        found_dll = LoadLibraryW(new_dir.c_str());
                    }
                }*/

                break;
            }

            const auto elapsed_time = std::chrono::system_clock::now() - start_time;

            if (elapsed_time > std::chrono::seconds(10)) {
                spdlog::error("[XInputHook] Failed to find {} after 10 seconds", name);
                return nullptr;
            }

            std::this_thread::yield();
        }

        return found_dll;
    };

    auto recursive_resolve_jmp = [](this const auto& self, uint8_t* instr) -> uintptr_t {
        try {
            const auto decoded = utility::decode_one(instr);

            if (decoded) {
                const auto mnem = std::string_view{decoded->Mnemonic};

                if (mnem.starts_with("JMP")) {
                    const auto target = utility::resolve_displacement((uintptr_t)instr);

                    if (target.has_value()) {
                        if (instr[0] == 0xFF && instr[1] == 0x25) {
                            const auto real_target = *(uintptr_t*)*target;

                            if (real_target == 0) {
                                return (uintptr_t)instr;
                            }

                            return self((uint8_t*)real_target);
                        }

                        return self((uint8_t*)target.value());
                    }
                }
            }
        } catch(...) {
            SPDLOG_ERROR("[XInputHook] recursive_resolve_jmp exception");
        }

        return (uintptr_t)instr;
    };

    auto perform_hooks_1_4 = [&]() {
        const auto xinput_1_4_dll = find_dll("xinput1_4.dll");

        if (xinput_1_4_dll != nullptr) {
            std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

            const auto get_state_fn = (void*)GetProcAddress(xinput_1_4_dll, "XInputGetState");
            const auto set_state_fn = (void*)GetProcAddress(xinput_1_4_dll, "XInputSetState");

            if (get_state_fn != nullptr) {
                m_xinput_1_4_get_state_hook = safetyhook::create_inline(get_state_fn, get_state_hook_1_4);

                if (!m_xinput_1_4_get_state_hook) {
                    spdlog::error("Failed to hook XInputGetState (1_4), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)get_state_fn);

                    if (jmp_addr != (uintptr_t)get_state_fn) {
                        m_xinput_1_4_get_state_hook = safetyhook::create_inline((void*)jmp_addr, get_state_hook_1_4);

                        if (!m_xinput_1_4_get_state_hook) {
                            spdlog::error("Failed to hook XInputGetState (1_4) (jmp)");
                        }
                    } else {
                        spdlog::error("Cannot try jmp hook for XInputGetState (1_4) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            } else {
                spdlog::error("[XInputHook] Failed to find XInputGetState");
            }

            if (set_state_fn != nullptr) {
                m_xinput_1_4_set_state_hook = safetyhook::create_inline(set_state_fn, set_state_hook_1_4);

                if (!m_xinput_1_4_set_state_hook) {
                    spdlog::error("Failed to hook XInputSetState (1_4), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)set_state_fn);

                    if (jmp_addr != (uintptr_t)set_state_fn) {
                        m_xinput_1_4_set_state_hook = safetyhook::create_inline((void*)jmp_addr, set_state_hook_1_4);

                        if (!m_xinput_1_4_set_state_hook) {
                            spdlog::error("Failed to hook XInputSetState (1_4) (jmp)");
                        }
                    } else {
                        spdlog::error("Cannot try jmp hook for XInputSetState (1_4) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            } else {
                spdlog::error("[XInputHook] Failed to find XInputSetState");
            }
        }

         spdlog::info("[XInputHook] Done (1_4)");
    };

    auto perform_hooks_1_3 = [&]() {
        const auto xinput_1_3_dll = find_dll("xinput1_3.dll");

        if (xinput_1_3_dll != nullptr) {
            std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

            const auto get_state_fn = (void*)GetProcAddress(xinput_1_3_dll, "XInputGetState");
            const auto set_state_fn = (void*)GetProcAddress(xinput_1_3_dll, "XInputSetState");

            if (get_state_fn != nullptr) {
                m_xinput_1_3_get_state_hook = safetyhook::create_inline(get_state_fn, get_state_hook_1_3);

                if (!m_xinput_1_3_get_state_hook) {
                    spdlog::error("Failed to hook XInputGetState (1_3), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)get_state_fn);

                    if (jmp_addr != (uintptr_t)get_state_fn) {
                        m_xinput_1_3_get_state_hook = safetyhook::create_inline((void*)jmp_addr, get_state_hook_1_3);

                        if (!m_xinput_1_3_get_state_hook) {
                            spdlog::error("Failed to hook XInputGetState (1_3) (jmp)");
                        }
                    } else {
                        spdlog::error("Cannot try jmp hook for XInputGetState (1_3) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            } else {
                spdlog::error("[XInputHook] Failed to find XInputGetState");
            }

            if (set_state_fn != nullptr) {
                m_xinput_1_3_set_state_hook = safetyhook::create_inline(set_state_fn, set_state_hook_1_3);

                if (!m_xinput_1_3_set_state_hook) {
                    spdlog::error("Failed to hook XInputSetState (1_3), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)set_state_fn);

                    if (jmp_addr != (uintptr_t)set_state_fn) {
                        m_xinput_1_3_set_state_hook = safetyhook::create_inline((void*)jmp_addr, set_state_hook_1_3);

                        if (!m_xinput_1_3_set_state_hook) {
                            spdlog::error("Failed to hook XInputSetState (1_3) (jmp)");
                        }
                    } else {
                        spdlog::error("Cannot try jmp hook for XInputSetState (1_3) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            } else {
                spdlog::error("[XInputHook] Failed to find XInputSetState");
            }
        }

        spdlog::info("[XInputHook] Done (1_3)");
    };

    // We use a thread because this may possibly take a while to search for the DLLs
    // and it shouldn't cause any issues anyway
    spdlog::info("[XInputHook] Starting hook thread");
    m_hook_thread_1_4 = std::make_unique<std::jthread>(perform_hooks_1_4);
    m_hook_thread_1_3 = std::make_unique<std::jthread>(perform_hooks_1_3);
    spdlog::info("[XInputHook] Hook thread started");
}

uint32_t XInputHook::get_state_hook_1_4(uint32_t user_index, XINPUT_STATE* state) {
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_4_get_state_hook.call<uint32_t>(user_index, state);
    }

    // Native Stereo only calls FViewport::Draw (which is where the engine polls/pumps input)
    // once per engine tick. Synchronized Sequential manually re-invokes FViewport::Draw a
    // second time per tick to draw the second eye, which re-enters this hook for what is
    // logically the same input frame. Re-polling the physical controller a second time can
    // return a subtly different state (a button released/pressed a few ms later), which makes
    // a single physical press look like a press/release/press transition to the engine's own
    // edge-triggered input handling (e.g. UI confirm/back), even though mods are only
    // dispatched once. To keep behavior identical to Native Stereo, replay the exact cached
    // state from the primary poll during the forced second pass instead of re-polling.
    auto& vr = VR::get();
    const auto is_forced_second_pass = user_index < XUSER_MAX_COUNT
        && [&]() {
            auto& stereo_hook = vr->get_fake_stereo_hook();
            return stereo_hook != nullptr && stereo_hook->is_in_synced_forced_viewport_draw();
        }();

    if (is_forced_second_pass && g_hook->s_has_cached_state_1_4[user_index]) {
        *state = g_hook->s_cached_state_1_4[user_index];
        return g_hook->s_cached_ret_1_4[user_index];
    }

    auto ret = g_hook->m_xinput_1_4_get_state_hook.call<uint32_t>(user_index, state);

    detail::log_raw_xinput_diag("get_state_1_4", user_index, ret, state);

    if (user_index < XUSER_MAX_COUNT && !is_forced_second_pass) {
        g_hook->s_cached_state_1_4[user_index] = *state;
        g_hook->s_cached_ret_1_4[user_index] = ret;
        g_hook->s_has_cached_state_1_4[user_index] = true;
    }

    if (is_forced_second_pass) {
        return ret;
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_get_state(&ret, user_index, state);
    }

    return ret;
}

uint32_t XInputHook::set_state_hook_1_4(uint32_t user_index, XINPUT_VIBRATION* vibration) {
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_4_set_state_hook.call<uint32_t>(user_index, vibration);
    }

    auto ret = g_hook->m_xinput_1_4_set_state_hook.call<uint32_t>(user_index, vibration);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_set_state(&ret, user_index, vibration);
    }

    return ret;
}

uint32_t XInputHook::get_state_hook_1_3(uint32_t user_index, XINPUT_STATE* state) {
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_3_get_state_hook.call<uint32_t>(user_index, state);
    }

    // See comment in get_state_hook_1_4 for why the forced second draw pass replays cached state.
    auto& vr = VR::get();
    const auto is_forced_second_pass = user_index < XUSER_MAX_COUNT
        && [&]() {
            auto& stereo_hook = vr->get_fake_stereo_hook();
            return stereo_hook != nullptr && stereo_hook->is_in_synced_forced_viewport_draw();
        }();

    if (is_forced_second_pass && g_hook->s_has_cached_state_1_3[user_index]) {
        *state = g_hook->s_cached_state_1_3[user_index];
        return g_hook->s_cached_ret_1_3[user_index];
    }

    auto ret = g_hook->m_xinput_1_3_get_state_hook.call<uint32_t>(user_index, state);

    detail::log_raw_xinput_diag("get_state_1_3", user_index, ret, state);

    if (user_index < XUSER_MAX_COUNT && !is_forced_second_pass) {
        g_hook->s_cached_state_1_3[user_index] = *state;
        g_hook->s_cached_ret_1_3[user_index] = ret;
        g_hook->s_has_cached_state_1_3[user_index] = true;
    }

    if (is_forced_second_pass) {
        return ret;
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_get_state(&ret, user_index, state);
    }

    return ret;
}

uint32_t XInputHook::set_state_hook_1_3(uint32_t user_index, XINPUT_VIBRATION* vibration) {
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_3_set_state_hook.call<uint32_t>(user_index, vibration);
    }

    auto ret = g_hook->m_xinput_1_3_set_state_hook.call<uint32_t>(user_index, vibration);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_set_state(&ret, user_index, vibration);
    }

    return ret;
}