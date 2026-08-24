#pragma once

#include <thread>

#include <safetyhook/inline_hook.hpp>
#include <Xinput.h>

class XInputHook {
public:
    XInputHook();

private:
    static uint32_t get_state_hook_1_4(uint32_t user_index, XINPUT_STATE* state);
    static uint32_t set_state_hook_1_4(uint32_t user_index, XINPUT_VIBRATION* vibration);

    static uint32_t get_state_hook_1_3(uint32_t user_index, XINPUT_STATE* state);
    static uint32_t set_state_hook_1_3(uint32_t user_index, XINPUT_VIBRATION* vibration);

    safetyhook::InlineHook m_xinput_1_4_get_state_hook;
    safetyhook::InlineHook m_xinput_1_4_set_state_hook;
    safetyhook::InlineHook m_xinput_1_3_get_state_hook;
    safetyhook::InlineHook m_xinput_1_3_set_state_hook;

    // Cached state from the primary (non-forced) poll each tick, keyed by user_index.
    // Used so the forced second FViewport::Draw pass in Synchronized Sequential mode
    // replays byte-identical input instead of re-polling the physical controller, which
    // can otherwise return a subtly different state a few ms later and make the engine's
    // own edge-triggered input handling (e.g. button confirm/back) see a spurious
    // press/release transition within a single logical frame.
    static inline XINPUT_STATE s_cached_state_1_4[XUSER_MAX_COUNT]{};
    static inline uint32_t s_cached_ret_1_4[XUSER_MAX_COUNT]{};
    static inline bool s_has_cached_state_1_4[XUSER_MAX_COUNT]{};

    static inline XINPUT_STATE s_cached_state_1_3[XUSER_MAX_COUNT]{};
    static inline uint32_t s_cached_ret_1_3[XUSER_MAX_COUNT]{};
    static inline bool s_has_cached_state_1_3[XUSER_MAX_COUNT]{};

    std::unique_ptr<std::jthread> m_hook_thread_1_4{};
    std::unique_ptr<std::jthread> m_hook_thread_1_3{};
};