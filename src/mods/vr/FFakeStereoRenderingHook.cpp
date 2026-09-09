#define NOMINMAX

#include <windows.h>
#include <winternl.h>
#include <unordered_set>
#include <array>
#include <cmath>

#include <asmjit/asmjit.h>
#include <future>
#include <filesystem>
#include <unordered_map>
#include <thread>
#include <mutex>

#include <spdlog/spdlog.h>
#include <utility/Memory.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/String.hpp>
#include <utility/Thread.hpp>
#include <utility/Emulation.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/EngineModule.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameEngine.hpp>
#include <sdk/CVar.hpp>
#include <sdk/Slate.hpp>
#include <sdk/DynamicRHI.hpp>
#include <sdk/FViewportInfo.hpp>
#include <sdk/Utility.hpp>
#include <sdk/RHICommandList.hpp>
#include <sdk/UGameViewportClient.hpp>
#include <sdk/Globals.hpp>
#include <sdk/FName.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FViewport.hpp>
#include <sdk/UKismetRenderingLibrary.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/FSceneViewFamily.hpp>

#include <sdk/UGameplayStatics.hpp>
#include <sdk/APawn.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/FTextureRenderTargetResource.hpp>

#include "Framework.hpp"
#include "Mods.hpp"
#include "mods/UObjectHook.hpp"

#include <bdshemu.h>
#include <bddisasm.h>
#include <disasmtypes.h>

#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/threading/RenderThreadWorker.hpp>
#include <sdk/threading/RHIThreadWorker.hpp>
#include "../VR.hpp"
#include "../../utility/Logging.hpp"

#include "FFakeStereoRenderingHook.hpp"

#include <tracy/Tracy.hpp>
#include "uevr/API.hpp"
#include <cstdint> // Ensure uint32_t / uintptr_t are available
#include <atomic>

//#define FFAKE_STEREO_RENDERING_LOG_ALL_CALLS

FFakeStereoRenderingHook* g_hook = nullptr;
uint32_t g_frame_count{};

// While a new level is streaming in (player controller not yet spawned), the scene-capture actor
// can be repeatedly garbage-collected/invalidated by the engine well after bIsTearingDown has
// cleared and the engine tick has resumed ticking normally. Neither of those signals catch this
// window, which is why create_scene_capture() was observed retriggering every frame for 70+
// seconds straight during a level transition even though tick_stalled/is_loading both read false.
// D3D12Component.cpp already uses this exact check (uevr::API::get()->get_player_controller(0))
// to gate its own scene-capture texture setup, so reuse it here as an additional loading signal.
static bool is_local_player_controller_missing() {
    auto& api = uevr::API::get();
    return api == nullptr || api->get_player_controller(0) == nullptr;
}

// A player controller can exist well before the player is actually in control of a pawn in the
// world (e.g. during level streaming, cutscenes, or the initial boot loading screen where the
// controller is spawned early but possession hasn't happened yet). Requiring an actual possessed
// pawn is a much stronger/harder-to-fool "the player is really in-game" signal than merely
// checking for a non-null player controller, since a missing pawn cannot be satisfied by the
// loading screen/transient boot world alone the way frame-pacing or controller-existence can be.
static bool is_local_pawn_missing() {
    auto& api = uevr::API::get();
    return api == nullptr || api->get_local_pawn(0) == nullptr;
}

// The existing loading guards (bIsTearingDown / tick-stalled / no-player-controller) only detect
// LEVEL TRANSITIONS after the game has already finished its initial boot. During the very first
// load into a map, none of those signals fire (world is valid, tick is running, player controller
// exists) even though the engine is still churning through asset streaming/shader compilation at a
// tiny fraction of normal frame rate. Observed in the wild: begin_render_viewfamily_real firing
// roughly once every ~2 seconds during the "stuck at 10%" boot screen. Because none of the existing
// guards catch this, create_scene_capture() (actor spawn + full D3D12 RTV/SRV setup) was retriggering
// every single one of those rare frames, since the actor/world it was spawned into gets explicitly
// destroyed (not just GC'd) as the transient boot/loading world tears itself down between stages.
//
// IMPORTANT: the native Unreal loading screen itself (Slate spinner/UI) renders CHEAPLY and FAST,
// so "30 fast frames in a row" can be satisfied almost instantly by the loading screen alone, well
// before the underlying world/streaming is actually ready. That was observed directly: this tracker
// ended after ~1.6s, the same-pass stereo capture then committed to full dual-view rendering, and
// THAT is what actually starved the boot sequence for the next ~30 seconds (each frame taking ~2s).
// None of bIsTearingDown/tick-stalled/no-player-controller fired during that 30s stall either, so
// this tracker cannot be a one-shot latch - it must keep monitoring frame pacing even after first
// reaching "complete", and re-arm (go back to suppressing) if slow frames resume for a sustained
// run. This makes it self-correcting instead of trusting a single early fast-frame burst forever.
static std::atomic<bool> g_boot_phase_active{true};

// Must be called exactly once per rendered frame (from begin_render_viewfamily_real) to advance the
// frame-pacing tracker. Other call sites should read g_boot_phase_active directly instead.
static void update_boot_phase_tracking() {
    static std::chrono::steady_clock::time_point s_boot_first_frame_time{};
    static std::chrono::steady_clock::time_point s_last_frame_time{};
    static uint32_t s_consecutive_fast_frames{0};
    static uint32_t s_consecutive_slow_frames{0};

    const auto now = std::chrono::steady_clock::now();

    if (s_boot_first_frame_time.time_since_epoch().count() == 0) {
        s_boot_first_frame_time = now;
        s_last_frame_time = now;
        return;
    }

    static constexpr auto fast_frame_threshold = std::chrono::milliseconds(200);
    // Anything this slow is well outside normal frame pacing (even accounting for hitches) and is a
    // strong signal that the render thread is starved/blocked, e.g. by the exact same-pass secondary
    // view cascade this tracker exists to gate. Require several in a row to avoid false positives
    // from a single one-off hitch (shader compile, disk stall, etc).
    static constexpr auto slow_frame_threshold = std::chrono::milliseconds(750);
    static constexpr uint32_t required_consecutive_fast_frames = 30;
    static constexpr uint32_t required_consecutive_slow_frames = 3;
    static constexpr auto max_boot_duration = std::chrono::seconds(30); // safety valve, only applies while still active

    const auto dt = now - s_last_frame_time;
    s_last_frame_time = now;

    const bool currently_active = g_boot_phase_active.load(std::memory_order_relaxed);

    if (!currently_active) {
        // Re-arm if slow frames resume even after we previously considered boot complete - this is
        // the case that a one-shot latch missed entirely (fast loading-screen frames satisfied the
        // original check, then the secondary-view cascade itself caused sustained slow frames).
        if (dt >= slow_frame_threshold) {
            ++s_consecutive_slow_frames;
        } else {
            s_consecutive_slow_frames = 0;
        }

        if (s_consecutive_slow_frames >= required_consecutive_slow_frames) {
            SPDLOG_WARN("[VR] Boot-phase scene-capture suppression RE-ARMED after {} consecutive slow frames (dt={}ms) - "
                        "something is still starving the render thread post-boot.",
                s_consecutive_slow_frames, std::chrono::duration_cast<std::chrono::milliseconds>(dt).count());
            g_boot_phase_active.store(true, std::memory_order_relaxed);
            s_consecutive_fast_frames = 0;
            s_consecutive_slow_frames = 0;
            s_boot_first_frame_time = now;

            // Tear down the existing scene capture (and its expensive secondary-view rendering)
            // immediately rather than waiting for it to naturally invalidate - the other loading
            // guards only re-check is_loading when the render target is already null, so without
            // this the re-arm above would have no effect until something else destroyed it.
            if (g_hook != nullptr) {
                if (auto rtm = g_hook->get_render_target_manager(); rtm != nullptr) {
                    rtm->destroy_scene_capture();
                }
            }
        }

        return;
    }

    s_consecutive_slow_frames = 0;

    if (dt < fast_frame_threshold) {
        ++s_consecutive_fast_frames;
    } else {
        s_consecutive_fast_frames = 0;
    }

    if (s_consecutive_fast_frames >= required_consecutive_fast_frames || (now - s_boot_first_frame_time) > max_boot_duration) {
        g_boot_phase_active.store(false, std::memory_order_relaxed);
        SPDLOG_INFO("[VR] Boot-phase scene-capture suppression window ended (consecutive_fast_frames={}, elapsed={}ms)",
            s_consecutive_fast_frames, std::chrono::duration_cast<std::chrono::milliseconds>(now - s_boot_first_frame_time).count());
    }
}

static bool is_boot_phase_active() {
    return g_boot_phase_active.load(std::memory_order_relaxed);
}

// A/B testing switch: exposed in the UEVR ImGui menu under "Native Stereo Fix" as
// "Disable Loading Guards (A/B testing)", or via environment variable
// UEVR_DISABLE_LOADING_GUARDS=1 before launching the game. Bypasses ALL of the
// loading-detection heuristics added above (tick-stall, no-player-controller, no-local-pawn,
// boot-phase frame-pacing) in one shot, reverting create_scene_capture() gating to its original
// pre-guard behavior. This lets the effect of these guards be directly A/B tested against
// baseline without having to comment out code and rebuild each time.
static bool are_loading_guards_disabled() {
    static const bool env_disabled = [] {
        char buf[8]{};
        const auto len = GetEnvironmentVariableA("UEVR_DISABLE_LOADING_GUARDS", buf, sizeof(buf));
        const bool result = len > 0 && buf[0] == '1';

        if (result) {
            SPDLOG_WARN("[VR] UEVR_DISABLE_LOADING_GUARDS=1 detected - all loading-detection guards "
                        "(tick-stall/no-controller/no-pawn/boot-phase) are DISABLED for A/B testing.");
        }

        return result;
    }();

    if (env_disabled) {
        return true;
    }

    return VR::get()->are_loading_guards_disabled();
}

// Scan through function instructions to detect usage of double

bool is_using_double_precision(uintptr_t addr) {
    SPDLOG_INFO("Scanning function at {:x} for double precision usage", addr);

    bool result = false;

    utility::exhaustive_decode((uint8_t*)addr, 50, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
        if (std::string_view{ix.Mnemonic}.starts_with("CALL")) {
            return utility::ExhaustionResult::STEP_OVER;
        }

        if (ix.Instruction == ND_INS_MOVSD && ix.Operands[0].Type == ND_OP_MEM && ix.Operands[1].Type == ND_OP_REG) {
            SPDLOG_INFO("[UE5 Detected] Detected Double precision MOVSD at {:x}", (uintptr_t)ip);
            result = true;
            return utility::ExhaustionResult::BREAK;
        }

        if (ix.Instruction == ND_INS_ADDSD) {
            SPDLOG_INFO("[UE5 Detected] Detected Double precision ADDSD at {:x}", (uintptr_t)ip);
            result = true;
            return utility::ExhaustionResult::BREAK;
        }

        return utility::ExhaustionResult::CONTINUE;
    });

    return result;
}

FFakeStereoRenderingHook::FFakeStereoRenderingHook() {
    g_hook = this;
    setup_options();
}

void FFakeStereoRenderingHook::on_frame() {
    attempt_hook_game_engine_tick();
    attempt_hook_slate_thread();
    attempt_hook_fsceneview_constructor();

    // Ideally we want to do all hooking
    // from game engine tick. if it fails
    // we will fall back to doing it here.
    if (!m_hooked_game_engine_tick && m_attempted_hook_game_engine_tick) {
        attempt_hooking();
    }
}


void FFakeStereoRenderingHook::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Stereo Hook Options")) {
        m_asynchronous_scan->draw("Asynchronous Code Scanning");
        m_recreate_textures_on_reset->draw("Recreate Textures on Reset");
        m_frame_delay_compensation->draw("Frame Delay Compensation");
        m_use_fmalloc_scene_view_extensions->draw("Use FMalloc for ISceneViewExtensions");

        if (m_tracking_system_hook != nullptr) {
            m_tracking_system_hook->on_draw_ui();
        }

#if 0
        if (ImGui::Button("Spawn scene capture")) {
            get_render_target_manager()->create_scene_capture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Destroy scene capture")) {
            get_render_target_manager()->destroy_scene_capture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Create texture")) {
            get_render_target_manager()->create_scene_capture_texture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Destroy texture")) {
            get_render_target_manager()->destroy_scene_capture();
        }

        bool status = false;

        if (get_render_target_manager()->get_scene_capture_utexture() != nullptr) {
            if (UObjectHook::get()->exists(get_render_target_manager()->get_scene_capture_utexture())) {
                status = true;
            }
        }
        ImGui::Text("Scene Capture Texture: %s", status ? "Exists" : "Does not exist");
#endif

        auto& data = m_viewport_rt_hook_data;
        std::scoped_lock _{data.retaddr_mutex};

        std::vector<uintptr_t> retaddrs{};
        std::vector<std::string> items{};
        for (auto& addr : data.seen_retaddrs) {
            items.push_back(fmt::format("{:x}", addr));
            retaddrs.push_back(addr);
        }
        
        std::vector<const char*> citems{};
        for (auto& item : items) {
            citems.push_back(item.c_str());
        }

        if (!items.empty()) {
            if (ImGui::BeginCombo("GetRenderTargetTexture Retaddrs", items[data.selected_retaddr].c_str())) {
                for (int n = 0; n < items.size(); n++) {
                    ImGui::PushID(n);
                    auto retaddr = retaddrs[n];
                    const bool is_selected = (data.selected_retaddr == n);

                    // Calculate the text size for the current item
                    const auto text_size = ImGui::CalcTextSize(items[n].c_str(), NULL, true);
                    const auto padding = ImGui::GetStyle().ItemSpacing.x;
                    const auto selectable_size = ImVec2{text_size.x + padding, text_size.y};

                    if (ImGui::Selectable(items[n].c_str(), is_selected, ImGuiSelectableFlags_None, selectable_size)) {
                        data.selected_retaddr = n;
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Call Original")) {
                        data.call_original_retaddrs.insert(retaddr);
                        data.redirected_retaddrs.erase(retaddr);
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Redirect")) {
                        data.redirected_retaddrs.insert(retaddr);
                        data.call_original_retaddrs.erase(retaddr);
                    }

                    ImGui::SameLine();
                    if (data.call_original_retaddrs.contains(retaddr)) {
                        ImGui::Text("[Calling Original]");
                    } else if (data.redirected_retaddrs.contains(retaddr)) {
                        ImGui::Text("[Redirected]");
                    } else {
                        ImGui::Text("[Default]");
                    }

                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

        }

        ImGui::TreePop();
    }

    ImGui::Separator();
}

void FFakeStereoRenderingHook::attempt_hooking() {
    if (m_finished_hooking || m_tried_hooking) {
        return;
    }

    // TODO: see if this can be threaded; it might not be able to because of TLS or something
    if (!VR::get()->should_skip_uobjectarray_init()) {
        sdk::FName::get_constructor();
        sdk::FName::get_to_string();
        sdk::FUObjectArray::get();
    }

    if (!m_injected_stereo_at_runtime) {
        attempt_runtime_inject_stereo();
        m_injected_stereo_at_runtime = true;
    }
    
    m_hooked = hook();
}

namespace detail{
bool pre_find_engine_tick() {
    ZoneScopedN(__FUNCTION__);
    sdk::UGameEngine::get_tick_address(); // this takes a LONG time to find
    sdk::UGameEngine::get_initialize_hmd_device_address();
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_game_engine_tick(uintptr_t return_address) {
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_engine_tick);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    if (m_hooked_game_engine_tick) {
        return;
    }

    if (return_address == 0 && m_attempted_hook_game_engine_tick) {
        return;
    }
    
    SPDLOG_INFO("Attempting to hook UGameEngine::Tick!");

    m_attempted_hook_game_engine_tick = true;

    auto func = sdk::UGameEngine::get_tick_address();

    if (!func) {
        if (return_address == 0) {
            SPDLOG_ERROR("Cannot hook UGameEngine::Tick");
            return;
        }

        const auto engine_module = sdk::get_ue_module(L"Engine");
        static const auto negative_delta_time_strings = 
            utility::scan_strings(engine_module, L"Negative delta time!");
        
        if (negative_delta_time_strings.empty()) {
            SPDLOG_ERROR("Cannot hook UGameEngine::Tick (Negative delta time! not found)");
            return;
        }

        static std::vector<uintptr_t> negative_delta_time_funcs = [&]() {
            std::vector<uintptr_t> out{};

            for (auto str : negative_delta_time_strings) {
                const auto ref = utility::scan_displacement_reference(engine_module, str);

                if (!ref) {
                    continue;
                }
                //
                const auto func_start = utility::find_virtual_function_start(*ref);

                if (!func_start) {
                    continue;
                }

                SPDLOG_INFO("Negative delta time string function @ {:x}", *func_start);

                out.push_back(*func_start);
            }

            return out;
        }();

        const auto return_address_func = utility::find_virtual_function_start(return_address);

        if (!return_address_func) {
            SPDLOG_ERROR("Return address is not within a valid function!");
            return;
        }

        // Check if the return address is within one of the negative delta time functions.
        // If it is, then it's UGameEngine::Tick. Set func to the return_address_func.
        for (auto potential : negative_delta_time_funcs) {
            if (potential == *return_address_func) {
                SPDLOG_INFO("Found UGameEngine::Tick @ {:x}", *return_address_func);
                func = *return_address_func;
                break;
            }
        }

        if (!func) {
            SPDLOG_ERROR("Return address is not the correct function!");
            return;
        }
    }

    // TODO: move this to a better place
    m_tick_hook = safetyhook::create_inline((void*)*func, &engine_tick_hook, safetyhook::InlineHook::StartDisabled);

    if (!m_tick_hook) {
        SPDLOG_ERROR("Failed to hook UGameEngine::Tick!");
        return;
    }

    if (auto tick_hook_enable = m_tick_hook.enable(); !tick_hook_enable.has_value()) {
        SPDLOG_ERROR("Failed to enable UGameEngine::Tick hook! {}", (int)tick_hook_enable.error().type);
        return;
    }

    m_hooked_game_engine_tick = true;

    SPDLOG_INFO("Hooked UGameEngine::Tick!");
}

void* FFakeStereoRenderingHook::engine_tick_hook(sdk::UGameEngine* engine, float delta, bool idle) {
    ZoneScopedN("UGameEngine::Tick Hook");
    FrameMarkStart("UGameEngine::Tick");

    auto hook = g_hook;
    
    hook->m_in_engine_tick = true;

    utility::ScopeGuard _{[]() {
        g_hook->m_in_engine_tick = false;
        FrameMarkEnd("UGameEngine::Tick");
    }};
    
    static bool once = true;

    if (once) {
        SPDLOG_INFO("First time calling UGameEngine::Tick!");
        once = false;
    }

    if (!g_framework->is_game_data_intialized()) {
        // This allocates memory on the stack.
        static bool check_canary_once = true;
        volatile uint64_t shadow_space[64]{};

#ifdef NDEBUG
        if (check_canary_once) {
#endif
            std::memset((void*)shadow_space, 0, 64 * sizeof(uint64_t));
#ifdef NDEBUG
        }
#endif
        // We're using original here instead of call_unsafe to make sure the canaries are the first thing on the stack.
        void* result = hook->m_tick_hook.original<void* (*)(sdk::UGameEngine*, float, bool)>()(engine, delta, idle);

        // At least do some logic with the shadow space so it doesn't get optimized out for some reason.
        // But only do it once in release builds.
#ifdef NDEBUG
        if (check_canary_once) {
#endif
            for (size_t i = 0; i < 64; ++i) {
                if (shadow_space[i] != 0) {
                    SPDLOG_ERROR("[UGameEngine::Tick] Shadow space was overwritten! {:x} @ {}", shadow_space[i], i);
                }
            }

#ifdef NDEBUG
            check_canary_once = false;
        }
#endif

        return result;
    }

    hook->attempt_hooking();

    // Best place to run game thread jobs.
    GameThreadWorker::get().execute();

    if (hook->m_ignore_next_engine_tick) {
        hook->m_ignored_engine_delta = delta;
        hook->m_ignore_next_engine_tick = false;
        return nullptr;
    }
    
    g_framework->enable_engine_thread();
    g_framework->run_imgui_frame(false);

    delta += hook->m_ignored_engine_delta;
    hook->m_ignored_engine_delta = 0.0f;

    if (hook->m_tracking_system_hook != nullptr) {
        hook->m_tracking_system_hook->on_pre_engine_tick(engine, delta);
    }

    const auto& mods = g_framework->get_mods()->get_mods();
    for (auto& mod : mods) {
        mod->on_pre_engine_tick(engine, delta);
    }

    void* result = nullptr;

    {
        // This allocates memory on the stack.
        static bool check_canary_once = true;
        volatile uint64_t shadow_space[64]{};

#ifdef NDEBUG
        if (check_canary_once) {
#endif
            std::memset((void*)shadow_space, 0, 64 * sizeof(uint64_t));
#ifdef NDEBUG
        }
#endif
        // We're using original here instead of call_unsafe to make sure the canaries are the first thing on the stack.
        result = hook->m_tick_hook.original<void* (*)(sdk::UGameEngine*, float, bool)>()(engine, delta, idle);

        // At least do some logic with the shadow space so it doesn't get optimized out for some reason.
        // But only do it once in release builds.
#ifdef NDEBUG
        if (check_canary_once) {
#endif
            for (size_t i = 0; i < 64; ++i) {
                if (shadow_space[i] != 0) {
                    SPDLOG_ERROR("[UGameEngine::Tick] Shadow space was overwritten! {:x} @ {}", shadow_space[i], i);
                }
            }

#ifdef NDEBUG
            check_canary_once = false;
        }
#endif
    }

    for (auto& mod : mods) {
        mod->on_post_engine_tick(engine, delta);
    }

    return result;
}

namespace detail{
bool pre_find_slate_thread() {
    sdk::slate::locate_draw_window_renderthread_fn(); // Can take a while to find
    sdk::slate::locate_draw_window_renderthread_fn_alternate();
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_slate_thread(uintptr_t return_address, bool alternate) {
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_slate_thread);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    if (m_hooked_slate_thread && !alternate) {
        return;
    }

    const auto attempted = alternate ? m_attempted_hook_slate_thread_alternate : m_attempted_hook_slate_thread;

    if (return_address == 0 && attempted) {
        return;
    }

    SPDLOG_INFO("Attempting to hook FSlateRHIRenderer::DrawWindow_RenderThread!");

    if (alternate) {
        SPDLOG_INFO("Using alternate method to hook FSlateRHIRenderer::DrawWindow_RenderThread!");
        m_attempted_hook_slate_thread_alternate = true;
    } else {
        m_attempted_hook_slate_thread = true;
    }

    auto func = alternate ? sdk::slate::locate_draw_window_renderthread_fn_alternate() : sdk::slate::locate_draw_window_renderthread_fn();

    if (!func && return_address == 0) {
        SPDLOG_ERROR("Cannot hook FSlateRHIRenderer::DrawWindow_RenderThread");
        return;
    }

    if (return_address != 0) {
        func = utility::find_function_start_with_call(return_address);

        if (!func) {
            SPDLOG_ERROR("Cannot hook FSlateRHIRenderer::DrawWindow_RenderThread with alternative return address method");
            m_hooked_slate_thread = true; // not actually true but just to stop spamming the scans
            return;
        }

        SPDLOG_INFO("Checking if the assembly listing for {:X} is really small", *func);

        // Check if the assembly listing for this function is really small. It shouldn't be really small.
        // This will happen on UE 5.5+ where RenderTexture_RenderThread is enqueued inside of a lambda.
        size_t distance_to_ret = 0;
        utility::exhaustive_decode((uint8_t*)*func, 1000, [&](utility::ExhaustionContext& ctx2) -> utility::ExhaustionResult {
            ++distance_to_ret;

            if (ctx2.instrux.BranchInfo.IsBranch && std::string_view{ctx2.instrux.Mnemonic}.starts_with("CALL")) {
                return utility::ExhaustionResult::STEP_OVER;
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        if (distance_to_ret < 50) {
            SPDLOG_ERROR("FSlateRHIRenderer::DrawWindow_RenderThread function is too small! Distance to RET: {}", distance_to_ret);
            m_hooked_slate_thread = true; // not actually true but just to stop spamming the scans
            return;
        }

        SPDLOG_INFO("Found FSlateRHIRenderer::DrawWindow_RenderThread with alternative return address method: {:x}", *func);
    }

    m_slate_thread_hook = safetyhook::create_inline((void*)*func, &FFakeStereoRenderingHook::slate_draw_window_render_thread, safetyhook::InlineHook::StartDisabled);
    m_hooked_slate_thread = true;

    if (!m_slate_thread_hook) {
        SPDLOG_ERROR("Failed to hook FSlateRHIRenderer::DrawWindow_RenderThread!");
        return;
    }

    if (auto enable_result = m_slate_thread_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable FSlateRHIRenderer::DrawWindow_RenderThread hook! {}", (int)enable_result.error().type);
        return;
    }

    SPDLOG_INFO("Hooked FSlateRHIRenderer::DrawWindow_RenderThread!");
}

namespace detail{
bool pre_find_fsceneview_constructor() {
    sdk::FSceneView::get_constructor_address(); // Can take a while to find
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_fsceneview_constructor() {
    if (m_attempted_hook_fsceneview_constructor) {
        return;
    }
    
    // just try to find it before ghosting fix is even enabled
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_fsceneview_constructor);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    auto& vr = VR::get();

    if (!vr->is_ghosting_fix_enabled() && !vr->is_splitscreen_compatibility_enabled() && !vr->is_sceneview_compatibility_enabled() && !vr->is_native_stereo_fix_enabled()) {
        return;
    }

    utility::ScopeGuard _{[&]() {
        m_attempted_hook_fsceneview_constructor = true;
    }};

    SPDLOG_INFO("Attempting to hook FSceneView::FSceneView constructor!");
    const auto constructor = sdk::FSceneView::get_constructor_address();

    if (!constructor) {
        SPDLOG_ERROR("Cannot hook FSceneView::FSceneView constructor");
        return;
    }

    g_hook->m_sceneview_data.constructor_hook = safetyhook::create_inline(*constructor, (uintptr_t)&sceneview_constructor, safetyhook::InlineHook::StartDisabled);

    if (!g_hook->m_sceneview_data.constructor_hook) {
        SPDLOG_ERROR("Failed to hook FSceneView::FSceneView constructor!");
        return;
    }

    if (auto enable_result = g_hook->m_sceneview_data.constructor_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable FSceneView::FSceneView constructor hook! {}", (int)enable_result.error().type);
        return;
    }

    SPDLOG_INFO("Hooked FSceneView::FSceneView constructor!");
}

bool FFakeStereoRenderingHook::hook() {
    SPDLOG_INFO("Entering FFakeStereoRenderingHook::hook");

    m_tried_hooking = true;

    // Locking the hook monitor mutex stops our code from trying to re-hook DX11 and 12 after
    // Long pauses in code execution, due to us doing massive scans for code in this function.
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    const auto vtable = locate_fake_stereo_rendering_vtable();

    // This happens if games have intentionally removed the stereo initialization functions and stereo emulation classes.
    // So we need to manually create the stereo device.
    if (!vtable) {
        SPDLOG_ERROR("Failed to locate Fake Stereo Rendering VTable, attempting to perform nonstandard hook");

        auto check_file_version = [](uint32_t ms, uint32_t ls) {
            try {
                const auto full_path = utility::get_module_pathw(utility::get_executable());

                if (!full_path) {
                    SPDLOG_ERROR("Failed to get executable path, falling back");
                    return false;
                }

                const auto file_version_size = GetFileVersionInfoSizeW(full_path->c_str(), nullptr);

                if (file_version_size == 0) {
                    SPDLOG_ERROR("Failed to get file version info size, falling back");
                    return false;
                }

                std::vector<uint8_t> file_version_data(file_version_size);
                GetFileVersionInfoW(full_path->c_str(), 0, file_version_size, file_version_data.data());

                UINT size{};
                VS_FIXEDFILEINFO* fixed_file_info{};

                if (VerQueryValueA(file_version_data.data(), "\\", (LPVOID*)&fixed_file_info, &size) && fixed_file_info != nullptr) {
                    SPDLOG_INFO("MS: {:x}, LS: {:x}", fixed_file_info->dwFileVersionMS, fixed_file_info->dwFileVersionLS);

                    if (fixed_file_info->dwFileVersionMS == ms && fixed_file_info->dwFileVersionLS == ls) {
                        SPDLOG_INFO("Found matching executable, attempting to perform nonstandard hook");
                        return true;
                    } else {
                        SPDLOG_INFO("File does not match requested version, falling back");
                    }
                } else {
                    SPDLOG_ERROR("Failed to get file version info, falling back");
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to get file version info, falling back");
            }

            return false;
        };

        const auto found_version = sdk::search_for_version(utility::get_executable());

        if (!found_version) {
            SPDLOG_WARN("Failed to find version in executable");
        }

        // Check for version 4.27.2.0
        // 4.26 also works here
        if (check_file_version(0x4001B, 0x20000) || found_version.value_or(L"") == L"4.26") {
            return nonstandard_create_stereo_device_hook_4_27();
        }

        // Check for version 4.22.3.0
        if (check_file_version(0x40016, 0x30000)) {
            return nonstandard_create_stereo_device_hook_4_22();
        }

        // Check for version 4.18.3.0
        if (check_file_version(0x40012, 0x30000)) {
            return nonstandard_create_stereo_device_hook_4_18();
        }

        return nonstandard_create_stereo_device_hook();
    }

    return standard_fake_stereo_hook(*vtable);
}

bool FFakeStereoRenderingHook::standard_fake_stereo_hook(uintptr_t vtable) {
    ZoneScopedN(__FUNCTION__);
    SPDLOG_INFO("Performing standard fake stereo hook");

    const auto game = sdk::get_ue_module(L"Engine");
    std::array<uint8_t, 0x1000> og_vtable{};
    memcpy(og_vtable.data(), (void*)vtable, og_vtable.size()); // to perform tests on.

    const auto module_vtable_within = utility::get_module_within(vtable);

    // In 4.18 the destructor virtual doesn't exist or is at the very end of the vtable.
    const auto is_stereo_enabled_index = sdk::is_vfunc_pattern(*(uintptr_t*)vtable, "B0 01") ? 0 : 1;
    const auto is_stereo_enabled_func_ptr = &((uintptr_t*)vtable)[is_stereo_enabled_index];

    SPDLOG_INFO("IsStereoEnabled Index: {}", is_stereo_enabled_index);

    const auto stereo_view_offset_index = get_stereo_view_offset_index(vtable);

    if (!stereo_view_offset_index) {
        SPDLOG_ERROR("Failed to locate Stereo View Offset Index");
        return false;
    }

    // Some compiler optimizations cause 31 C0 (xor eax, eax) to be used.
    bool uses_33_c0 = false;

    for (size_t i = 0; i < 30; ++i) try {
        const auto fn = ((uintptr_t*)vtable)[i];

        if (fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) {
            SPDLOG_WARN("Found null function pointer at index {}", i);
            break;
        }

        if (sdk::is_vfunc_pattern(fn, "33 C0")) {
            uses_33_c0 = true;
            SPDLOG_INFO("Found 33 C0 pattern at index {}", i);
            break;
        }
    } catch(...) {

    }

    const auto stereo_projection_matrix_index = *stereo_view_offset_index + 1;
    const auto is_4_18_or_lower = *stereo_view_offset_index <= 6;

    const auto& stereo_view_offset_func = ((uintptr_t*)vtable)[*stereo_view_offset_index];

    auto render_texture_render_thread_func = utility::find_virtual_function_from_string_ref(game, L"RenderTexture_RenderThread");

    // Seems more robust than simply just checking the vtable index.
    m_uses_old_rendertarget_manager = *stereo_view_offset_index <= 11 && !render_texture_render_thread_func;

    SPDLOG_INFO("Using old rendertarget manager: {}", m_uses_old_rendertarget_manager);

    if (!render_texture_render_thread_func) {
        // Fallback scan to checking for the first non-default virtual function (<= 4.18)
        SPDLOG_INFO("Failed to find RenderTexture_RenderThread, falling back to first non-default virtual function");

        for (auto i = 2; i < 10; ++i) {
            const auto func = ((uintptr_t*)vtable)[stereo_projection_matrix_index + i];

            // Some protectors can fool this check, so we also check for the vfunc pattern (emulates the code)
            if (!utility::is_stub_code((uint8_t*)func) && 
                !sdk::is_vfunc_pattern(func, "33 C0") &&
                !sdk::is_vfunc_pattern(func, "32 C0"))
            {
                render_texture_render_thread_func = func;
                break;
            }
        }

        if (!render_texture_render_thread_func) {
            SPDLOG_ERROR("Failed to find RenderTexture_RenderThread");
            return false;
        }
    }

    SPDLOG_INFO("RenderTexture_RenderThread: {:x}", (uintptr_t)*render_texture_render_thread_func);

    // Scan for the function pointer, it should be in the middle of the vtable.
    auto rendertexture_fn_vtable_middle = utility::scan_ptr(vtable + ((stereo_projection_matrix_index + 2) * sizeof(void*)), 50 * sizeof(void*), *render_texture_render_thread_func);

    if (!rendertexture_fn_vtable_middle) {
        SPDLOG_ERROR("Failed to find RenderTexture_RenderThread VTable Middle");
        return false;
    }

    auto rendertexture_fn_vtable_index = (*rendertexture_fn_vtable_middle - vtable) / sizeof(uintptr_t);
    SPDLOG_INFO("RenderTexture_RenderThread VTable Middle: {} {:x}", rendertexture_fn_vtable_index, (uintptr_t)*rendertexture_fn_vtable_middle);

    auto render_target_manager_vtable_index = rendertexture_fn_vtable_index + 1 + (2 * (size_t)is_4_18_or_lower);

    // verify first that the render target manager index is returning a null pointer
    // and if not, scan forward until we run into a vfunc that returns a null pointer
    auto get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

    bool is_4_11 = false;

    //if (!sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "33 C0")) {
        //SPDLOG_INFO("Expected GetRenderTargetManager function at index {} does not return null, scanning forward for return nullptr.", render_target_manager_vtable_index);

        for (;;++render_target_manager_vtable_index) {
            get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

            if (IsBadReadPtr(*(void**)get_render_target_manager_func_ptr, 1)) {
                SPDLOG_ERROR("Failed to find GetRenderTargetManager vtable index, a crash is imminent");
                return false;
            }

            if (sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "33 C0") || (!uses_33_c0 && sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "31 C0"))) {
                const auto distance_from_rendertexture_fn = render_target_manager_vtable_index - rendertexture_fn_vtable_index;

                // means it's 4.17 I think. 12 means 4.11.
                if (distance_from_rendertexture_fn == 10 || distance_from_rendertexture_fn == 11 || distance_from_rendertexture_fn == 12) {
                    is_4_11 = distance_from_rendertexture_fn == 12;
                    m_rendertarget_manager_embedded_in_stereo_device = true;
                    SPDLOG_INFO("Render target manager appears to be directly embedded in the stereo device vtable");
                } else {
                    // Now this may potentially be the correct index, but we're not quite done yet.
                    // On 4.19 (and possibly others), the index is 1 higher than it should be.
                    // We can tell by checking how many functions in front of this index return null.
                    // if there are two functions in front of this index that return null, we need to add 1 to the index.
                    SPDLOG_INFO("Found potential GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                    SPDLOG_INFO("Double checking GetRenderTargetManager index...");

                    int32_t count = 0;
                    for (auto i = render_target_manager_vtable_index + 1; i < render_target_manager_vtable_index + 5; ++i) {
                        const auto addr_of_func = (uintptr_t)&((uintptr_t*)vtable)[i];
                        const auto func = ((uintptr_t*)vtable)[i];

                        if (func == 0 || IsBadReadPtr((void*)func, 1)) {
                            break;
                        }

                        // Make sure we didn't cross over into another vtable's boundaries.
                        const auto module_within = utility::get_module_within(addr_of_func);

                        if (module_within && utility::scan_displacement_reference(*module_within, addr_of_func)) {
                            SPDLOG_INFO("Crossed over into another vtable's boundaries, aborting double check");
                            SPDLOG_INFO("Reached end of double check at index {}, {} appears to be the correct index.", i, render_target_manager_vtable_index);
                            break;
                        }

                        if (!sdk::is_vfunc_pattern(func, "33 C0") && !sdk::is_vfunc_pattern(func, "31 C0")) {
                            SPDLOG_INFO("Reached end of double check at index {}, {} appears to be the correct index.", i, render_target_manager_vtable_index);
                            break;
                        }

                        if (++count >= 2) {
                            ++render_target_manager_vtable_index;
                            get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

                            SPDLOG_INFO("Adjusted GetRenderTargetManager index to {}", render_target_manager_vtable_index);
                            break;
                        }
                    }

                    SPDLOG_INFO("Distance: {}", distance_from_rendertexture_fn);
                }

                break;
            } else {
                try {
                    using GetRenderTargetManagerFn = IStereoRenderTargetManager* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
                    const auto func = (GetRenderTargetManagerFn)(*get_render_target_manager_func_ptr);
    
                    // On UE5.5+ FFakeStereoRendering has a valid GetRenderTargetManager that doesn't return null.
                    if (!is_4_18_or_lower && func(og_vtable.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == (IStereoRenderTargetManager*)&og_vtable[sizeof(void*)]) {
                        m_uses_old_rendertarget_manager = false; // nope
                        SPDLOG_INFO("Found UE5.5+ variant of GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                        SPDLOG_INFO("GetRenderTargetManager function at index {} appears to be valid.", render_target_manager_vtable_index);
                        break;
                    }
                } catch(...) {
                    SPDLOG_WARN("Unknown exception while checking GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                }
            }
        }
    //} else {
        //SPDLOG_INFO("GetRenderTargetManager function at index {} appears to be valid.", render_target_manager_vtable_index);
    //}
    
    const auto get_stereo_layers_func_ptr = (uintptr_t)(get_render_target_manager_func_ptr + sizeof(void*));

    if (get_render_target_manager_func_ptr == 0) {
        SPDLOG_ERROR("Failed to find GetRenderTargetManager");
        return false;
    }

    if (get_stereo_layers_func_ptr == 0) {
        SPDLOG_ERROR("Failed to find GetStereoLayers");
        return false;
    }

    SPDLOG_INFO("GetRenderTargetManagerptr: {:x}", (uintptr_t)get_render_target_manager_func_ptr);
    SPDLOG_INFO("GetStereoLayersptr: {:x}", (uintptr_t)get_stereo_layers_func_ptr);

    const auto adjust_view_rect_distance = is_4_18_or_lower ? 2 : 3;
    const auto adjust_view_rect_index = *stereo_view_offset_index - adjust_view_rect_distance;

    SPDLOG_INFO("AdjustViewRect Index: {}", adjust_view_rect_index);
    
    auto calculate_stereo_projection_matrix_index = *stereo_view_offset_index + 1;

    // While generally most of the time the stereo projection matrix func is the next one after the stereo view offset func,
    // it's not always the case. We can scan for a call to the tanf function in one of the virtual functions to find it.
    for (auto i = 0; i < 10; ++i) {
        const auto potential_func = ((uintptr_t*)vtable)[calculate_stereo_projection_matrix_index + i];
        if (potential_func == 0 || IsBadReadPtr((void*)potential_func, 1) || utility::is_stub_code((uint8_t*)potential_func)) {
            continue;
        }

        auto ip = (uint8_t*)potential_func;
        if (*(uint8_t*)ip == 0xE9) {
            ip = (uint8_t*)utility::calculate_absolute(potential_func + 1);
            SPDLOG_INFO("Found JMP at {:x}, jumping to {:x}", (uintptr_t)potential_func, (uintptr_t)ip);
        }

        bool found = false;

        SPDLOG_INFO("Scanning {:x}...", (uintptr_t)ip);

        for (auto j = 0; j < 50; ++j) {
            INSTRUX ix{};

            const auto status = NdDecodeEx(&ix, (ND_UINT8*)ip, 1000, ND_CODE_64, ND_DATA_64);

            if (!ND_SUCCESS(status)) {
                SPDLOG_INFO("Decoding failed with error {:x}!", (uint32_t)status);
                break;
            }

            if (ix.Category == ND_CAT_RET || ix.InstructionBytes[0] == 0xE9) {
                SPDLOG_INFO("Encountered RET or JMP at {:x}, aborting scan", (uintptr_t)ip);
                break;
            }

            if (ix.InstructionBytes[0] == 0xE8) {
                auto called_func = (uintptr_t)(ip + ix.Length + (int32_t)ix.RelativeOffset);
                auto inner_ins = utility::decode_one((uint8_t*)called_func);

                SPDLOG_INFO("called {:x}", (uintptr_t)called_func);
                uintptr_t final_func = 0;

                // Fully resolve the pointer jmps until we reach another module.
                while (inner_ins && inner_ins->InstructionBytes[0] == 0xFF && inner_ins->InstructionBytes[1] == 0x25) {
                    const auto called_func_ptr = (uintptr_t*)(called_func + inner_ins->Length + (int32_t)inner_ins->Displacement);
                    const auto called_func_ptr_val = *called_func_ptr;

                    SPDLOG_INFO("called ptr {:x}", (uintptr_t)called_func_ptr_val);

                    inner_ins = utility::decode_one((uint8_t*)called_func_ptr_val);
                    final_func = called_func_ptr_val;
                    called_func = called_func_ptr_val;
                }

                // Check if this function is jmping into the "tanf" export in ucrtbase.dll
                if (final_func != 0) {
                    const auto module_within = utility::get_module_within(final_func);

                    if (module_within &&
                        (final_func == (uintptr_t)GetProcAddress(*module_within, "tanf") ||
                        final_func == (uintptr_t)GetProcAddress(*module_within, "tan"))) 
                    {
                        SPDLOG_INFO("Found CalculateStereoProjectionMatrix: {} {:x}", calculate_stereo_projection_matrix_index + i, potential_func);
                        calculate_stereo_projection_matrix_index += i;
                        found = true;
                        break;
                    } else {
                        SPDLOG_INFO("Function did not call tanf, skipping");
                    }
                } else {
                    SPDLOG_INFO("Failed to resolve inner pointer");
                }
            }

            ip += ix.Length;
        }

        if (found) {
            break;
        }
    }

    const auto init_canvas_index = calculate_stereo_projection_matrix_index + 1;

    const auto adjust_view_rect_func = ((uintptr_t*)vtable)[adjust_view_rect_index];
    const auto calculate_stereo_projection_matrix_func = ((uintptr_t*)vtable)[calculate_stereo_projection_matrix_index];
    const auto init_canvas_func_ptr = &((uintptr_t*)vtable)[init_canvas_index];
    // const auto render_texture_render_thread_func = ((uintptr_t*)*vtable)[*stereo_view_offset_index + 3];
    

    SPDLOG_INFO("AdjustViewRect: {:x}", (uintptr_t)adjust_view_rect_func);
    SPDLOG_INFO("CalculateStereoProjectionMatrix: {:x}", (uintptr_t)calculate_stereo_projection_matrix_func);
    SPDLOG_INFO("CalculateStereoViewOffset: {:x}", (uintptr_t)stereo_view_offset_func);
    SPDLOG_INFO("IsStereoEnabled: {:x}", (uintptr_t)*is_stereo_enabled_func_ptr);

    m_has_double_precision = is_using_double_precision(stereo_view_offset_func) || is_using_double_precision(calculate_stereo_projection_matrix_func);

    {
        m_adjust_view_rect_hook = safetyhook::create_inline((void*)adjust_view_rect_func, adjust_view_rect);
        m_calculate_stereo_view_offset_hook_inline = safetyhook::create_inline((void*)stereo_view_offset_func, calculate_stereo_view_offset);
        m_calculate_stereo_projection_matrix_hook = safetyhook::create_inline((void*)calculate_stereo_projection_matrix_func, calculate_stereo_projection_matrix);
    }
    
    if (!m_adjust_view_rect_hook) {
        SPDLOG_ERROR("Failed to create AdjustViewRect hook");
    }

    if (!m_calculate_stereo_view_offset_hook_inline) {
        SPDLOG_ERROR("Failed to create CalculateStereoViewOffset hook, falling back to pointer hook");
        m_calculate_stereo_view_offset_hook_ptr = std::make_unique<PointerHook>((void**)&stereo_view_offset_func, (void*)calculate_stereo_view_offset);
    }

    if (!m_calculate_stereo_projection_matrix_hook) {
        SPDLOG_ERROR("Failed to create CalculateStereoProjectionMatrix hook");
    }

    // This requires a pointer hook because the virtual just returns false
    // compiler optimization makes that function get re-used in a lot of places
    // so it's not feasible to just detour it, we need to replace the pointer in the vtable.
    if (!m_rendertarget_manager_embedded_in_stereo_device) {
        m_render_texture_render_thread_hook = safetyhook::create_inline((void*)*render_texture_render_thread_func, render_texture_render_thread);

        if (!m_render_texture_render_thread_hook) {
            SPDLOG_ERROR("Failed to create RenderTexture_RenderThread hook");
        }

        // Seems to exist in 4.18+
        m_get_render_target_manager_hook = std::make_unique<PointerHook>((void**)get_render_target_manager_func_ptr, (void*)&get_render_target_manager_hook);
    } else {
        // When the render target manager is embedded in the stereo device, it just means
        // that all of the virtuals are now part of FFakeStereoRendering
        // instead of being a part of IStereoRenderTargetManager and being returned via GetRenderTargetManager.
        // Only seen in 4.17 and below.
        SPDLOG_INFO("Performing hooks on embedded RenderTargetManager");

        // Scan forward from the alleged RenderTexture_RenderThread function to find the
        // real RenderTexture_RenderThread function, because it is different when the
        // render target manager is embedded in the stereo device.
        // When it's embedded, it seems like it's the first function right after
        // a set of functions that return false sequentially.
        bool prev_function_returned_false = false;

        for (auto i = rendertexture_fn_vtable_index + 1; i < 100; ++i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find real RenderTexture_RenderThread");
                return false;
            }
            
            if (sdk::is_vfunc_pattern(func, "32 C0")) {
                prev_function_returned_false = true;
            } else {
                if (prev_function_returned_false) {
                    render_texture_render_thread_func = func;
                    rendertexture_fn_vtable_index = i;
                    m_render_texture_render_thread_hook = safetyhook::create_inline((void*)*render_texture_render_thread_func, render_texture_render_thread);
                    if (!m_render_texture_render_thread_hook) {
                        SPDLOG_ERROR("Failed to create RenderTexture_RenderThread hook");
                    }
                    SPDLOG_INFO("Real RenderTexture_RenderThread: {} {:x}", rendertexture_fn_vtable_index, (uintptr_t)*render_texture_render_thread_func);
                    break;
                }

                prev_function_returned_false = false;
            }
        }

        // Scan backwards from RenderTexture_RenderThread for the first virtual that just returns
        int32_t calculate_render_target_size_index = 0;

        for (auto i = rendertexture_fn_vtable_index - 1; i > 0; --i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find calculate render target size index, falling back to hardcoded index");
                calculate_render_target_size_index = rendertexture_fn_vtable_index - 3;
                break;
            }

            if (sdk::is_vfunc_pattern(func, "C3") || sdk::is_vfunc_pattern(func, "C2 00 00")) {
                SPDLOG_INFO("Dynamically found CalculateRenderTargetSize index: {}", i);
                calculate_render_target_size_index = i;
                break;
            }
        }

        const auto calculate_render_target_size_func_ptr = &((uintptr_t*)vtable)[calculate_render_target_size_index];
        SPDLOG_INFO("CalculateRenderTargetSize index: {}", calculate_render_target_size_index);

        // To be seen if this one needs automated analysis
        const auto need_reallocate_viewport_render_target_index = calculate_render_target_size_index + 1;
        const auto need_reallocate_viewport_render_target_func_ptr = &((uintptr_t*)vtable)[need_reallocate_viewport_render_target_index];

        // To be seen if this one needs automated analysis
        const auto should_use_separate_render_target_index = calculate_render_target_size_index + 2;
        const auto should_use_separate_render_target_func_ptr = &((uintptr_t*)vtable)[should_use_separate_render_target_index];

        // Log a warning if NeedReallocateViewportRenderTarget or ShouldUseSeparateRenderTarget are not
        // functions that plainly return false, but do not fail entirely.
        bool need_reallocate_viewport_render_target_is_bad = false;
        bool should_use_separate_render_target_is_bad = false;

        if (!sdk::is_vfunc_pattern(*need_reallocate_viewport_render_target_func_ptr, "32 C0")) {
            SPDLOG_WARN("NeedReallocateViewportRenderTarget is not a function that returns false");
            need_reallocate_viewport_render_target_is_bad = true;
        }

        if (!sdk::is_vfunc_pattern(*should_use_separate_render_target_func_ptr, "32 C0")) {
            SPDLOG_WARN("ShouldUseSeparateRenderTarget is not a function that returns false");
            should_use_separate_render_target_is_bad = true;
        }

        SPDLOG_INFO("NeedReallocateViewportRenderTarget index: {}", need_reallocate_viewport_render_target_index);
        SPDLOG_INFO("ShouldUseSeparateRenderTarget index: {}", should_use_separate_render_target_index);

        // Scan forward from RenderTexture_RenderThread for the first virtual that returns false
        int32_t allocate_render_target_index = 0;

        for (auto i = rendertexture_fn_vtable_index + 1; i < 100; ++i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find allocate render target index, falling back to hardcoded index");
                allocate_render_target_index = render_target_manager_vtable_index + 3;
                break;
            }

            if (sdk::is_vfunc_pattern(func, "32 C0")) {
                SPDLOG_INFO("Dynamically found AllocateRenderTarget index: {}", i);
                allocate_render_target_index = i;
                break;
            }
        }

        const auto allocate_render_target_func_ptr = &((uintptr_t*)vtable)[allocate_render_target_index];
        SPDLOG_INFO("AllocateRenderTarget index: {}", allocate_render_target_index);

        m_embedded_rtm.calculate_render_target_size_hook = 
            std::make_unique<PointerHook>((void**)calculate_render_target_size_func_ptr, +[](void* self, const sdk::FViewport& viewport, uint32_t& x, uint32_t& y) {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("CalculateRenderTargetSize (embedded)");
            #else
                SPDLOG_INFO_ONCE("CalculateRenderTargetSize (embedded)");
            #endif

                return g_hook->get_render_target_manager()->calculate_render_target_size(viewport, x, y);
            }
        );

        m_embedded_rtm.allocate_render_target_texture_hook = 
            std::make_unique<PointerHook>((void**)allocate_render_target_func_ptr, +[](void* self, 
                uint32_t index, uint32_t w, uint32_t h, uint8_t format, uint32_t num_mips,
                ETextureCreateFlags lags, ETextureCreateFlags targetable_texture_flags, FTexture2DRHIRef& out_texture,
                FTexture2DRHIRef& out_shader_resource, uint32_t num_samples) -> bool {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("AllocateRenderTargetTexture (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #else
                SPDLOG_INFO_ONCE("AllocateRenderTargetTexture (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #endif

                return g_hook->get_render_target_manager()->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &out_texture, &out_shader_resource);
            }
        );
    
        m_embedded_rtm.should_use_separate_render_target_hook = 
            std::make_unique<PointerHook>((void**)should_use_separate_render_target_func_ptr, +[](void* self) -> bool {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("ShouldUseSeparateRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #else
                SPDLOG_INFO_ONCE("ShouldUseSeparateRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #endif
            
                auto vr = VR::get();

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    return false;
                }

                if (vr->is_hmd_active() && !vr->is_stereo_emulation_enabled()) {
                    g_hook->get_embedded_rtm().should_use_separate_rt_called = true;
                    return true;
                }

                return false;
            }
        );

        if (!need_reallocate_viewport_render_target_is_bad) {
            m_embedded_rtm.need_reallocate_viewport_render_target_hook = 
                std::make_unique<PointerHook>((void**)need_reallocate_viewport_render_target_func_ptr, +[](void* self, sdk::FViewport* viewport) -> bool {
                #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                    SPDLOG_INFO("NeedReallocateViewportRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
                #else
                    SPDLOG_INFO_ONCE("NeedReallocateViewportRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
                #endif

                    if (g_hook->get_render_target_manager()->need_reallocate_view_target(*viewport)) {
                        g_hook->get_embedded_rtm().need_reallocate_viewport_render_target_called = true;
                        g_hook->get_embedded_rtm().last_time_needed_hmd_reallocate = std::chrono::steady_clock::now();
                        return true;
                    }

                    return false;
                }
            );
        }
    }
    
    m_is_stereo_enabled_hook = std::make_unique<PointerHook>((void**)is_stereo_enabled_func_ptr, (void*)&is_stereo_enabled);

    // scan for GetDesiredNumberOfViews function, we use this function to perform AFR if needed
    SPDLOG_INFO("Searching for GetDesiredNumberOfViews function...");
    std::optional<uint32_t> get_desired_number_of_views_index{};

    for (auto i = 1; i < 20; ++i) {
        auto func_ptr = &((uintptr_t*)vtable)[i];

        if (IsBadReadPtr((void*)*func_ptr, sizeof(void*))) {
            SPDLOG_INFO("Could not locate GetDesiredNumberOfViews function, this is okay, not really needed");
            break;
        }

        // pretty consistent patterns
        if (sdk::is_vfunc_pattern(*func_ptr, "0F B6 C2 FF C0 C3") ||
            sdk::is_vfunc_pattern(*func_ptr, "33 C0 84 D2 0F 95 C0 FF C0 C3") || 
            sdk::is_vfunc_pattern(*func_ptr, "84 D2 74 04 8B 41 ? C3 B8 01"))
        {
            SPDLOG_INFO("Found GetDesiredNumberOfViews function at index: {}", i);
            get_desired_number_of_views_index = i;
            m_get_desired_number_of_views_hook = std::make_unique<PointerHook>((void**)func_ptr, (void*)&get_desired_number_of_views_hook);
            break;
        }
    }

    // If double precision detected, it means it's >= UE 5.0.3
    if (m_has_double_precision && get_desired_number_of_views_index) {
        SPDLOG_INFO("Searching for GetViewPassForIndex function...");

        // Pretty simple, it's at +1, to be seen if this needs automation
        const auto get_view_pass_for_index_index = *get_desired_number_of_views_index + 1;

        auto func_ptr = &((uintptr_t*)vtable)[get_view_pass_for_index_index];

        if (IsBadReadPtr((void*)*func_ptr, sizeof(void*))) {
            SPDLOG_INFO("Could not locate GetViewPassForIndex function. A crash may occur.");
        } else {
            SPDLOG_INFO("Found GetViewPassForIndex function at index: {}", get_view_pass_for_index_index);
            m_get_view_pass_for_index_hook = std::make_unique<PointerHook>((void**)func_ptr, (void*)&get_view_pass_for_index_hook);
        }
    } else if (m_has_double_precision) {
        SPDLOG_INFO("Could not locate GetViewPassForIndex function because GetDesiredNumberOfViews function was not found. A crash may occur.");
    }

    SPDLOG_INFO("Leaving FFakeStereoRenderingHook::hook");

    const auto renderer_module = sdk::get_ue_module(L"Renderer");
    const auto backbuffer_format_cvar = sdk::find_cvar_by_description(L"Defines the default back buffer pixel format.", L"r.DefaultBackBufferPixelFormat", 4, renderer_module);
    m_pixel_format_cvar_found = backbuffer_format_cvar.has_value();

    // In 4.18 this doesn't exist. Not much we can do about that.
    if (backbuffer_format_cvar) {
        SPDLOG_INFO("Backbuffer Format CVar: {:x}", (uintptr_t)*backbuffer_format_cvar);
        *(int32_t*)(*(uintptr_t*)*backbuffer_format_cvar + 0) = 0;   // 8bit RGBA, which is what VR headsets support
        *(int32_t*)(*(uintptr_t*)*backbuffer_format_cvar + 0x4) = 0; // 8bit RGBA, which is what VR headsets support
    } else {
        SPDLOG_ERROR("Failed to find backbuffer format cvar, continuing anyways...");
    }

    // make a shadow copy of FFakeStereoRendering's vtable to get past weird compiler optimizations
    // that cause the hook to not work, reason being that the compiler will optimize
    // if the vtable pointer is equal to the original vtable pointer, and it will
    // not call the hook function, so we make a shadow copy of the vtable
    auto active_stereo_device = locate_active_stereo_rendering_device();
    
    // We need to manually insert a stereo device at this point if it's not already.
    // This is what the "nonstandard" hooks did, but those did not have access to FFakeStereoRendering's vtable.
    // All we need to do in this instance is get the engine offset to the stereo device, create a fake pointer with our own vtable,
    // and just overwrite the engine's (null) stereo device pointer with our fake one.
    // It is very rare that this should need to be done.
    if (!active_stereo_device) {
        SPDLOG_INFO("Attempting to create a stereo device without InitializeHMDDevice...");
        const auto device_offset = sdk::UEngine::get_stereo_rendering_device_offset();

        if (device_offset) {
            auto engine = sdk::UGameEngine::get();

            if (engine != nullptr) {
                m_fallback_device.vtable = (void*)vtable;
                *(uintptr_t*)((uintptr_t)engine + *device_offset) = (uintptr_t)&m_fallback_device;

                active_stereo_device = (uintptr_t)&m_fallback_device;
                s_stereo_rendering_device_offset = *device_offset; // Set it up if it's not already
            }
        } else {
            SPDLOG_ERROR("Could not create a new stereo device, VR may not work!");
        }
    }

    if (active_stereo_device) {
        SPDLOG_INFO("Found active stereo device: {:x}", (uintptr_t)*active_stereo_device);
        SPDLOG_INFO("Overwriting vtable...");

        static std::vector<uintptr_t> shadow_vtable{};
        auto& vtable = *(uintptr_t**)*active_stereo_device;

        for (auto i = 0; i < 100; i++) {
            shadow_vtable.push_back(vtable[i]);
        }

        vtable = shadow_vtable.data();
    } else {
        SPDLOG_INFO("Current stereo device is null, cannot overwrite vtable");
        patch_vtable_checks(); // fallback to patching vtable checks
    }

    setup_view_extensions();
    hook_game_viewport_client();

    m_finished_hooking = true;

    SPDLOG_INFO("Finished hooking FFakeStereoRendering!");

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook() {
    // This may only work on one game for now, but it should be a good placeholder
    // for creating a stereo device for games that don't have one.
    // We can figure out how to make it work for other games when we run into one
    // that needs this same functionality.

    // The reason why this function is needed is because in the one game that
    // the FFakeStereoRenderingHook doesn't work through the standard method,
    // is because the VR pipeline seems to have been heavily modified,
    // and so the -emulatestereo command line argument doesn't work, and
    // the FFakeStereoRendering vtable does not seem to exist
    // However the StereoRenderingDevice within GEngine seems to still exist
    // so we can take advantage of that and create our own stereo device
    // the downside is it will be much more difficult to figure out the 
    // proper vtable indices for the functions we need to hook
    // and we will need to actually implement some of the functions
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method");
    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    // Actually implement the ones we care about now.
    auto idx = 0;
    //m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect


    ++idx; // idk waht this is.

    // in this version the index is passed...?
    /*m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, uint32_t index, Vector2f* bounds) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetTextSafeRegionBounds called");
#endif

        bounds->x = 0.75f;
        bounds->y = 0.75f;

        return bounds;
    };*/ // GetTextSafeRegionBounds

    m_fallback_vtable[idx++] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    
    idx++;

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, void* a2) {
        // do nothing
    }; // not sure what this one is. think it sets the FOV. Not present in newer UE4 versions.

    idx++; // just leave this one as a placeholder for now. Returns false.

    m_fallback_vtable[idx++] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    idx++; // just leave this one as a placeholder for now. Probably SetClippingPlanes.

    m_fallback_vtable[13] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager
    //m_fallback_vtable[13] = +[](FFakeStereoRendering* stereo) { return nullptr; }; // GetRenderTargetManager

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    //m_418_detected = true;
    m_special_detected = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAC8; // fallback for the engine this was originally made for.
    }

    *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset) = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_27() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.27)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;
    constexpr auto GET_VIEW_INDEX_FOR_PASS_INDEX = 6;

    constexpr auto DEVICE_IS_STEREO_EYE_PASS_INDEX = 7;
    constexpr auto DEVICE_IS_STEREO_EYE_VIEW_INDEX = 8;
    constexpr auto DEVICE_IS_A_PRIMARY_PASS_INDEX = 9;
    constexpr auto DEVICE_IS_A_PRIMARY_VIEW_INDEX = 10;
    constexpr auto DEVICE_IS_A_SECONDARY_PASS_INDEX = 11;
    constexpr auto DEVICE_IS_A_SECONDARY_VIEW_INDEX = 12;
    constexpr auto DEVICE_IS_AN_ADDITIONAL_PASS_INDEX = 13; // not necessary...?
    constexpr auto DEVICE_IS_AN_ADDITIONAL_VIEW_INDEX = 14; // not necessary...?
    constexpr auto DEVICE_GET_LOD_VIEW_INDEX_INDEX = 15; // not necessary...?

    constexpr auto ADJUST_VIEW_RECT_INDEX = 16;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = 19;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = 20;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = 22;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = 23;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xB18; // fallback for the engine this was originally made for.
    }

    static constexpr auto FSCENEVIEW_STEREO_PASS_OFFSET = 0xAF0;
    static auto get_stereo_pass = [](const sdk::FSceneView& view) -> EStereoscopicPass {
        return (EStereoscopicPass)*(uint8_t*)((uintptr_t)&view + FSCENEVIEW_STEREO_PASS_OFFSET);
    };

    // Actually implement the ones we care about now.
    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t { 
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled); 
    }; // GetDesiredNumberOfViews

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    }; // GetViewPassForIndex

    m_fallback_vtable[GET_VIEW_INDEX_FOR_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> int32_t {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewIndexForPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        switch (pass) {
            case EStereoscopicPass::eSSP_FULL:
            case EStereoscopicPass::eSSP_PRIMARY:
                return 0;
            
            case EStereoscopicPass::eSSP_SECONDARY:
                return 1;
            
            default:
                SPDLOG_ERROR("Unknown pass: {}", (uint32_t)pass);
                return -1;
        };
    };

    m_fallback_vtable[DEVICE_IS_STEREO_EYE_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsStereoEyePass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass != EStereoscopicPass::eSSP_FULL;
    }; // DeviceIsStereoEyePass

    m_fallback_vtable[DEVICE_IS_STEREO_EYE_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsStereoEyeView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) != EStereoscopicPass::eSSP_FULL;
    }; // DeviceIsStereoEyePass

    m_fallback_vtable[DEVICE_IS_A_PRIMARY_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsAPrimaryPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass == EStereoscopicPass::eSSP_FULL || pass == EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsAPrimaryPass

    m_fallback_vtable[DEVICE_IS_A_PRIMARY_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsAPrimaryView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) == EStereoscopicPass::eSSP_FULL || get_stereo_pass(view) == EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsAPrimaryPass

    m_fallback_vtable[DEVICE_IS_A_SECONDARY_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsASecondaryPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return !(pass == EStereoscopicPass::eSSP_FULL || pass == EStereoscopicPass::eSSP_PRIMARY);
    }; // DeviceIsASecondaryPass

    m_fallback_vtable[DEVICE_IS_A_SECONDARY_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsASecondaryView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) > EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsASecondaryView

    m_special_detected_4_27 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_22() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.22)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;
    constexpr auto GET_VIEW_INDEX_FOR_PASS_INDEX = 6;
    constexpr auto IS_STEREO_EYE_PASS_INDEX = 7;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 8;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = ADJUST_VIEW_RECT_INDEX + 3;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = CALCULATE_STEREO_VIEW_OFFSET_INDEX + 1;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = CALCULATE_STEREO_PROJECTION_MATRIX_INDEX + 2;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = RENDER_TEXTURE_RENDER_THREAD_INDEX + 1;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAB8; // fallback for the engine this was originally made for.
    }

    // Actually implement the ones we care about now.
    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t { 
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled); 
    }; // GetDesiredNumberOfViews

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    }; // GetViewPassForIndex

    m_fallback_vtable[GET_VIEW_INDEX_FOR_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> int32_t {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewIndexForPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        switch (pass) {
            case EStereoscopicPass::eSSP_FULL:
            case EStereoscopicPass::eSSP_PRIMARY:
                return 0;
            
            case EStereoscopicPass::eSSP_SECONDARY:
                return 1;
            
            default:
                SPDLOG_ERROR("Unknown pass: {}", (uint32_t)pass);
                return -1;
        };
    };

    m_fallback_vtable[IS_STEREO_EYE_PASS_INDEX ] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass != EStereoscopicPass::eSSP_FULL;
    };

    m_special_detected_4_22 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_18() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.18)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto IS_STEREO_ENABLED_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 1;
    constexpr auto ENABLE_STEREO_INDEX = 2;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 3;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = ADJUST_VIEW_RECT_INDEX + 2;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = CALCULATE_STEREO_VIEW_OFFSET_INDEX + 1;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = CALCULATE_STEREO_PROJECTION_MATRIX_INDEX + 3;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = RENDER_TEXTURE_RENDER_THREAD_INDEX + 3;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAE8; // fallback for the engine this was originally made for.
    }

    // Actually implement the ones we care about now.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_special_detected_4_18 = true;
    m_uses_old_rendertarget_manager = true; // this engine has a funny render target manager.
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::hook_game_viewport_client() try {
    SPDLOG_INFO("Attempting to hook UGameViewportClient::Draw...");

    // We need to cache the canvas index before we hook the draw function or else this doesn't work.
    sdk::FViewport::get_debug_canvas_index();
    auto game_viewport_client_draw = sdk::UGameViewportClient::get_draw_function();

    if (!game_viewport_client_draw) {
        SPDLOG_ERROR("Failed to find UGameViewportClient::Draw!");
        m_has_game_viewport_client_draw_hook = false;
        return false;
    }

    m_gameviewportclient_draw_hook = safetyhook::create_inline((void*)*game_viewport_client_draw, &game_viewport_client_draw_hook, safetyhook::InlineHook::StartDisabled);
    m_has_game_viewport_client_draw_hook = true;

    if (!m_gameviewportclient_draw_hook) {
        SPDLOG_ERROR("Failed to hook UGameViewportClient::Draw!");
        return false;
    }

    if (auto enable_result = m_gameviewportclient_draw_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable UGameViewportClient::Draw hook!");
        return false;
    }

    return true;
} catch(std::exception& e) {
    SPDLOG_ERROR("Failed to hook UGameViewportClient: {}", e.what());
    return false;
} catch(...) {
    SPDLOG_ERROR("Failed to hook UGameViewportClient!");
    return false;
}

void* FFakeStereoRenderingHook::viewport_destructor_hook(void* viewport, void* a2, void* a3, void* a4) {
    ZoneScopedN(__FUNCTION__);

    SPDLOG_INFO("FViewport::~FViewport called: {:x}", (uintptr_t)_ReturnAddress());

    // Call the original destructor.
    auto call_orig = [&]() -> void* {
        ZoneScopedN("FViewport::~FViewport");
        auto res = g_hook->m_viewport_destructor_hook->get_original<decltype(&viewport_destructor_hook)>()(viewport, a2, a3, a4);
        g_hook->m_last_destroyed_viewport = viewport;

        return res;
    };

    if (!g_framework->is_game_data_intialized()) {
        return call_orig();
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        return call_orig();
    }

    static bool once = true;

    if (once) {
        SPDLOG_INFO("FViewport::Destructor called for the first time.");
        once = false;
    }

    return call_orig();
}

void FFakeStereoRenderingHook::viewport_draw_hook(void* viewport, bool should_present) {
    ZoneScopedN(__FUNCTION__);

    g_hook->m_last_viewport_vtable = *(void***)viewport;

    auto call_orig = [&]() {
        ZoneScopedN("FViewport::Draw");
        g_hook->m_viewport_draw_hook.call(viewport, should_present);
    };

    if (!g_framework->is_game_data_intialized()) {
        call_orig();
        return;
    }

    if (g_hook->m_viewport_destructor_hook == nullptr) {
        static bool already_tried = false;

        if (!already_tried) {
            already_tried = true;
            auto& vtable = *(void***)viewport;

            if (vtable != nullptr && vtable[0] != nullptr) {
                // Destructors usually have some kind of test reg8, 01 instruction within them.
                if (utility::find_pattern_in_path((uint8_t*)vtable[0], 0x100, false, "F6 ? 01")) {
                    SPDLOG_INFO("Found TEST mnemonic for FViewport destructor at {:x}", (uintptr_t)vtable[0]);
                    SPDLOG_INFO("Hooking FViewport::~FViewport at {:x}", (uintptr_t)vtable[0]);
                    g_hook->m_viewport_destructor_hook = std::make_unique<PointerHook>(&vtable[0], &viewport_destructor_hook);
                } else {
                    SPDLOG_ERROR("Failed to find FViewport destructor pattern at {:x}", (uintptr_t)vtable[0]);
                }
            }
        }
    }

    if (g_hook->m_ignore_next_viewport_draw) {
        g_hook->m_ignore_next_viewport_draw = false;
        return;
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        call_orig();
        return;
    }

    static bool once = true;

    if (once) {
        SPDLOG_INFO("FViewport::Draw called for the first time.");
        once = false;
    }

    call_orig();
}

// This function needs some more work for more rigorous filtering
// However it does its job on the relevant titles
// This is only used for the UI compatibility mode.
FRHITexture2D** FFakeStereoRenderingHook::viewport_get_render_target_texture_hook(sdk::FViewport* viewport) {
    const auto retaddr = (uintptr_t)_ReturnAddress();

    SPDLOG_INFO_ONCE("FViewport::GetRenderTargetTexture called!");
    const auto og = g_hook->m_viewport_get_render_target_texture_hook->get_original<decltype(&viewport_get_render_target_texture_hook)>();
    const auto& vr = VR::get();

    if (!vr->is_ahud_compatibility_enabled() || !vr->is_hmd_active() || g_hook->m_slate_draw_window_thread_id == 0) {
        return og(viewport);
    }

    auto& data = g_hook->m_viewport_rt_hook_data;

    {
        std::scoped_lock _{data.retaddr_mutex};
        utility::ScopeGuard guard{[&](){ data.seen_retaddrs.insert(retaddr); }};

        if (data.call_original_retaddrs.contains(retaddr)) {
            return og(viewport);
        }

        std::optional<size_t> func_start{};

        // ALWAYS check the retaddr for ViewFamilyTexture first and never skip it
        // This will fix the case where we run into some other texture initially.
        if (!data.seen_retaddrs.contains(retaddr)) {
            SPDLOG_INFO("FViewport::GetRenderTargetTexture called from {:x}", retaddr);

            func_start = utility::find_function_start(retaddr);

            if (!func_start) {
                func_start = retaddr;
            }

            // The function that has this string reference should ALWAYS get passed
            // back to the original function, this is the actual scene render target.
            // Everything else we will redirect to the UI render target.
            if (utility::find_string_reference_in_path(*func_start, L"ViewFamilyTexture", false) || utility::find_string_reference_in_path(*func_start, L"ViewFamilyTarget", false)) {
                SPDLOG_INFO("Found view family texture reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                data.has_view_family_tex = true;
                return og(viewport);
            }

            // We should always allow the viewport when used in a post processing context to go through.
            // There's two because this function stops itself at 200 instructions
            // doing a second one from the retaddr allows us to go further.
            if (utility::find_string_reference_in_path(*func_start, L"FinalPostProcessColor", false) || utility::find_string_reference_in_path(retaddr, L"FinalPostProcessColor", false)) {
                SPDLOG_INFO("Found FinalPostProcessColor reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }

            const auto next_fn_call = utility::scan_disasm(retaddr, 0x30, "E8 ? ? ? ?");

            if (next_fn_call) {
                const auto fn = utility::calculate_absolute(*next_fn_call + 1);

                // I don't know of any other way to check this. I'm not sure what this function is.
                // It seems like deep within a threaded or function for enqueueing a render command.
                if (utility::scan(fn, 0x50, "01 01 01 01") && utility::scan(fn, 0x50, "22 00 00 00")) {
                    SPDLOG_INFO("Found unknown screen space rendering call @ {:x}", retaddr);
                    data.redirected_retaddrs.insert(retaddr);
                }
            }

            // There are multiple other HAL references we can use too.
            static const auto hal_clear_solid_rectangle_fn = utility::find_function_from_string_ref(utility::get_executable(), "HAL::ClearSolidRectangle");
            static std::unordered_set<uintptr_t> scaleform_hal_vtable_functions{};

            const auto is_scaleform = hal_clear_solid_rectangle_fn.has_value();

            if (hal_clear_solid_rectangle_fn.has_value() && scaleform_hal_vtable_functions.empty()) try {
                scaleform_hal_vtable_functions.insert(*hal_clear_solid_rectangle_fn);

                SPDLOG_INFO("Found HAL::ClearSolidRectangle function @ {:x}", *hal_clear_solid_rectangle_fn);
                std::vector<uintptr_t> scaleform_hal_vtable_refs{};
                const auto module_size = utility::get_module_size(utility::get_executable()).value_or(0);
                const auto start = (uintptr_t)utility::get_executable();
                const auto end = (uintptr_t)utility::get_executable() + module_size;
                const auto hal_module = utility::get_module_within(*hal_clear_solid_rectangle_fn).value_or(nullptr);

                // There are multiple HAL vtable, so just collect all of them.
                for (auto i = start; i < end - 0x1000; i += sizeof(uintptr_t)) {
                    const auto remaining = end - i;
                    const auto function_ptr = utility::scan_ptr(i, remaining - 0x1000, *hal_clear_solid_rectangle_fn);

                    if (!function_ptr.has_value()) {
                        break;
                    }

                    i = *function_ptr;

                    SPDLOG_INFO("Found HAL::ClearSolidRectangle function pointer @ {:x}", *function_ptr);
                    for (auto j = 0; j < 100; ++j) {
                        const auto entry = *(uintptr_t*)(*function_ptr + (j * sizeof(uintptr_t)));

                        if (entry == 0 || IsBadReadPtr((void*)entry, sizeof(uintptr_t))) {
                            break;
                        }

                        const auto is_same_module = utility::get_module_within(entry).value_or(nullptr) == hal_module;

                        if (!is_same_module) {
                            break;
                        }

                        scaleform_hal_vtable_functions.insert(entry);
                    }
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to find Scaleform HAL vtable functions!");
            }

            if (is_scaleform && !scaleform_hal_vtable_functions.empty()) try {
                // Walk the stack, get function starts and check if any are in the vtable
                constexpr auto max_stack_depth = 100;
                uintptr_t stack[max_stack_depth]{};

                const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

                for (auto i = 0; i < depth; ++i) {
                    SPDLOG_INFO(" Stack[{}]: {:x}", i, stack[i]);
                }

                bool found = false;

                for (auto i = 1; i < std::min<uint16_t>(7, depth); ++i) {
                    const auto scaleform_func_start = utility::find_virtual_function_start(stack[i]);

                    if (!scaleform_func_start) {
                        continue;
                    }

                    if (scaleform_hal_vtable_functions.contains(*scaleform_func_start)) {
                        SPDLOG_INFO("Found Scaleform HAL vtable function reference @ {:x}", retaddr);
                        data.redirected_retaddrs.insert(retaddr);
                        found = true;
                        break;
                    }
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to walk stack for scaleform vtable functions!");
            }
        }

        // Hacky way to allow the first texture to go through
        // For the games that are using something other than ViewFamilyTexture as the scene RT.
        if (!data.call_original_retaddrs.empty() && !data.redirected_retaddrs.contains(retaddr) && !data.has_view_family_tex) {
            return og(viewport);
        }

        if (!data.redirected_retaddrs.contains(retaddr) && !data.call_original_retaddrs.contains(retaddr)) {
            if (!func_start) {
                func_start = utility::find_function_start(retaddr);

                if (!func_start) {
                    func_start = retaddr;
                }
            }

            // Probably NOT...
            /*if (utility::find_string_reference_in_path(*func_start, L"r.RHICmdAsyncRHIThreadDispatch")) {
                SPDLOG_INFO("Found RHICmdAsyncRHIThreadDispatch reference @ {:x}", retaddr);
                call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }*/

            // TODO? this needs some more rigorous filtering
            // some games are insane and have multiple "UnknownTexture" references...
            if (utility::find_string_reference_in_path(*func_start, L"UnknownTexture", false)) {
                SPDLOG_INFO("Found unknown texture reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }

            SPDLOG_INFO("Redirecting FViewport::GetRenderTargetTexture call to UI render target @ {:x}", retaddr);
            data.redirected_retaddrs.insert(retaddr);
        }
    }

    // Finally redirect the call to the UI render target.
    const auto rtm = g_hook->get_render_target_manager();

    if (rtm == nullptr) {
        SPDLOG_WARN("[viewport_get_render_target_texture_hook] render_target_manager is nullptr, falling back to original call");
        return og(viewport);
    }

    auto& ui_target = rtm->get_ui_target();

    // ui_target can be non-null but dangling (pointing at a freed RHI texture) if a texture
    // recreation (e.g. tall-UI toggle, resolution change during a menu transition) happened
    // between when the render target manager last assigned it and now. Handing a dangling
    // pointer straight to the engine (which immediately dereferences it as the viewport's
    // render target texture) causes a null/invalid pointer crash inside engine code. Guard
    // with IsBadReadPtr, matching the safety pattern used elsewhere in this file (e.g. around
    // g_lgui_swap.ui_target usage) before returning it.
    if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60)) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[DIAG] viewport_get_render_target_texture_hook: redirecting retaddr={:x} to ui_target={:x}",
            retaddr, (uintptr_t)ui_target);
        return &ui_target;
    }

    if (ui_target != nullptr) {
        SPDLOG_WARN("[viewport_get_render_target_texture_hook] ui_target={:x} failed IsBadReadPtr validation (dangling/freed) - falling back to original for retaddr={:x}",
            (uintptr_t)ui_target, retaddr);
    } else {
        SPDLOG_INFO_EVERY_N_SEC(2, "[DIAG] viewport_get_render_target_texture_hook: ui_target is null, falling back to original for retaddr={:x}", retaddr);
    }

    return og(viewport);
}

// OPTION B (UI canvas fit): LGUI lays its UI canvas out in pixel space at the game-thread FViewport::GetSizeXY size
// (per-eye hmd_w x hmd_h with NSF, e.g. 2699x3193) and paints it top-left into the fixed 3840x2160 ui_target. When the
// per-eye height (3193) exceeds the target height (2160), the bottom of the UI is clipped - this is the NSF-ON bottom
// cutoff. Proven by [LGUI_D3D] (viewport always full-target) + [LGUI_BOUNDS] (paint height tracks hmd_h, not ui_target).
// Fix: for the DURATION of the UI draw only, overwrite the FViewport SizeX/SizeY that LGUI reads with a size that fits
// inside the ui_target (same aspect, scaled so height <= ui_h), then RESTORE the real values immediately after so the
// 3D scene renderer - which reads the same FViewport later - is never affected. Scoped, synchronous, and toggle-gated.
static constexpr bool LGUI_FIT_UI_CANVAS = true;
// FViewport::SizeX/SizeY byte offset, resolved once by the size scan below and reused at the patch site.
static std::optional<uint32_t> g_fviewport_size_off{};

void FFakeStereoRenderingHook::game_viewport_client_draw_hook(sdk::UGameViewportClient* viewport_client, sdk::FViewport* viewport, sdk::FCanvas* canvas, void* a4) {
    ZoneScopedN(__FUNCTION__);

    // UI compatibility mode
    // Tries to redirect calls to GetRenderTargetTexture to point towards our UI
    // texture instead of the scene render target, if it's not the scene itself/the view family texture.
    // This usually isn't needed but sometimes there are bespoke changes to the rendering pipeline
    // or uses of the AHUD class that make it necessary.
    if (g_framework->is_game_data_intialized() && VR::get()->is_ahud_compatibility_enabled() && viewport != nullptr) {
        if (g_hook->m_viewport_get_render_target_texture_hook == nullptr) {
            SPDLOG_INFO("Hooking FViewport::GetRenderTargetTexture...");
            void** vp_vtable = *(void***)viewport;
            g_hook->m_viewport_get_render_target_texture_hook = std::make_unique<PointerHook>(&vp_vtable[1], &viewport_get_render_target_texture_hook);
            SPDLOG_INFO("Hooked FViewport::GetRenderTargetTexture!");
        }
    }

    auto call_orig = [=]() {
        ZoneScopedN("UGameViewportClient::Draw");
        g_hook->m_gameviewportclient_draw_hook.call(viewport_client, viewport, canvas, a4);
    };

    SPDLOG_INFO_ONCE("UGameViewportClient::Draw called for the first time.");

    if (!g_framework->is_game_data_intialized()) {
        call_orig();
        return;
    }

    g_hook->m_in_viewport_client_draw = true;
    g_hook->m_was_in_viewport_client_draw = false;
    g_hook->get_render_target_manager()->set_viewport(viewport);

    // Sample the game-thread viewport size LGUI uses for its canvas layout (see lgui_slot24_hook).
    // The SDK's GetViewportSizeXY vtable scan fails on this engine build, so fall back to locating FViewport::SizeX/SizeY
    // in the object: the int pair equal to the window client size (2D mode) pins the offset, which is then reused in VR.
    if (viewport != nullptr && !IsBadReadPtr(viewport, 0x200)) {
        static std::optional<uint32_t> size_off{};
        static bool logged_candidates = false;
        auto vr = VR::get();

        SPDLOG_INFO_ONCE("[LGUI_VP] viewport sampling active (viewport={:x} sdk_index={})", (uintptr_t)viewport, sdk::FViewport::get_viewport_size_xy_index().has_value());

        sdk::FViewport::IntPoint sz{};
        if (sdk::FViewport::get_viewport_size_xy_index().has_value()) {
            sz = viewport->get_viewport_size_xy();
        }

        if (sz.x <= 0 || sz.y <= 0) {
            RECT rc{};
            const auto have_rc = g_framework->get_window() != nullptr && GetClientRect(g_framework->get_window(), &rc) != FALSE;
            const int32_t win_w = have_rc ? rc.right - rc.left : 0;
            const int32_t win_h = have_rc ? rc.bottom - rc.top : 0;

            // NOTE: this scan used to be gated behind flat_view (2D/non-HMD only), so in real VR sessions (HMD
            // active, not 2D screen) it never ran and size_off was never resolved - meaning the whole
            // [LGUI_VP] game viewport size log below never fired during VR testing. Run the scan regardless of
            // mode; win_w/win_h (desktop mirror window) and alt_w/alt_h (hmd size) are both valid candidates to
            // match against in VR too.
            if (!size_off.has_value()) {
                const int32_t alt_w = (int32_t)vr->get_hmd_width();
                const int32_t alt_h = (int32_t)vr->get_hmd_height();
                std::string cands{};
                for (uint32_t off = 0x8; off + 8 <= 0x200; off += 4) {
                    const auto x = *(int32_t*)((uintptr_t)viewport + off);
                    const auto y = *(int32_t*)((uintptr_t)viewport + off + 4);
                    if (x >= 64 && x <= 16384 && y >= 64 && y <= 16384) {
                        cands += fmt::format("{:x}=({},{}) ", off, x, y);
                        if (!size_off.has_value() && ((win_w > 0 && x == win_w && y == win_h) || (alt_w > 0 && x == alt_w && y == alt_h))) {
                            size_off = off;
                        }
                    }
                }
                if (!logged_candidates || size_off.has_value()) {
                    logged_candidates = true;
                    SPDLOG_INFO("[LGUI_VP] FViewport size scan: window client={}x{} hmd2d={}x{} 2d_screen={} hmd_active={} candidates: {} -> SizeXY offset {}", win_w, win_h, alt_w, alt_h,
                        vr->is_using_2d_screen(), vr->is_hmd_active(), cands.empty() ? "<none>" : cands, size_off.has_value() ? fmt::format("{:x}", *size_off) : "not found");
                }
            }

            if (size_off.has_value()) {
                sz.x = *(int32_t*)((uintptr_t)viewport + *size_off);
                sz.y = *(int32_t*)((uintptr_t)viewport + *size_off + 4);
                g_fviewport_size_off = size_off; // share with the Option B fit patch at the call_orig() site
            }
        }

        // Option B needs the BYTE offset of FViewport::SizeX/SizeY inside the object so it can patch it around the UI draw.
        // The fallback scan above only runs when the SDK's get_viewport_size_xy() fails; when the SDK path succeeds we have
        // the size value (sz) but not the offset. Resolve the offset independently by scanning for the int pair that equals
        // the known-good sz, so g_fviewport_size_off is populated regardless of which path produced sz.
        if (!g_fviewport_size_off.has_value() && sz.x > 0 && sz.y > 0) {
            for (uint32_t off = 0x8; off + 8 <= 0x200; off += 4) {
                const auto x = *(int32_t*)((uintptr_t)viewport + off);
                const auto y = *(int32_t*)((uintptr_t)viewport + off + 4);
                if (x == sz.x && y == sz.y) {
                    g_fviewport_size_off = off;
                    SPDLOG_INFO("[LGUI_VP] resolved FViewport SizeXY offset {:x} from sz={}x{} (sdk path)", off, sz.x, sz.y);
                    break;
                }
            }
        }

        const auto prev = g_hook->get_game_viewport_size();
        if (sz.x > 0 && sz.y > 0 && (sz.x != prev.width || sz.y != prev.height)) {
            g_hook->set_game_viewport_size(sz.x, sz.y);
            SPDLOG_INFO("[LGUI_VP] game viewport size {}x{} (hmd_active={} nsf={} hmd={}x{})", sz.x, sz.y,
                vr->is_hmd_active(), vr->is_native_stereo_fix_enabled(), vr->get_hmd_width(), vr->get_hmd_height());
        }
    }

    utility::ScopeGuard _{ 
        []() { 
            g_hook->m_in_viewport_client_draw = false;
            g_hook->m_was_in_viewport_client_draw = false;
        } 
    };

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        call_orig();
        return;
    }

    static uint32_t hook_attempts = 0;
    static bool run_anyways = false;

    if (hook_attempts < 100 && !g_hook->m_hooked_game_engine_tick && g_hook->m_attempted_hook_game_engine_tick) {
        ZoneScopedN("UGameViewportClient::Draw (hook UGameEngine::Tick)");
        SPDLOG_INFO("Performing alternative UGameEngine::Tick hook for synced AFR.");

        ++hook_attempts;

        // Go up the stack and find the viewport draw function.
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

        for (auto i = 0; i < depth; ++i) {
            SPDLOG_INFO("Stack[{}]: {:x}", i, stack[i]);
        }

        for (auto i = 3; i < depth; ++i) {
            const auto ret = stack[i];

            g_hook->attempt_hook_game_engine_tick(ret);

            if (g_hook->m_hooked_game_engine_tick) {
                SPDLOG_INFO("Successfully hooked UGameEngine::Tick for synced AFR.");
                break;
            }
        }
    } else {
        run_anyways = !g_hook->m_hooked_game_engine_tick;
    }

    const auto in_engine_tick = g_hook->m_in_engine_tick;

    if (run_anyways || in_engine_tick) {
        if (g_hook->m_has_view_extension_hook) {
            g_frame_count = vr->get_runtime()->internal_frame_count;
            vr->update_hmd_state(true, vr->get_runtime()->internal_frame_count + 1);
        } else {
            vr->update_hmd_state(false);
        }
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (const auto& mod : mods) {
        mod->on_pre_viewport_client_draw(viewport_client, viewport, canvas);
    }

    // OPTION B: temporarily shrink the FViewport SizeX/SizeY that LGUI reads for its canvas layout so the UI fits inside
    // the ui_target (no bottom clip), then restore before the scene renderer reads the same FViewport. Same-aspect scale
    // so height <= ui_h; only touches the two ints at the resolved offset, only while HMD is active, fully restored below.
    bool fit_patched = false;
    int32_t fit_saved_x = 0, fit_saved_y = 0;
    if (LGUI_FIT_UI_CANVAS && vr->is_hmd_active() && g_fviewport_size_off.has_value() &&
        viewport != nullptr && !IsBadReadPtr(viewport, 0x200) &&
        g_hook->get_render_target_manager() != nullptr) {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
        if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60)) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
            auto px = (int32_t*)((uintptr_t)viewport + *g_fviewport_size_off);
            auto py = (int32_t*)((uintptr_t)viewport + *g_fviewport_size_off + 4);
            const int32_t cur_x = *px, cur_y = *py;
            if (ui_w >= 64 && ui_h >= 64 && cur_x >= 64 && cur_y >= 64 && (cur_x > ui_w || cur_y > ui_h)) {
                const float aspect = (float)cur_x / (float)cur_y;
                int32_t new_h = ui_h;
                int32_t new_w = (int32_t)((float)new_h * aspect + 0.5f);
                if (new_w > ui_w) { new_w = ui_w; new_h = (int32_t)((float)new_w / aspect + 0.5f); }
                fit_saved_x = cur_x;
                fit_saved_y = cur_y;
                *px = new_w;
                *py = new_h;
                fit_patched = true;
                static int32_t last_x = 0, last_y = 0;
                if (cur_x != last_x || cur_y != last_y) {
                    last_x = cur_x; last_y = cur_y;
                    SPDLOG_INFO("[LGUI_FIT] viewport SizeXY {}x{} -> {}x{} for UI draw (ui_target={}x{}, aspect={:.4f})",
                        cur_x, cur_y, new_w, new_h, ui_w, ui_h, aspect);
                }
            }
        }
    }

    call_orig();

    if (fit_patched) {
        auto px = (int32_t*)((uintptr_t)viewport + *g_fviewport_size_off);
        auto py = (int32_t*)((uintptr_t)viewport + *g_fviewport_size_off + 4);
        *px = fit_saved_x;
        *py = fit_saved_y;
    }

    // Perform synced eye rendering (synced AFR)
    if (in_engine_tick && vr->is_using_synchronized_afr()) {
        static bool hooked_viewport_draw = false;

        // Hook for FViewport::Draw
        if (g_hook->m_hooked_game_engine_tick && !hooked_viewport_draw) {
            hooked_viewport_draw = true;

            // Go up the stack and find the viewport draw function.
            constexpr auto max_stack_depth = 100;
            uintptr_t stack[max_stack_depth]{};

            const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
            if (depth >= 2) {
                // Log the stack functions
                for (auto i = 0; i < depth; ++i) {
                    SPDLOG_INFO("(Stack[{}]: {:x}", i, stack[i]);
                }

                auto try_hook_index = [&](uint32_t index) -> bool {
                    SPDLOG_INFO("Attempting to locate FViewport::Draw function @ stack[{}]", index);

                    const auto viewport_draw_middle = stack[index];
                    const auto viewport_draw = utility::find_function_start_with_call(viewport_draw_middle);

                    if (!viewport_draw) {
                        SPDLOG_ERROR("Failed to find viewport draw function @ {}", index);
                        return false;
                    }

                    SPDLOG_INFO("Found FViewport::Draw function at {:x}", (uintptr_t)*viewport_draw); 

                    g_hook->m_viewport_draw_hook = safetyhook::create_inline((void*)*viewport_draw, &viewport_draw_hook);

                    if (!g_hook->m_viewport_draw_hook) {
                        SPDLOG_ERROR("Failed to hook FViewport::Draw function!");
                        return false;
                    }

                    return true;
                };

                if (!try_hook_index(1)) {
                    // Fallback to index 3, on some UE4 games the viewport draw function is called from a different stack index.
                    if (!try_hook_index(2)) {
                        SPDLOG_ERROR("Failed to find viewport draw function! Cannot perform synced AFR!");
                    }
                }
            }
        }
    }

    // This is how synchronized AFR works. it forces a world draw
    // on the start of the next engine tick, before the world ticks again.
    // that will allow both views and the world to be drawn in sync with no artifacts.
    if (in_engine_tick && vr->is_using_synchronized_afr() && g_frame_count % 2 == 0) {
        GameThreadWorker::get().enqueue([=]() {
            if (g_hook->m_viewport_draw_hook && viewport != g_hook->m_last_destroyed_viewport) {
                __try {
                    auto& current_vtable = *(void***)viewport;

                    if (current_vtable == nullptr || current_vtable[0] == nullptr) {
                        SPDLOG_ERROR("FViewport::Draw called with a bad viewport pointer! This is not expected!");
                        return;
                    }

                    if (g_hook->m_last_viewport_vtable != nullptr && current_vtable != g_hook->m_last_viewport_vtable) {
                        SPDLOG_ERROR_EVERY_N_SEC(1, "FViewport::Draw called on a viewport with a different vtable! Updating cached vtable and continuing.");
                    }

                    // Self-heal instead of permanently disabling the forced second-eye draw: this vtable
                    // pointer is only ever set from the hooked FViewport::Draw, which may not have fired
                    // yet the first time this deferred callback runs (e.g. right when the game loads),
                    // leaving m_last_viewport_vtable null/stale and causing every synced frame to skip
                    // the second eye forever, resulting in a persistent black screen.
                    g_hook->m_last_viewport_vtable = current_vtable;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    SPDLOG_ERROR("FViewport::Draw called with a bad viewport pointer! This is not expected!");
                    return;
                }

                // Route the forced second-eye draw through our own viewport_draw_hook (instead of
                // calling the raw original trampoline directly) so it goes through the exact same
                // gating/state updates as the primary draw call (m_ignore_next_viewport_draw,
                // is_hmd_active() check, m_last_viewport_vtable update, etc). Calling the raw
                // trampoline directly skipped all of that, meaning the primary and forced draws
                // were running through subtly different code paths every tick in Synchronized
                // Sequential mode - a plausible cause of the UI focus/hover instability (menu
                // items highlight/flicker but don't confirm) reported only in that mode.
                const auto viewport_draw = [](void* vp, bool present) {
                    FFakeStereoRenderingHook::viewport_draw_hook(vp, present);
                };

                // LIVE DIAGNOSTIC TOGGLE (VR mod menu -> "DIAG_DisableForcedSecondDraw"): lets us
                // A/B test with the forced second-eye draw fully disabled, without rebuilding.
                // Screen will look broken (one eye won't update) while enabled for testing.
                if (!vr->is_synced_forced_second_draw_disabled()) {
                    // Mark that we're inside the manually-forced second FViewport::Draw call used to
                    // draw the second eye within the same engine tick for Synchronized Sequential mode.
                    // FViewport::Draw is also where the engine polls/pumps input devices, so calling it
                    // twice per tick causes a single physical button press to be seen as two separate
                    // input frames by the engine's own input processing. Input-facing hooks (e.g.
                    // XInputHook) check this flag to avoid re-dispatching the same input state as if it
                    // were a new frame during this forced redraw.
                    g_hook->m_in_synced_forced_viewport_draw = true;
                    viewport_draw(viewport, true);
                    g_hook->m_in_synced_forced_viewport_draw = false;
                }

                auto& vr = VR::get();
                const auto method = vr->get_synced_sequential_method();
                
                if (method == VR::SyncedSequentialMethod::SKIP_TICK) {
                    g_hook->m_ignore_next_engine_tick = true;
                    //g_hook->m_ignore_next_viewport_draw = true;
                } else if (method == VR::SyncedSequentialMethod::SKIP_DRAW) {
                    g_hook->m_ignore_next_viewport_draw = true;
                }
            }
        });
    }

    for (const auto& mod : mods) {
        mod->on_post_viewport_client_draw(viewport_client, viewport, canvas);
    }
}

static std::array<uintptr_t, 50> g_view_extension_vtable{};
static FSceneViewExtensions* g_engine_view_extensions{nullptr}; // GEngine->ViewExtensions, resolved in setup_view_extensions
struct SceneViewExtensionAnalyzer;

static void diag_dump_engine_view_extensions(sdk::FSceneViewFamily& view_family);

// Diagnostic (one-shot, background thread): find LGUI renderer string anchors in the game module and
// the functions that reference them, to locate the UI draw callsite since it is not a view extension.
static void diag_scan_lgui_render_anchors() {
    static bool started = false;
    if (started) return;
    started = true;

    std::thread([]() {
        try {
            const auto exe = utility::get_executable();
            const auto base = (uintptr_t)exe;
            const auto size = utility::get_module_size(exe).value_or(0);
            SPDLOG_INFO("[LGUI_ANCHOR] scanning module {:x} size {:x}", base, size);

            const std::vector<std::wstring> wanchors = {
                L"LGUIRenderer", L"RenderLGUI", L"LGUIHudRender", L"ScreenSpaceOverlay",
            };
            const std::vector<std::string> anchors = {
                "ScreenSpaceOverlay", "RenderLGUI", "LGUIHudRender",
            };

            auto report = [&](const std::string& tag, uintptr_t str_addr, bool wide) {
                // Rewind to the real start of the string (previous NUL) since substring hits land mid-string
                const size_t cw = wide ? 2 : 1;
                auto start = str_addr;
                for (size_t k = 0; k < 256; ++k) {
                    const auto prev = start - cw;
                    if (IsBadReadPtr((void*)prev, cw)) break;
                    const bool is_nul = wide ? (*(const uint16_t*)prev == 0) : (*(const uint8_t*)prev == 0);
                    if (is_nul) break;
                    start = prev;
                }

                auto refs = utility::scan_displacement_references(exe, start);
                const auto rel_count = refs.size();
                if (refs.empty()) {
                    if (const auto abs = utility::scan_reference(exe, start, false); abs.has_value()) {
                        refs.push_back(*abs);
                    }
                }

                std::string fns{};
                size_t shown = 0;
                for (const auto ref : refs) {
                    const auto fn = utility::find_function_start_with_call(ref);
                    fns += fmt::format("ref={:x}", ref - base);
                    if (fn) fns += fmt::format("(fn={:x})", *fn - base);
                    fns += ",";
                    if (++shown >= 8) { fns += "..."; break; }
                }

                SPDLOG_INFO("[LGUI_ANCHOR] {} @rva {:x} start={:x} refs={} rel={} [{}]", tag, str_addr - base, start - base, refs.size(), rel_count, fns);
            };

            for (const auto& a : wanchors) {
                const auto hits = utility::scan_strings(exe, a, false);
                SPDLOG_INFO("[LGUI_ANCHOR] L\"{}\" hits={}", utility::narrow(a), hits.size());
                size_t n = 0;
                for (const auto h : hits) {
                    std::wstring full{};
                    for (auto p = (const wchar_t*)h; !IsBadReadPtr(p, 2) && *p != 0 && full.size() < 96; ++p) full += *p;
                    report(fmt::format("  L\"{}\"", utility::narrow(full)), h, true);
                    if (++n >= 8) break;
                }
            }

            for (const auto& a : anchors) {
                const auto hits = utility::scan_strings(exe, a, false);
                SPDLOG_INFO("[LGUI_ANCHOR] \"{}\" hits={}", a, hits.size());
                size_t n = 0;
                for (const auto h : hits) {
                    std::string full{};
                    for (auto p = (const char*)h; !IsBadReadPtr(p, 1) && *p != 0 && full.size() < 96; ++p) full += *p;
                    report(fmt::format("  \"{}\"", full), h, false);
                    if (++n >= 8) break;
                }
            }

            SPDLOG_INFO("[LGUI_ANCHOR] scan complete");
        } catch (...) {
            SPDLOG_ERROR("[LGUI_ANCHOR] exception during scan");
        }
    }).detach();
}

struct SceneViewExtensionAnalyzer {
    template<int N>
    struct FillVtable {
        static void fill(std::array<uintptr_t, 50>& table);
        static void fill2(std::array<uintptr_t, 50>& table);
    };

    template<>
    struct FillVtable<-1> {
        static void fill(std::array<uintptr_t, 50>& table) {}
        static void fill2(std::array<uintptr_t, 50>& table) {}
    };

    struct AnalyzedFunction {
        uint32_t call_count{0};
        uint32_t frame_count_a2{0};
        uint32_t frame_count_a3{0};
        uint32_t frame_count_offset_a2{0};
        uint32_t frame_count_offset_a3{0};
        uint32_t times_frame_count_correct_a2{0};
        uint32_t times_frame_count_correct_a3{0};
        std::array<uint8_t, 0x100> a2_data{};
        std::array<uint8_t, 0x100> a3_data{};
    };

    static inline std::recursive_mutex dummy_mutex{};
    static inline uint32_t total_call_count{};
    static inline std::unordered_map<uint32_t, AnalyzedFunction> functions{};
    static inline bool has_found_is_active_this_frame_index{false};
    static inline bool has_found_begin_render_viewfamily{false};
    static inline bool index_0_called{false};
    
    static inline uint32_t is_active_this_frame_index{0};
    static inline uint32_t begin_render_viewfamily_index{0};
    static inline uint32_t pre_render_viewfamily_renderthread_index{0};
    static inline uint32_t frame_count_offset{0};

    template<int N>
    static bool analysis_dummy_stage1(ISceneViewExtension* extension, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
        if (N == 0) {
            index_0_called = true;
        }

        if (has_found_is_active_this_frame_index) {
            return false;
        }

        std::scoped_lock _{dummy_mutex};

        auto& func = functions[N];

        ++total_call_count;
        ++functions[N].call_count;

        if (total_call_count >= 50) {
            // Find the most called index, it's going to be IsActiveThisFrame
            uint32_t max_count = 0;
            uint32_t max_index = 0;

            for (const auto& func : functions) {
                const auto count = func.second.call_count;
                const auto index = func.first;

                if (count > max_count) {
                    max_count = count;
                    max_index = index;
                }
            }

            SPDLOG_INFO("[Stage 1] Found most called index to be {} with {} calls", max_index, max_count);

            functions.clear();
            FillVtable<g_view_extension_vtable.size() - 1>::fill2(g_view_extension_vtable);

            // Force the function to return true
            g_view_extension_vtable[max_index] = (uintptr_t)+[](ISceneViewExtension* ext) -> bool {
                return true;
            };

            has_found_is_active_this_frame_index = true;
            is_active_this_frame_index = max_index;
        } else {
            if (functions[N].call_count == 1) {
                SPDLOG_INFO("[Stage 1] ISceneViewExtension Index {} called for the first time!", N);
            }
        }

        return false;
    };

    template<int N>
    static bool analysis_dummy_stage2(ISceneViewExtension* extension, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
        if (has_found_begin_render_viewfamily) {
            return false;
        }

        if (N == 0) {
            index_0_called = true;
        }

        std::scoped_lock _{dummy_mutex};

        if (functions.contains(N)) {
            auto& func = functions[N];

            if (func.call_count++ == 0) {
                SPDLOG_INFO("[Stage 2] SceneViewExtension Index {} called for the first time!", N);
            }

            const auto& last_view_family_data_a2 = func.a2_data;
            const auto view_family_a2 = (uintptr_t)a2;

            if (a2 != 0 && !IsBadReadPtr((void*)a2, 0x100)) {
                for (auto i = 0x10; i < last_view_family_data_a2.size(); i += sizeof(uint32_t)) {
                    const auto a = *(uint32_t*)&last_view_family_data_a2[i];
                    const auto b = *(uint32_t*)&((uint8_t*)view_family_a2)[i];

                    if (b == a + 1 && a >= 10) { // rule out really low frame counts (this could be something else)
                        if (func.frame_count_a2 + 1 == b) {
                            SPDLOG_INFO("[A2] Function index {} Found frame count offset at {:x}, ({})", N, i, b);

                            func.frame_count_offset_a2 = i;
                            ++func.times_frame_count_correct_a2;

                            // func_next is one of the functions ahead of N and has the frame count in a3
                            AnalyzedFunction* func_next = nullptr;
                            uint32_t next_index = 0;

                            for (auto j = N + 1; j < g_view_extension_vtable.size(); j++) {
                                if (functions.contains(j)) {
                                    const auto& next = functions[j];

                                    if (next.times_frame_count_correct_a3 >= 10) {
                                        func_next = &functions[j];
                                        next_index = j;
                                        break;
                                    }
                                }
                            }

                            if (func_next != nullptr) {
                                if (func.times_frame_count_correct_a2 >= 50 && 
                                    func_next->times_frame_count_correct_a3 >= 50 && 
                                    func.frame_count_offset_a2 == func_next->frame_count_offset_a3 &&
                                    std::abs((int32_t)func.frame_count_a2 - (int32_t)func_next->frame_count_a3) <= 3) // In some games, the frame delta is really high but the same offset (so, it's wrong)
                                {
                                    SPDLOG_INFO("Found final frame count offset at {:x}", i);
                                    SPDLOG_INFO("Found BeginRenderViewFamily at index {}", N);
                                    SPDLOG_INFO("Found PreRenderViewFamily_RenderThread at index {}", next_index);
                                    has_found_begin_render_viewfamily = true;
                                    begin_render_viewfamily_index = N;
                                    pre_render_viewfamily_renderthread_index = next_index;

                                    frame_count_offset = i;
                                    sdk::FSceneViewFamily::set_frame_count_offset(frame_count_offset);

                                    setup_view_extension_hook();
                                    return false;
                                }   
                            }
                        }

                        func.frame_count_a2 = b;
                        break;
                    }
                }
            }

            const auto& last_view_family_data_a3 = func.a3_data;
            const auto view_family_a3 = (uintptr_t)a3;

            if (a3 != 0 && !IsBadReadPtr((void*)a3, 0x100)) {
                for (auto i = 0x10; i < last_view_family_data_a3.size(); i += sizeof(uint32_t)) {
                    const auto a = *(uint32_t*)&last_view_family_data_a3[i];
                    const auto b = *(uint32_t*)&((uint8_t*)view_family_a3)[i];

                    if (b == a + 1 && a >= 10) { // rule out really low frame counts (this could be something else)
                        if (func.frame_count_a3 + 1 == b) {
                            SPDLOG_INFO("[A3] Function index {} Found frame count offset at {:x} ({})", N, i, b);
                            ++func.times_frame_count_correct_a3;
                        }

                        func.frame_count_a3 = b;
                        func.frame_count_offset_a3 = i;
                        break;
                    }
                }
            }
        }

        if (a2 != 0 && !IsBadReadPtr((void*)a2, 0x100)) {
            memcpy(functions[N].a2_data.data(), (void*)a2, 0x100);
        }

        if (a3 != 0 && !IsBadReadPtr((void*)a3, 0x100)) {
            memcpy(functions[N].a3_data.data(), (void*)a3, 0x100);
        }

        return false;
    }

    static inline std::recursive_mutex vtable_mutex{};
    static inline std::unordered_map<sdk::FRHICommandBase_New*, void**> original_vtables{};
    static inline std::unordered_map<sdk::FRHICommandBase_New*, uint32_t> cmd_frame_counts{};

    // Meant to be called after analysis has been completed
    static void setup_view_extension_hook() {
        std::scoped_lock _{dummy_mutex};

        SPDLOG_INFO("Setting up BeginRenderViewFamily hook...");

        const auto setup_view_family_index = index_0_called ? 0 : 1;

        g_view_extension_vtable[setup_view_family_index] = (uintptr_t)&FFakeStereoRenderingHook::setup_view_family;
        g_view_extension_vtable[begin_render_viewfamily_index] = (uintptr_t)&FFakeStereoRenderingHook::begin_render_viewfamily;

        if (!index_0_called && (setup_view_family_index + 2) != begin_render_viewfamily_index) {
            g_view_extension_vtable[setup_view_family_index + 2] = (uintptr_t)&FFakeStereoRenderingHook::setup_viewpoint;
        }

        // PreRenderViewFamily_RenderThread
        g_view_extension_vtable[pre_render_viewfamily_renderthread_index] = (uintptr_t)&FFakeStereoRenderingHook::pre_render_viewfamily_renderthread;

        SPDLOG_INFO("Done setting up BeginRenderViewFamily hook!");
    }

    static inline std::unordered_set<int> tested_execute_indices{};
    static inline int correct_execute_index{0};
    static inline bool found_correct_execute{false};

    template<int N>
    static void* hooked_command_fn(sdk::FRHICommandBase_New* cmd, sdk::FRHICommandListBase* cmd_list, void* debug_context, void* r9, void* stack_1, void* stack_2, void* stack_3, void* stack_4, void* stack_5, void* stack_6, void* stack_7, void* stack_8) {
        std::scoped_lock _{vtable_mutex};
        //std::scoped_lock __{VR::get()->get_vr_mutex()};

        static bool once = true;

        if (once) {
            SPDLOG_INFO("[ISceneViewExtension] Successfully hijacked command list! {}", N);
        }

        const auto original_vtable = original_vtables[cmd];
        const auto original_func = original_vtable[N];

        const auto func = (decltype(hooked_command_fn<N>)*)original_func;
        const auto frame_count = cmd_frame_counts[cmd];

        if (once) {
            SPDLOG_INFO("[ISceneViewExtension] Command list frame count: {}", frame_count);
            SPDLOG_INFO("[ISceneViewExtension] Original vtable: {:x}", (uintptr_t)original_vtable);
            once = false;
        }

        if (!found_correct_execute && !tested_execute_indices.contains(N) && VR::get()->get_present_thread_id() != 0) {
            tested_execute_indices.insert(N);

            // N == 0 is a pretty safe heuristic
            // Otherwise if >= 1 gets called first, we can assume if the thread is the same
            // as the DXGI present thread, then it's the correct execute function
            if (N == 0 || GetCurrentThreadId() == VR::get()->get_present_thread_id()) {
                correct_execute_index = N;
                found_correct_execute = true;
                SPDLOG_INFO("[ISceneViewExtension] Found correct execute index: {}", N);
            }
        }

        auto& vr = VR::get();
        auto runtime = vr->get_runtime();

        auto call_orig = [=]() {
            const auto result = func(cmd, cmd_list, debug_context, r9, stack_1, stack_2, stack_3, stack_4, stack_5, stack_6, stack_7, stack_8);

            if (N == correct_execute_index) {
                runtime->enqueue_render_poses(frame_count);
            }

            return result;
        };

        if (N != correct_execute_index) {
            return call_orig();
        }

        // set the vtable back
        *(void**)cmd = original_vtable;
        original_vtables.erase(cmd);
        cmd_frame_counts.erase(cmd);

        RHIThreadWorker::get().execute();

        if (vr->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
            if (runtime->is_openxr()) {
                if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                    if (!runtime->got_first_sync || runtime->synchronize_frame(frame_count) != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }  
                } else if (runtime->synchronize_frame(frame_count) != VRRuntime::Error::SUCCESS) {
                    return call_orig();
                }

                vr->get_openxr_runtime()->begin_frame();
            } else {
                if (runtime->synchronize_frame(frame_count) != VRRuntime::Error::SUCCESS) {
                    return call_orig();
                }
            }
        }

        return call_orig();
    }

    static void hook_new_rhi_command(sdk::FRHICommandBase_New* last_command, uint32_t frame_count) {
        std::scoped_lock __{vtable_mutex};

        auto runtime = VR::get()->get_runtime();
        runtime->on_pre_render_render_thread(frame_count);

        if (last_command == nullptr || *(void**)last_command == nullptr) {
            SPDLOG_INFO("Cannot hook command with no vtable, falling back to passing current frame count to runtime");
            runtime->enqueue_render_poses(frame_count);
            return;
        }

        // Whichever one gets called first is the winner winner chicken dinner
        static std::array<uintptr_t, 7> new_vtable{
            (uintptr_t)&hooked_command_fn<0>,
            (uintptr_t)&hooked_command_fn<1>,
            (uintptr_t)&hooked_command_fn<2>,
            (uintptr_t)&hooked_command_fn<3>,
            (uintptr_t)&hooked_command_fn<4>,
            (uintptr_t)&hooked_command_fn<5>,
            (uintptr_t)&hooked_command_fn<6>
        };

        cmd_frame_counts[last_command] = frame_count;

        if (original_vtables.contains(last_command) || *(void**)last_command == new_vtable.data()) {
            static auto last_log_time = std::chrono::high_resolution_clock::time_point{};
            const auto now = std::chrono::high_resolution_clock::now();
            
            if (now - last_log_time > std::chrono::seconds(1)) {
                SPDLOG_WARN("Something strange is going on, the vtable is already hooked, maybe previous frame was not rendered?");
                last_log_time = now;
            }

            return;
        }

        original_vtables[last_command] = *(void***)last_command;
        *(void***)last_command = (void**)new_vtable.data();
    }

    static void hook_old_rhi_command(sdk::FRHICommandBase_Old* last_command, uint32_t frame_count) {
        static std::recursive_mutex func_mutex{};
        static std::unordered_map<sdk::FRHICommandBase_Old*, sdk::FRHICommandBase_Old::Func> original_funcs{};
        static std::unordered_map<sdk::FRHICommandBase_Old*, uint32_t> cmd_frame_counts{};

        std::scoped_lock __{func_mutex};

        auto runtime = VR::get()->get_runtime();
        runtime->on_pre_render_render_thread(frame_count);

        cmd_frame_counts[last_command] = frame_count;

        if (original_funcs.contains(last_command)) {
            static auto last_log_time = std::chrono::high_resolution_clock::time_point{};
            const auto now = std::chrono::high_resolution_clock::now();
            
            if (now - last_log_time > std::chrono::seconds(1)) {
                SPDLOG_WARN("Something strange is going on, the function is already hooked, maybe previous frame was not rendered?");
                last_log_time = now;
            }

            return;
        }

        static auto func_override = (sdk::FRHICommandBase_Old::Func)+[](sdk::FRHICommandListBase* cmd_list, sdk::FRHICommandBase_Old* cmd) {
            std::scoped_lock _{func_mutex};
            //std::scoped_lock __{VR::get()->get_vr_mutex()};

            static bool once = true;

            if (once) {
                SPDLOG_INFO("[ISceneViewExtension] Successfully hijacked command list!");
                once = false;
            }

            auto& vr = VR::get();
            auto runtime = vr->get_runtime();

            const auto func = original_funcs[cmd];
            const auto frame_count = cmd_frame_counts[cmd];

            runtime->enqueue_render_poses(frame_count);
            runtime->on_pre_render_rhi_thread(frame_count);

            auto call_orig = [&]() {
                func(*cmd_list, cmd);
            };

            cmd->func = func;
            original_funcs.erase(cmd);
            cmd_frame_counts.erase(cmd);

            RHIThreadWorker::get().execute();

            if (vr->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
                if (runtime->is_openxr()) {
                    if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                        if (!runtime->got_first_sync || runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                            return call_orig();
                        }  
                    } else if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }

                    vr->get_openxr_runtime()->begin_frame();
                } else {
                    if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }
                }
            }

            return call_orig();
        };

        original_funcs[last_command] = last_command->func;
        last_command->func = func_override;
    }
};

// Diagnostic: enumerates every ISceneViewExtension registered in GEngine->ViewExtensions
// so we can identify which one belongs to the game's UI renderer (e.g. LGUI's FLGUIRenderer)
// LGUI screen-space UI draw: ISceneViewExtension slot 24 on the two LGUI classes (vtable rva 26ca0760 / 26ca0830).
// Confirmed by bisect: stubbing slot 24 on both removes main menu UI and in-game HUD; 25 alone does not.
static std::unordered_map<uintptr_t, uintptr_t> g_lgui_slot24_originals{}; // vtable -> original fn
static constexpr int32_t LGUI_DRAW_SLOT = 24;

// Game-thread ISceneViewExtension callbacks on the LGUI classes. The canvas layout (the rect LGUI paints into, measured by
// LGUI_BOUNDS as exactly hmd_w x hmd_h) is fixed before the render-thread draw, so it must be read from the FSceneView /
// FSceneViewFamily in one of these. Stock layout: 2 = SetupView(Family, View), 13 = BeginRenderViewFamily(Family).
// While LGUI runs inside the callback, present every (0,0,hmd_w,hmd_h) rect in the view as (0,0,ui_w,ui_h) so its ortho
// canvas is laid out 16:9 for ui_target; restore afterwards so the scene keeps the real eye rect.
static constexpr int32_t LGUI_SETUP_VIEW_SLOT = 2;
static constexpr int32_t LGUI_BEGIN_FAMILY_SLOT = 13;
static std::unordered_map<uintptr_t, uintptr_t> g_lgui_setup_view_originals{};
static std::unordered_map<uintptr_t, uintptr_t> g_lgui_begin_family_originals{};

struct LguiRectPatch {
    int32_t* p{};
    int32_t saved[4]{};
};

static void lgui_patch_view_rects(uintptr_t obj, uint32_t len, const char* tag, std::vector<LguiRectPatch>& patches, std::string& where) {
    if (obj < 0x10000 || IsBadReadPtr((void*)obj, len)) return;

    const auto hw = (int32_t)VR::get()->get_hmd_width();
    const auto hh = (int32_t)VR::get()->get_hmd_height();
    if (hw <= 0 || hh <= 0 || g_hook == nullptr || g_hook->get_render_target_manager() == nullptr) return;

    const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
    if (ui_target == nullptr || IsBadReadPtr(ui_target, 0x60)) return;
    const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
    const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
    if (ui_w < 64 || ui_h < 64 || ui_w > 16384 || ui_h > 16384) return;

    for (uint32_t off = 0; off + 16 <= len; off += 4) {
        auto r = (int32_t*)(obj + off);
        // FIntRect {Min.X, Min.Y, Max.X, Max.Y}: any rect that is exactly one eye (hw x hh) or the family (2hw x hh) tall/wide.
        // With NSF off the right eye sits at Min.X == hw; patch it too (only LGUI sees it, the scene is restored right after).
        const int32_t w = r[2] - r[0];
        const int32_t h = r[3] - r[1];
        const bool eye = r[1] == 0 && h == hh && w == hw && (r[0] == 0 || r[0] == hw);
        const bool wide = r[0] == 0 && r[1] == 0 && w == hw * 2 && h == hh;
        if (!eye && !wide) continue;

        LguiRectPatch pt{r, {r[0], r[1], r[2], r[3]}};
        patches.push_back(pt);
        r[0] = 0; r[1] = 0; r[2] = ui_w; r[3] = ui_h;
        where += fmt::format("{}+{:x}{} ", tag, off, wide ? "(2w)" : (pt.saved[0] != 0 ? "(R)" : ""));
        off += 12;
    }

    // Also look for bare (hw, hh) size pairs (e.g. FIntPoint ViewSize / BufferSize) that are not part of a rect we just patched.
    for (uint32_t off = 0; off + 8 <= len; off += 4) {
        auto p = (int32_t*)(obj + off);
        if (!((p[0] == hw || p[0] == hw * 2) && p[1] == hh)) continue;
        bool already = false;
        for (const auto& pt : patches) {
            const auto b = (uintptr_t)pt.p;
            if ((uintptr_t)p >= b && (uintptr_t)p < b + 16) { already = true; break; }
        }
        if (already) continue;
        LguiRectPatch pt{p, {p[0], p[1], 0, 0}};
        pt.p = p;
        patches.push_back(pt);
        p[0] = ui_w; p[1] = ui_h;
        where += fmt::format("{}+{:x}(sz) ", tag, off);
        off += 4;
    }
}

static void lgui_restore_view_rects(std::vector<LguiRectPatch>& patches) {
    for (auto& pt : patches) {
        pt.p[0] = pt.saved[0]; pt.p[1] = pt.saved[1];
        // size-pair entries have saved[2]==saved[3]==0 and must not touch the following 8 bytes
        if (pt.saved[2] != 0 || pt.saved[3] != 0) { pt.p[2] = pt.saved[2]; pt.p[3] = pt.saved[3]; }
    }
}

// ==== EXPERIMENT: render-thread-scoped real FSceneView::ViewRect swap ==============================
// Tests the user's hypothesis directly: hook LGUI's render pass, temporarily overwrite the REAL
// FSceneView::ViewRect (and any per-eye/family rect it carries) to the ui_target size for the DURATION
// of LGUI's draw ONLY, then restore synchronously the instant LGUI returns. Because a3 here is the
// actual render-thread FSceneView/FSceneViewFamily the LGUI pass consumes, this is exactly "swap
// during LGUI's pass, restore immediately after" - not the earlier persistent/game-thread mutations.
// If LGUI's painted extent (measured by [LGUI_BOUNDS] in D3D12Component) grows to the full ui_target
// height with this on, the view-rect IS LGUI's layout source; if it stays clipped, the theory is
// disproven and the target-sizing fix remains the only lever. LEAVE FALSE for normal play; the swap
// touches the live scene view and must only run while actively measuring (enable LGUI_BOUNDS_DIAG too).
// RESULT (2026-09-05): DISPROVEN. Swap fired (a3view+a88 -> 3840x2683) but [LGUI_BOUNDS] painted width
// stayed at hmd_w (2268), never 3840 -> LGUI does NOT read FSceneView::ViewRect for its raster extent;
// it clips to the render-target/HMD size. The ui_target sizing fix remains the only working lever.
constexpr bool LGUI_PROBE_VIEWRECT_SWAP = false;

// Collect the real FSceneView object(s) reachable from a3 (either a3 IS an FSceneView, or a3 is an
// FSceneViewFamily whose views[] we walk) and patch their per-eye/family view rects to ui_target size.
// Patches are recorded so the caller restores them synchronously right after LGUI's draw returns.
static void lgui_probe_swap_view_rects(void* a3, std::vector<LguiRectPatch>& patches, std::string& where) {
    if (a3 == nullptr || IsBadReadPtr(a3, 0x100)) return;

    // The FSceneView layout is large; ViewRect / UnconstrainedViewRect live within the first ~0xB00 bytes.
    constexpr uint32_t VIEW_SCAN_LEN = 0xB00;

    // Hypothesis A: a3 is an FSceneViewFamily. Its views resolve back to a3 (view->Family == a3).
    const auto fam = (sdk::FSceneViewFamily*)a3;
    const auto views = fam->get_views();
    bool patched_family = false;
    if (views != nullptr && !IsBadReadPtr(views, sizeof(*views)) && views->count > 0 && views->count <= 4 &&
        views->data != nullptr && !IsBadReadPtr(views->data, sizeof(void*) * views->count))
    {
        for (int32_t k = 0; k < views->count; ++k) {
            const auto v = (uintptr_t)views->data[k];
            if (v == 0 || IsBadReadPtr((void*)v, VIEW_SCAN_LEN) || *(uintptr_t*)v != (uintptr_t)a3) { patched_family = false; break; }
            lgui_patch_view_rects(v, VIEW_SCAN_LEN, "view", patches, where);
            patched_family = true;
        }
    }

    // Hypothesis B: a3 is itself an FSceneView. Patch it directly.
    if (!patched_family) {
        lgui_patch_view_rects((uintptr_t)a3, VIEW_SCAN_LEN, "a3view", patches, where);
    }
}

// Cap the HEIGHT of any eye/family view rect to ui_h so LGUI's canvas never exceeds the capture target's height.
// With NSF ON the per-eye render height (e.g. 3193) is taller than ui_target (2160); LGUI lays its canvas out at that
// height and paints top-left, so the bottom is clipped off the target. Clamping only Max.Y (never enlarging, never
// touching width/X) keeps the canvas within 2160 so the full UI fits. Patches are recorded so the caller restores them
// synchronously right after LGUI captures its layout - they never persist into a later scene render.
static void lgui_cap_view_rect_height(uintptr_t obj, uint32_t len, const char* tag, std::vector<LguiRectPatch>& patches, std::string& where) {
    if (obj < 0x10000 || IsBadReadPtr((void*)obj, len)) return;

    const auto hw = (int32_t)VR::get()->get_hmd_width();
    const auto hh = (int32_t)VR::get()->get_hmd_height();
    if (hw <= 0 || hh <= 0 || g_hook == nullptr || g_hook->get_render_target_manager() == nullptr) return;

    const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
    if (ui_target == nullptr || IsBadReadPtr(ui_target, 0x60)) return;
    const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
    if (ui_h < 64 || ui_h > 16384) return;
    if (hh <= ui_h) return; // eye already fits, nothing to cap

    for (uint32_t off = 0; off + 16 <= len; off += 4) {
        auto r = (int32_t*)(obj + off);
        const int32_t w = r[2] - r[0];
        const int32_t h = r[3] - r[1];
        // FIntRect {Min.X, Min.Y, Max.X, Max.Y}: an eye (hw x hh) or family (2hw x hh) rect at Min.Y == 0.
        const bool eye = r[1] == 0 && h == hh && w == hw && (r[0] == 0 || r[0] == hw);
        const bool wide = r[0] == 0 && r[1] == 0 && w == hw * 2 && h == hh;
        if (!eye && !wide) continue;

        LguiRectPatch pt{r, {r[0], r[1], r[2], r[3]}};
        patches.push_back(pt);
        r[3] = r[1] + ui_h; // clamp Max.Y so height == ui_h; leave X/width/Min.Y untouched
        where += fmt::format("{}+{:x}(capH) ", tag, off);
        off += 12;
    }
}


// RE-ENABLED (attempt #5): feed LGUI a fixed ui_target-sized (3840x2160) view/family rect during its OWN SetupView so
// its canvas lays out at the capture-target resolution instead of the per-eye stereo rect (e.g. 2699x3193). This isolates
// the UI's layout resolution from the stereo/eye resolution and fixes the NSF ON bottom cutoff (canvas no longer 3193 tall).
// The patch is applied and RESTORED synchronously around the original SetupView call, so the real 3D scene render for that
// eye - which reads its own live FSceneView rect - is never affected. The earlier suspected "lag" was actually the global
// D3D12 viewport/scissor vtable hook (now disabled), not this patch. Quad aspect is still corrected at presentation.
// SUPERSEDED by Option 2 (game-thread FSceneView clone, LGUI_CLONE_VIEW below): this in-place patch mutates the REAL shared
// view rect (even if restored synchronously) and must NOT run alongside the clone path, or LGUI reads a doubly-modified view.
// Disabled so only the clone (pristine-copy) route feeds LGUI its ui_target-sized rects.
constexpr bool LGUI_PATCH_GAME_THREAD_RECTS = false;

// Master debug toggle for the steady-state UI/LGUI investigation diagnostics that would otherwise emit
// every frame (game-thread [LGUI_GT] SetupView dumps + dimension/float scans, the [VIEWEXT_DIAG] engine
// view-extension vtable walk, and the [DIAG] AdjustViewRect trace). These do module lookups, IsBadReadPtr
// walks and fmt::format string building on hot threads, so they are a confirmed steady-state cost. Leave
// FALSE for normal play; flip to TRUE only when actively re-probing the UI size sources.
constexpr bool LGUI_DIAG_STEADY_STATE = false;

// SEH wrapper (no C++ objects) for calling the LGUI draw with a possibly-null depth texture.
static bool lgui_call_draw_seh(void* (*fn)(void*, void*, void*, void*, void*, void*), void* self, void* a2, void* a3, void* a4, void* a5, void* a6, void** out) {
    __try {
        *out = fn(self, a2, a3, a4, a5, a6);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH wrapper (no unwinding C++ objects) for calling SetupView with cloned view/family args. Returns false on fault.
static bool lgui_call_setup_view_seh(void (*fn)(void*, void*, void*), void* self, void* family, void* view) {
    __try {
        fn(self, family, view);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void lgui_setup_view_hook(void* self, void* family, void* view) {
    const auto vtable = *(uintptr_t*)self;
    const auto it = g_lgui_setup_view_originals.find(vtable);
    if (it == g_lgui_setup_view_originals.end()) return;

    static uint32_t n = 0;
    const auto k = n++;
    // REVERTED: deferring restoration of view/family rects across the frame boundary (previous attempt) corrupted
    // the REAL FSceneView/FSceneViewFamily rects (ViewRect/UnconstrainedViewRect) that the actual 3D scene render
    // for that eye also reads - not just LGUI. This produced a new regression (right eye pushed up with a black
    // bar = its real view rect was patched to ui_target size while the scene rendered). These are live engine
    // objects shared with the renderer, so the patch must be applied and restored within the SAME call, synchronously
    // around the original function, exactly as before. LGUI's actual render-thread read is instead handled correctly-
    // timed in lgui_slot24_hook (patches a2/refs there, at render-thread execute time) - do not try to solve LGUI
    // timing by holding the game-thread view/family patch open across frames.
    std::vector<LguiRectPatch> patches{};
    std::string where{};

    if (LGUI_PATCH_GAME_THREAD_RECTS && VR::get()->is_hmd_active()) {
        lgui_patch_view_rects((uintptr_t)view, 0x1800, "view", patches, where);
        lgui_patch_view_rects((uintptr_t)family, 0x400, "family", patches, where);
    }

    // Height cap: independent of the (disabled) full-rect patch above. Clamp only the view rect HEIGHT to ui_h so
    // LGUI's canvas fits inside the 2160-tall capture target and its bottom is no longer clipped when the per-eye
    // render height exceeds 2160 (NSF ON at higher HMD resolutions). Restored synchronously below.
    // Redundant when LGUI_PATCH_GAME_THREAD_RECTS is on (the full-rect patch already sets height to ui_h). Kept for
    // fallback use if the full-rect patch is ever disabled again.
    constexpr bool LGUI_CAP_VIEW_HEIGHT = false;
    if (LGUI_CAP_VIEW_HEIGHT && VR::get()->is_hmd_active()) {
        lgui_cap_view_rect_height((uintptr_t)view, 0x1800, "view", patches, where);
        lgui_cap_view_rect_height((uintptr_t)family, 0x400, "family", patches, where);
    }

    // OPTION 1 (spatial UI/scene separation): patch LGUI's OWN canvas fields BEFORE calling the original SetupView, so
    // LGUI computes its ortho canvas / projection from the ui_target size (3840x2160, 16:9) instead of recomputing it
    // from the shared per-eye FSceneView rect (e.g. 2699x3193, ~0.85 aspect). These are LGUI-private members - NOT the
    // shared FSceneView/FSceneViewFamily rects the 3D scene renderer reads - so unlike the reverted deferred-restore
    // experiment this can never corrupt the stereo scene render. Fields (confirmed via [LGUI_GT] diagnostics):
    //   self+0xb4, self+0x104 : canvas aspect ratio (== eye/wide aspect); set to ui aspect
    //   self+0x264, self+0x268 : canvas width/height ints (== hmd_w/hmd_h); set to ui_w/ui_h
    // NOTE: the existing LGUI_PATCH_SELF_ASPECT / LGUI_PATCH_SELF_TREE_SIZES blocks below run AFTER the original call and
    // were being overridden because SetupView recomputes these from the view rect during the call. Doing it BEFORE lets
    // SetupView's own math consume the ui-target size. Kept behind a clearly-labelled toggle so it can be reverted instantly.
    // These fields belong to the persistent LGUI object, so they must be RESTORED after the original call (below) to avoid
    // permanently mutating engine state between frames.
    // CONFIRMED NO-OP (2026-09-05): [LGUI_BOUNDS] stayed at 2700x2160 with these fields patched, so LGUI ignores its
    // own aspect/size members for the raster extent and reads the shared FSceneView view rect directly. Disabled; the
    // FSceneView view rect is instead handled render-thread-locally in lgui_slot24_hook (Option 3). Code kept for record.
    constexpr bool LGUI_PATCH_SELF_PRECALL = false;
    constexpr uint32_t LGUI_SELF_ASPECT_OFFS_PRE[] = {0xb4, 0x104};
    constexpr uint32_t LGUI_SELF_SIZE_OFFS_PRE[] = {0x264, 0x268}; // {width, height}
    struct SelfFieldSave { void* addr; uint32_t raw; };
    std::vector<SelfFieldSave> self_pre_saved{};
    if (LGUI_PATCH_SELF_PRECALL && VR::get()->is_hmd_active() && self != nullptr && !IsBadReadPtr(self, 0x300) &&
        g_hook != nullptr && g_hook->get_render_target_manager() != nullptr)
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
        const auto hw = (int32_t)VR::get()->get_hmd_width();
        const auto hh = (int32_t)VR::get()->get_hmd_height();
        if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60) && hw > 0 && hh > 0) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
            if (ui_w >= 64 && ui_h >= 64) {
                const float eye_aspect = (float)hw / (float)hh;
                const float wide_aspect = (hw * 2.0f) / (float)hh;
                const float ui_aspect = (float)ui_w / (float)ui_h;
                for (auto off : LGUI_SELF_ASPECT_OFFS_PRE) {
                    auto p = (float*)((uintptr_t)self + off);
                    if (std::fabs(*p - eye_aspect) <= eye_aspect * 1e-4f || std::fabs(*p - wide_aspect) <= wide_aspect * 1e-4f) {
                        SelfFieldSave s{}; s.addr = p; memcpy(&s.raw, p, 4); self_pre_saved.push_back(s);
                        *p = ui_aspect;
                    }
                }
                for (auto off : LGUI_SELF_SIZE_OFFS_PRE) {
                    auto p = (int32_t*)((uintptr_t)self + off);
                    const int32_t want = (off == LGUI_SELF_SIZE_OFFS_PRE[0]) ? ui_w : ui_h;
                    if (*p == hw || *p == hh || *p == hw * 2) {
                        SelfFieldSave s{}; s.addr = p; memcpy(&s.raw, p, 4); self_pre_saved.push_back(s);
                        *p = want;
                    }
                }
                static size_t last_pre = SIZE_MAX;
                if (self_pre_saved.size() != last_pre) {
                    last_pre = self_pre_saved.size();
                    SPDLOG_INFO("[LGUI_GT] self pre-call patched {} fields (eye_aspect={:.4f} -> ui_aspect={:.4f}, {}x{} -> {}x{})",
                        self_pre_saved.size(), eye_aspect, ui_aspect, hw, hh, ui_w, ui_h);
                }
            }
        }
    }

    // One-shot: dump every int in the view that looks like a dimension (64..16384) so remaining size fields can be found.
    static bool dumped = false;
    if (!dumped && !patches.empty() && view != nullptr && !IsBadReadPtr(view, 0x1800)) {
        dumped = true;
        std::string dims{};
        for (uint32_t off = 0; off + 4 <= 0x1800; off += 4) {
            const auto v = *(int32_t*)((uintptr_t)view + off);
            if (v >= 64 && v <= 16384) dims += fmt::format("{:x}={} ", off, v);
        }
        SPDLOG_INFO("[LGUI_GT] view dimension-like ints (post-patch): {}", dims);

        // Projection matrices: a perspective projection has [0][0] = 1/tan(fovx/2), [1][1] = 1/tan(fovy/2) with
        // [1][1]/[0][0] == aspect (w/h). Log every 4x4 float block whose diagonal ratio equals the eye or wide aspect.
        const float hw = (float)VR::get()->get_hmd_width();
        const float hh = (float)VR::get()->get_hmd_height();
        const float eye = hw / hh, wide = 2.0f * hw / hh;
        std::string mats{};
        for (uint32_t off = 0; off + 64 <= 0x1800; off += 16) {
            const auto m = (const float*)((uintptr_t)view + off);
            const float a = m[0], b = m[5];
            if (!(std::fabs(a) > 1e-3f && std::fabs(a) < 100.0f && std::fabs(b) > 1e-3f && std::fabs(b) < 100.0f)) continue;
            const float ratio = b / a;
            if (std::fabs(ratio - eye) < eye * 2e-3f || std::fabs(ratio - wide) < wide * 2e-3f || std::fabs(ratio - 1.0f / eye) < 2e-3f) {
                mats += fmt::format("{:x}(m00={:.4f} m11={:.4f} r={:.4f}) ", off, a, b, ratio);
            }
        }
        SPDLOG_INFO("[LGUI_GT] view matrices with eye/wide aspect diagonal: {}", mats.empty() ? "<none>" : mats);
    }

    static size_t last_count = SIZE_MAX;
    if (LGUI_DIAG_STEADY_STATE && (k < 5 || patches.size() != last_count || (k % 600) == 0)) {
        last_count = patches.size();
        SPDLOG_INFO("[LGUI_GT] SetupView #{} self={:x} family={:x} view={:x} rects patched={} at [{}] (hmd={}x{} nsf={})",
            k, (uintptr_t)self, (uintptr_t)family, (uintptr_t)view, patches.size(), where.empty() ? "-" : where,
            VR::get()->get_hmd_width(), VR::get()->get_hmd_height(), VR::get()->is_native_stereo_fix_enabled());
    }

    // OPTION 2 (game-thread FSceneView clone): the FSceneView/FSceneViewFamily passed here are the SAME objects the 3D
    // scene renderer reads, so we cannot patch their rects in place without corrupting stereo. Instead, hand LGUI's
    // SetupView deep-enough COPIES of view/family whose rects are overwritten to the ui_target size (3840x2160, 16:9),
    // while the real objects stay pristine for the scene render. The clones must OUTLIVE this call because LGUI stores
    // the pointer and the render thread reads it later in the frame - use static ring buffers. SEH-guarded because
    // SetupView may dereference a member pointer we copied that now points at freed/relative data. Behind a toggle so it
    // reverts instantly; on any fault we fall back to the real objects for the rest of the session.
    // CONFIRMED INERT (2026-09-05): the clone fired cleanly with no stereo corruption, but [LGUI_BOUNDS] still showed the
    // paint capped at 2700x2160 (== per-eye hmd width). LGUI does NOT read the FSceneView view rect for its raster extent;
    // it sizes its pass from the FViewport/render-target size instead. Disabled to avoid the per-call memcpy overhead.
    // The real lever is the LGUI pass viewport width (handled elsewhere), not this view/family clone.
    constexpr bool LGUI_CLONE_VIEW = false;
    void* view_arg = view;
    void* family_arg = family;
    static bool clone_faulted = false;
    if (LGUI_CLONE_VIEW && !clone_faulted && VR::get()->is_hmd_active() &&
        view != nullptr && !IsBadReadPtr(view, 0x1800) && family != nullptr && !IsBadReadPtr(family, 0x400) &&
        g_hook != nullptr && g_hook->get_render_target_manager() != nullptr)
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
        const auto hw = (int32_t)VR::get()->get_hmd_width();
        const auto hh = (int32_t)VR::get()->get_hmd_height();
        if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60) && hw > 0 && hh > 0) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
            if (ui_w >= 64 && ui_h >= 64) {
                constexpr size_t VIEW_COPY = 0x1800;
                constexpr size_t FAMILY_COPY = 0x400;
                constexpr size_t RING = 4; // one per in-flight frame/eye
                static uint8_t view_ring[RING][VIEW_COPY]{};
                static uint8_t family_ring[RING][FAMILY_COPY]{};
                static uint32_t ring_idx = 0;
                const auto slot = ring_idx++ % RING;
                auto vc = view_ring[slot];
                auto fc = family_ring[slot];
                memcpy(vc, view, VIEW_COPY);
                memcpy(fc, family, FAMILY_COPY);

                // Overwrite any eye/family (0,0,hw,hh)/(0,0,2hw,hh) FIntRect in the CLONES with the ui_target rect.
                auto rewrite_rects = [&](uint8_t* base, size_t len) {
                    for (size_t off = 0; off + 16 <= len; off += 4) {
                        auto r = (int32_t*)(base + off);
                        const int32_t w = r[2] - r[0];
                        const int32_t h = r[3] - r[1];
                        const bool eye = r[1] == 0 && h == hh && w == hw && (r[0] == 0 || r[0] == hw);
                        const bool wide = r[0] == 0 && r[1] == 0 && w == hw * 2 && h == hh;
                        if (eye || wide) {
                            r[0] = 0; r[1] = 0; r[2] = ui_w; r[3] = ui_h;
                            off += 12;
                        }
                    }
                };
                rewrite_rects(vc, VIEW_COPY);
                rewrite_rects(fc, FAMILY_COPY);

                view_arg = vc;
                family_arg = fc;

                static bool logged_clone = false;
                if (!logged_clone) {
                    logged_clone = true;
                    SPDLOG_INFO("[LGUI_GT] Option 2 clone active: passing cloned view/family with rects -> {}x{} (real objects untouched)", ui_w, ui_h);
                }
            }
        }
    }

    // Call the original with the (possibly cloned) arguments. SEH-guarded: if LGUI faults on a cloned pointer member,
    // fall back to the real objects permanently for this session so the game keeps running.
    if (view_arg != view || family_arg != family) {
        if (!lgui_call_setup_view_seh((void(*)(void*, void*, void*))it->second, self, family_arg, view_arg)) {
            clone_faulted = true;
            SPDLOG_ERROR("[LGUI_GT] Option 2 clone faulted in SetupView; reverting to real view/family for the rest of the session");
            ((void(*)(void*, void*, void*))it->second)(self, family, view);
        }
    } else {
        ((void(*)(void*, void*, void*))it->second)(self, family, view);
    }

    // Restore the pre-call LGUI-private field patches: SetupView has now consumed the ui-target size for its canvas
    // math, and we must not leave the persistent LGUI object mutated between frames.
    for (auto& s : self_pre_saved) {
        memcpy(s.addr, &s.raw, 4);
    }
    // LGUI stores the canvas aspect ratio in its own object (self+0xb4 and self+0x104 == hmd_w/hmd_h, found via the diff
    // below). It stays at the eye aspect even when every rect in the view reads 3840x2160 during the call, so it comes from
    // elsewhere (projection / render target). Overwrite it with the ui_target aspect after the call; the render-thread draw
    // reads it from here.
    constexpr bool LGUI_PATCH_SELF_ASPECT = true;
    constexpr uint32_t LGUI_SELF_ASPECT_OFFS[] = {0xb4, 0x104};
    if (LGUI_PATCH_SELF_ASPECT && VR::get()->is_hmd_active() && self != nullptr && !IsBadReadPtr(self, 0x200) &&
        g_hook != nullptr && g_hook->get_render_target_manager() != nullptr)
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
        const auto hw = (float)VR::get()->get_hmd_width();
        const auto hh = (float)VR::get()->get_hmd_height();
        if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60) && hw > 0.0f && hh > 0.0f) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
            if (ui_w >= 64 && ui_h >= 64) {
                const float eye_aspect = hw / hh;
                const float wide_aspect = (hw * 2.0f) / hh;
                const float ui_aspect = (float)ui_w / (float)ui_h;
                std::string done{};
                for (auto off : LGUI_SELF_ASPECT_OFFS) {
                    auto& f = *(float*)((uintptr_t)self + off);
                    if (std::fabs(f - eye_aspect) <= eye_aspect * 1e-4f || std::fabs(f - wide_aspect) <= wide_aspect * 1e-4f) {
                        f = ui_aspect;
                        done += fmt::format("{:x} ", off);
                    }
                }
                static std::string last_done{"?"};
                if (done != last_done) {
                    last_done = done;
                    SPDLOG_INFO("[LGUI_GT] self aspect patched at [{}] eye={:.4f} -> ui={:.4f}", done.empty() ? "-" : done, eye_aspect, ui_aspect);
                }
            }
        }
    }

    // Diff self before/after: whatever LGUI stores from this call (canvas size, ortho matrix, viewport) lives here and is what
    // the render thread later reads. Log ints/floats that changed and any dimension-like ints / hmd-derived floats present.
    // Also: the sibling LGUI object self[1] (vtable 26ca0760, the canvas/interaction class) held hmd_w at 0x118/0x128/0x158/
    // 0x168/0x178/0x198 in an earlier dump. Scan self and every object self points to (one hop) for hw/hh ints and patch them.
    constexpr bool LGUI_PATCH_SELF_TREE_SIZES = true;
    if (LGUI_PATCH_SELF_TREE_SIZES && VR::get()->is_hmd_active() && self != nullptr && !IsBadReadPtr(self, 0x800) &&
        g_hook != nullptr && g_hook->get_render_target_manager() != nullptr)
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
        const auto hw = (int32_t)VR::get()->get_hmd_width();
        const auto hh = (int32_t)VR::get()->get_hmd_height();
        if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60) && hw > 0 && hh > 0) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);
            if (ui_w >= 64 && ui_h >= 64) {
                std::string done{};
                auto patch_obj = [&](uintptr_t obj, uint32_t len, const std::string& tag) {
                    if (obj < 0x10000 || IsBadReadPtr((void*)obj, len)) return;
                    for (uint32_t off = 0; off + 4 <= len; off += 4) {
                        auto p = (int32_t*)(obj + off);
                        if (*p == hw) { *p = ui_w; done += fmt::format("{}+{:x}=w ", tag, off); }
                        else if (*p == hh) { *p = ui_h; done += fmt::format("{}+{:x}=h ", tag, off); }
                        else if (*p == hw * 2) { *p = ui_w; done += fmt::format("{}+{:x}=2w ", tag, off); }
                    }
                };

                // PERF: the full self-tree scan below used to run every call - walking up to 64 child pointers
                // with an IsBadReadPtr(0x400) probe on EACH ONE (a syscall/SEH-guarded probe, not cheap), plus a
                // 0x800-byte and up-to-64x 0x400-byte int scan - unconditionally, once per eye, every single frame.
                // That is the dominant CPU cost that was starving the GPU (CPU can't queue work fast enough ->
                // GPU idles waiting -> GPU usage drops even though render work is unchanged). The set of child
                // pointers that are valid/worth scanning is stable for a given `self` and HMD resolution, so
                // resolve it once and reuse the cached indices on every subsequent frame; only redo the expensive
                // walk when `self` changes (new LGUI instance) or the HMD resolution changes.
                static const void* cached_self = nullptr;
                static int32_t cached_hw = -1, cached_hh = -1;
                static std::vector<int> cached_children{};

                const bool need_rescan = cached_self != self || cached_hw != hw || cached_hh != hh;
                if (need_rescan) {
                    cached_self = self;
                    cached_hw = hw;
                    cached_hh = hh;
                    cached_children.clear();
                    for (int j = 1; j < 64; ++j) {
                        const auto q = ((uintptr_t*)self)[j];
                        if (q < 0x10000 || (q & 7) != 0 || q == (uintptr_t)self || IsBadReadPtr((void*)q, 0x400)) continue;
                        cached_children.push_back(j);
                    }
                }

                patch_obj((uintptr_t)self, 0x800, "self");
                for (const auto j : cached_children) {
                    const auto q = ((uintptr_t*)self)[j];
                    if (q < 0x10000 || (q & 7) != 0 || q == (uintptr_t)self) continue;
                    patch_obj(q, 0x400, fmt::format("self[{}]", j));
                }
                static std::string last_done{"?"};
                if (done != last_done) {
                    last_done = done;
                    SPDLOG_INFO("[LGUI_GT] self-tree hw/hh ints patched: [{}] (hmd={}x{} -> {}x{})", done.empty() ? "-" : done, hw, hh, ui_w, ui_h);
                }
            }
        }

        // Targeted raw dump of self[1] (LGUI canvas/interaction object, vtable 26ca0760) at the specific offsets an
        // earlier dump found holding hmd_w (0x118/0x128/0x158/0x168/0x178/0x198). The generic hw/hh-int scanner above
        // never reports these as patched, meaning they don't currently hold exact hw/hh ints - dump their raw
        // int/float interpretation every time the values change so we can see what they actually hold (design
        // resolution, scale factor, or a derived/scaled value) and find the real field(s) to pin.
        constexpr bool LGUI_DUMP_SELF1_CANVAS = true;
        constexpr uint32_t LGUI_SELF1_CANVAS_OFFS[] = {0x118, 0x128, 0x158, 0x168, 0x178, 0x198};
        if (LGUI_DUMP_SELF1_CANVAS && self != nullptr && !IsBadReadPtr(self, 0x10)) {
            const auto self1 = ((uintptr_t*)self)[1];
            if (self1 > 0x10000 && (self1 & 7) == 0 && !IsBadReadPtr((void*)self1, 0x200)) {
                std::string dump{};
                for (auto off : LGUI_SELF1_CANVAS_OFFS) {
                    const auto iv = *(int32_t*)(self1 + off);
                    const auto fv = *(float*)(self1 + off);
                    dump += fmt::format("{:x}=[i={} f={:.4f}] ", off, iv, fv);
                }
                // Log whenever the HMD resolution changes (not just when the dump values change) so we can tell
                // apart "field never changes across resolution" from "field changed but we deduped the log line".
                static int32_t last_hw = -1, last_hh = -1;
                if ((int32_t)hw != last_hw || (int32_t)hh != last_hh) {
                    last_hw = (int32_t)hw;
                    last_hh = (int32_t)hh;
                    SPDLOG_INFO("[LGUI_GT] self[1] canvas raw ({:x}): {} (hmd={}x{})", self1, dump, hw, hh);

                    // Full dimension-like-int scan every time resolution changes too, so we can see which fields
                    // in self[1] track the new hmd size versus which stay fixed.
                    std::string dims{};
                    for (uint32_t off = 0; off + 4 <= 0x200; off += 4) {
                        const auto v = *(int32_t*)(self1 + off);
                        if (v >= 64 && v <= 16384) dims += fmt::format("{:x}={} ", off, v);
                    }
                    SPDLOG_INFO("[LGUI_GT] self[1] dimension-like ints (hmd={}x{}): {}", hw, hh, dims);
                }
            }
        }
    }

    if (LGUI_DIAG_STEADY_STATE && (k < 3 || (k % 1200) == 0)) {
        if (self != nullptr && !IsBadReadPtr(self, 0x800)) {
            static uint32_t before[0x200]{};
            // snapshot taken lazily: compare against the previous call's post-state (self is persistent)
            std::string changed{}, dims{}, fl{};
            const auto hw = (float)VR::get()->get_hmd_width();
            const auto hh = (float)VR::get()->get_hmd_height();
            const float cands[] = {hw, hh, hw / hh, hh / hw, 1.0f / hw, 1.0f / hh, 2.0f / hw, 2.0f / hh, 3840.0f, 2160.0f, 3840.0f / 2160.0f, 2.0f / 3840.0f, 2.0f / 2160.0f};
            const char* names[] = {"hw", "hh", "hw/hh", "hh/hw", "1/hw", "1/hh", "2/hw", "2/hh", "UW", "UH", "UW/UH", "2/UW", "2/UH"};
            for (uint32_t off = 0; off < 0x800; off += 4) {
                const auto u = *(uint32_t*)((uintptr_t)self + off);
                const auto iv = (int32_t)u;
                const auto fv = *(float*)&u;
                if (before[off / 4] != u) changed += fmt::format("{:x} ", off);
                before[off / 4] = u;
                if (iv >= 64 && iv <= 16384) dims += fmt::format("{:x}={} ", off, iv);
                for (int c = 0; c < 13; ++c) {
                    if (std::fabs(fv - cands[c]) <= std::fabs(cands[c]) * 1e-4f && cands[c] != 0.0f) { fl += fmt::format("{:x}={} ", off, names[c]); break; }
                }
            }
            SPDLOG_INFO("[LGUI_GT] self after SetupView #{}: changed=[{}] dims=[{}] floats=[{}]", k, changed, dims, fl);
        }
    }

    // Restore immediately - these are the real engine view/family rects (see comment above patches).
    lgui_restore_view_rects(patches);
}

static void lgui_begin_family_hook(void* self, void* family) {
    const auto vtable = *(uintptr_t*)self;
    const auto it = g_lgui_begin_family_originals.find(vtable);
    if (it == g_lgui_begin_family_originals.end()) return;

    static uint32_t n = 0;
    const auto k = n++;

    // Patch/restore synchronously within this call - family is the real FSceneViewFamily also used by the actual
    // 3D scene render (see comment in lgui_setup_view_hook for why deferring this across frames was reverted).
    std::vector<LguiRectPatch> patches{};
    std::string where{};

    if (LGUI_PATCH_GAME_THREAD_RECTS && VR::get()->is_hmd_active() && family != nullptr && !IsBadReadPtr(family, 0x400)) {
        lgui_patch_view_rects((uintptr_t)family, 0x400, "family", patches, where);

        // Views array: TArray<const FSceneView*> at family+0x0 in stock UE (data, count, max)
        const auto views = ((sdk::FSceneViewFamily*)family)->get_views();
        if (views != nullptr && !IsBadReadPtr(views, sizeof(*views)) && views->count > 0 && views->count <= 4 && views->data != nullptr && !IsBadReadPtr(views->data, sizeof(void*) * views->count)) {
            for (int32_t i = 0; i < views->count; ++i) {
                lgui_patch_view_rects((uintptr_t)views->data[i], 0x1000, fmt::format("view{}", i).c_str(), patches, where);
            }
        }
    }

    static size_t last_count = SIZE_MAX;
    if (k < 5 || patches.size() != last_count || (k % 600) == 0) {
        last_count = patches.size();
        SPDLOG_INFO("[LGUI_GT] BeginRenderViewFamily #{} self={:x} family={:x} rects patched={} at [{}] (hmd={}x{} nsf={})",
            k, (uintptr_t)self, (uintptr_t)family, patches.size(), where.empty() ? "-" : where,
            VR::get()->get_hmd_width(), VR::get()->get_hmd_height(), VR::get()->is_native_stereo_fix_enabled());
    }

    ((void(*)(void*, void*))it->second)(self, family);

    lgui_restore_view_rects(patches);
}

// LGUI's screen-space draw is a TRDGLambdaPass (vtable rva 276032d0, found by diffing the FRDGBuilder pass registry at
// a2+0x378 / arr+0x1168 across the slot-24 call). Slot 1 of that vtable is Execute(FRHIComputeCommandList&).
// We hook it so the RHI swap on the ViewFamilyTexture FRDGTexture can be undone right after LGUI has drawn.
// NOTE: multiple distinct TRDGLambdaPass closures (distinct C++ lambda types -> distinct vtables) can exist; the
// hardcoded static guess at 0x276032d0 turned out to be a generic/decoy lambda pass with no captured state, not
// LGUI's real UI draw. Track hooked vtables in a map (not a single global) so hooking the decoy does not
// permanently block discovering and hooking the real pass vtable via the pass-registry-growth diagnostic below.
static std::unordered_map<uintptr_t, uintptr_t> g_lgui_pass_originals{};
static std::mutex g_lgui_pass_originals_mutex{};
static uintptr_t g_lgui_pass_vtable = 0; // last vtable hooked, diagnostic only
static uintptr_t g_lgui_pass_execute_original = 0; // set when the FIRST (decoy) vtable hooks; kept for existing gates
static constexpr int32_t LGUI_PASS_EXECUTE_SLOT = 1;

struct LguiSwapState {
    void* rdg_texture{nullptr};
    void* shadow_copy{nullptr};
    void* original_rhi{nullptr};
    void* ui_target{nullptr};
    uintptr_t rdg_texture_vtable{0};
    bool armed{false};
    bool pass_seen_this_frame{false};
};
static LguiSwapState g_lgui_swap{};

// Diagnostic only, kept deliberately cheap: dump the LGUI pass object (lambda captures live inline in TRDGLambdaPass) and
// flag any qword equal to the textures we know about. No recursion / deep scanning: that stalled the render thread.
static bool lgui_copy_pass_seh(void* pass, uintptr_t* out, int count) {
    __try {
        for (int k = 0; k < count; ++k) {
            out[k] = ((uintptr_t*)pass)[k];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void lgui_dump_pass_seh(void* pass, std::string& raw, std::string& hits) {
    const auto real = (uintptr_t)g_lgui_swap.rdg_texture;
    const auto copy = (uintptr_t)g_lgui_swap.shadow_copy;
    const auto orig = (uintptr_t)g_lgui_swap.original_rhi;
    const auto ui = (uintptr_t)g_lgui_swap.ui_target;
    const auto rdg_vt = g_lgui_swap.rdg_texture_vtable;

    constexpr int COUNT = 96;
    uintptr_t q[COUNT]{};

    if (!lgui_copy_pass_seh(pass, q, COUNT)) {
        hits += "<fault>";
    }

    for (int k = 0; k < COUNT; ++k) {
        const auto v = q[k];
        raw += fmt::format("{:x},", v);
        if (v == real) hits += fmt::format("[{}]=REAL ", k);
        else if (copy != 0 && v == copy) hits += fmt::format("[{}]=COPY ", k);
        else if (orig != 0 && v == orig) hits += fmt::format("[{}]=ORIG_RHI ", k);
        else if (ui != 0 && v == ui) hits += fmt::format("[{}]=UI_RHI ", k);
        else if (v > 0x10000 && (v & 7) == 0 && !IsBadReadPtr((void*)v, 0x60)) {
            const auto vt = *(uintptr_t*)v;
            if (rdg_vt != 0 && vt == rdg_vt) {
                hits += fmt::format("[{}]=RDGTEX({:x} rhi={:x} ext={}x{}) ", k, v, *(uintptr_t*)(v + 0x10), *(int32_t*)(v + 0x54), *(int32_t*)(v + 0x58));
            } else if (orig != 0 && !IsBadReadPtr((void*)orig, 8) && vt == *(uintptr_t*)orig) {
                hits += fmt::format("[{}]=RHITEX({:x} ext={}x{}) ", k, v, *(int32_t*)(v + 0x54), *(int32_t*)(v + 0x58));
            }
        }
    }
}

// D3D12 command list viewport/scissor interception, active only while LGUI's pass Execute runs.
// NOTE: UE's RHI command list translation can run on a worker thread pool spawned from within Execute and joined
// before it returns, so a thread_local flag set on the calling thread would never be visible to the thread that
// actually issues RSSetViewports (this was tried first and never fired). Use a process-wide atomic counter instead;
// it is safe as long as the translation work is joined before lgui_pass_execute_hook's call to Execute returns,
// which is the standard synchronous-translation model.
static std::atomic<int32_t> g_lgui_in_execute_count{0};
static uintptr_t g_d3d12_cmdlist_vtable = 0;
static void (STDMETHODCALLTYPE* g_orig_rs_set_viewports)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*) = nullptr;
static void (STDMETHODCALLTYPE* g_orig_rs_set_scissor)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*) = nullptr;
static constexpr bool LGUI_WIDEN_D3D12_VIEWPORT = true;

// Master switch for the whole D3D12 viewport/scissor interception experiment. It hooks the SHARED
// ID3D12GraphicsCommandList vtable, so its hook functions run for EVERY RSSetViewports/RSSetScissorRects call in the
// entire renderer (main scene, shadows, post, UI) on every frame - a per-draw-call cost that lowers framerate
// independent of resolution. The experiment already established (via [LGUI_BOUNDS]) that LGUI's viewport is always the
// full 3840x2160 target and rewriting it never moved the painted region, so it is pure overhead now. Disabled by
// default; only flip this true when actively re-measuring the viewport LGUI submits.
// PROBE CONCLUDED (2026-09-05): every [LGUI_D3D] RSSetViewports was vp=(0,0 3840x2160) and gated=false - LGUI always
// submits the FULL target viewport (never eye-sized), and the execute-window gate never overlapped a viewport call, so
// LGUI_WIDEN_D3D12_VIEWPORT never fired. The viewport is NOT the clip: LGUI lays its canvas out at the game-thread
// per-eye view size (hmd_w x hmd_h) and paints top-left into the 3840x2160 target, so when hmd_h > 2160 the bottom is
// clipped. Disabled again - this shared-vtable hook is pure per-draw overhead with no benefit for this cause.
static constexpr bool LGUI_ENABLE_D3D12_VIEWPORT_HOOK = false;

static bool lgui_ui_target_size(int32_t& w, int32_t& h) {
    if (g_hook == nullptr || g_hook->get_render_target_manager() == nullptr) return false;
    const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
    if (ui_target == nullptr || IsBadReadPtr(ui_target, 0x60)) return false;
    w = *(int32_t*)((uintptr_t)ui_target + 0x54);
    h = *(int32_t*)((uintptr_t)ui_target + 0x58);
    return w >= 64 && h >= 64 && w <= 16384 && h <= 16384;
}

static std::atomic<uint64_t> g_rsviewport_total_calls{0};
static std::atomic<uint64_t> g_rsscissor_total_calls{0};

// DIAG: unconditional, low-rate heartbeat proving whether this hooked vtable receives ANY calls at all
// (from LGUI or plain 3D scene rendering, which calls RSSetViewports every view every frame). If this
// never increments across a whole play session, the hooked vtable is not the one the engine actually uses
// (e.g. a debug-layer wrapper, a different device/adapter, or a distinct command-list pool), independent of
// any LGUI-specific timing/gating question.
static void lgui_d3d12_heartbeat() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 3) {
        last = now;
        SPDLOG_INFO("[LGUI_D3D] heartbeat: total RSSetViewports={} total RSSetScissorRects={} (since hook install)",
            g_rsviewport_total_calls.load(std::memory_order_relaxed), g_rsscissor_total_calls.load(std::memory_order_relaxed));
    }
}

static void STDMETHODCALLTYPE lgui_rs_set_viewports_hook(ID3D12GraphicsCommandList* self, UINT count, const D3D12_VIEWPORT* vps) {
    const bool gated = g_lgui_in_execute_count.load(std::memory_order_relaxed) > 0;
    g_rsviewport_total_calls.fetch_add(1, std::memory_order_relaxed);
    lgui_d3d12_heartbeat();
    // DIAG: log the first N calls regardless of the gate, so we can tell whether RSSetViewports is called at all
    // and, separately, whether the execute-window gate lines up with it.
    static uint32_t total_n = 0;
    if (total_n++ < 20 && vps != nullptr && count >= 1) {
        SPDLOG_INFO("[LGUI_D3D] (ungated) RSSetViewports #{}: gated={} vp=({},{} {}x{})", total_n, gated, vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height);
    }

    if (gated && vps != nullptr && count >= 1) {
        static uint32_t n = 0;
        const auto k = n++;
        const auto hw = (float)VR::get()->get_hmd_width();
        const auto hh = (float)VR::get()->get_hmd_height();
        int32_t ui_w = 0, ui_h = 0;
        const bool have_ui = lgui_ui_target_size(ui_w, ui_h);

        static float last_w = -1.0f, last_h = -1.0f;
        if (k < 5 || vps[0].Width != last_w || vps[0].Height != last_h) {
            last_w = vps[0].Width;
            last_h = vps[0].Height;
            SPDLOG_INFO("[LGUI_D3D] RSSetViewports in LGUI Execute: n={} vp=({},{} {}x{}) hmd={}x{} ui={}x{}", count, vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height, hw, hh, ui_w, ui_h);
        }

        if (LGUI_WIDEN_D3D12_VIEWPORT && have_ui && count == 1) {
            const bool eye = std::fabs(vps[0].Width - hw) < 1.0f || std::fabs(vps[0].Width - hw * 2.0f) < 1.0f;
            if (eye) {
                D3D12_VIEWPORT vp = vps[0];
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width = (float)ui_w;
                vp.Height = (float)ui_h;
                g_orig_rs_set_viewports(self, 1, &vp);
                return;
            }
        }
    }
    g_orig_rs_set_viewports(self, count, vps);
}

static void STDMETHODCALLTYPE lgui_rs_set_scissor_hook(ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* rects) {
    const bool gated = g_lgui_in_execute_count.load(std::memory_order_relaxed) > 0;
    g_rsscissor_total_calls.fetch_add(1, std::memory_order_relaxed);
    if (gated && rects != nullptr && count >= 1) {
        static uint32_t n = 0;
        const auto k = n++;
        const auto hw = (LONG)VR::get()->get_hmd_width();
        const auto hh = (LONG)VR::get()->get_hmd_height();
        int32_t ui_w = 0, ui_h = 0;
        const bool have_ui = lgui_ui_target_size(ui_w, ui_h);

        static LONG last_r = -1, last_b = -1;
        if (k < 5 || rects[0].right != last_r || rects[0].bottom != last_b) {
            last_r = rects[0].right;
            last_b = rects[0].bottom;
            SPDLOG_INFO("[LGUI_D3D] RSSetScissorRects in LGUI Execute: n={} rect=({},{})-({},{}) hmd={}x{} ui={}x{}", count, rects[0].left, rects[0].top, rects[0].right, rects[0].bottom, hw, hh, ui_w, ui_h);
        }

        if (LGUI_WIDEN_D3D12_VIEWPORT && have_ui && count == 1) {
            const LONG w = rects[0].right - rects[0].left;
            if (w == hw || w == hw * 2) {
                D3D12_RECT r{0, 0, (LONG)ui_w, (LONG)ui_h};
                g_orig_rs_set_scissor(self, 1, &r);
                return;
            }
        }
    }
    g_orig_rs_set_scissor(self, count, rects);
}

static void lgui_try_hook_d3d12_cmdlist() {
    if (!LGUI_ENABLE_D3D12_VIEWPORT_HOOK) return;
    if (g_d3d12_cmdlist_vtable != 0) return;
    static bool logged_once = false;
    if (g_framework == nullptr || g_framework->is_dx11()) {
        if (!logged_once) { logged_once = true; SPDLOG_INFO("[LGUI_D3D] skip: framework_null={} is_dx11={}", g_framework == nullptr, g_framework != nullptr && g_framework->is_dx11()); }
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        if (!logged_once) { logged_once = true; SPDLOG_INFO("[LGUI_D3D] skip: d3d12_hook is null"); }
        return;
    }
    auto device = hook->get_device();
    if (device == nullptr) {
        if (!logged_once) { logged_once = true; SPDLOG_INFO("[LGUI_D3D] skip: device is null"); }
        return;
    }

    // Create a throwaway list to obtain the vtable (shared by every ID3D12GraphicsCommandList on this device).
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list{};
    HRESULT hr_alloc{}, hr_list{};
    if (FAILED(hr_alloc = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
        if (!logged_once) { logged_once = true; SPDLOG_INFO("[LGUI_D3D] skip: CreateCommandAllocator failed hr={:x}", (uint32_t)hr_alloc); }
        return;
    }
    if (FAILED(hr_list = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        if (!logged_once) { logged_once = true; SPDLOG_INFO("[LGUI_D3D] skip: CreateCommandList failed hr={:x}", (uint32_t)hr_list); }
        return;
    }
    list->Close();

    const auto vt = *(uintptr_t*)list.Get();
    // ID3D12GraphicsCommandList: IUnknown(3) + ID3D12Object(4) + ID3D12DeviceChild(1) + ID3D12CommandList(1) = 9,
    // then Close(9) Reset(10) ClearState(11) DrawInstanced(12) DrawIndexedInstanced(13) Dispatch(14) CopyBufferRegion(15)
    // CopyTextureRegion(16) CopyResource(17) CopyTiles(18) ResolveSubresource(19) IASetPrimitiveTopology(20)
    // RSSetViewports(21) RSSetScissorRects(22)
    constexpr int RS_SET_VIEWPORTS = 21;
    constexpr int RS_SET_SCISSOR = 22;

    auto vp_slot = (uintptr_t*)(vt + sizeof(uintptr_t) * RS_SET_VIEWPORTS);
    auto sc_slot = (uintptr_t*)(vt + sizeof(uintptr_t) * RS_SET_SCISSOR);
    DWORD old{};
    if (VirtualProtect(vp_slot, sizeof(uintptr_t) * 2, PAGE_READWRITE, &old)) {
        g_orig_rs_set_viewports = (decltype(g_orig_rs_set_viewports))*vp_slot;
        g_orig_rs_set_scissor = (decltype(g_orig_rs_set_scissor))*sc_slot;
        *vp_slot = (uintptr_t)&lgui_rs_set_viewports_hook;
        *sc_slot = (uintptr_t)&lgui_rs_set_scissor_hook;
        VirtualProtect(vp_slot, sizeof(uintptr_t) * 2, old, &old);
        g_d3d12_cmdlist_vtable = vt;
        SPDLOG_INFO("[LGUI_D3D] hooked ID3D12GraphicsCommandList RSSetViewports/RSSetScissorRects (vtable={:x})", vt);
    }
}

static void lgui_pass_execute_hook(void* pass, void* rhi_cmd_list) {
    static uint32_t exec_count = 0;
    const auto n = exec_count++;

    // Look up the original function for THIS pass's specific vtable (multiple distinct closure types share this
    // trampoline once each is hooked; using the global "first hooked" original would call the wrong function for
    // every vtable except the first one).
    uintptr_t orig_fn = 0;
    const auto this_vt = (pass != nullptr && !IsBadReadPtr(pass, sizeof(uintptr_t))) ? *(uintptr_t*)pass : 0;
    if (this_vt != 0) {
        std::scoped_lock lock{g_lgui_pass_originals_mutex};
        const auto it = g_lgui_pass_originals.find(this_vt);
        if (it != g_lgui_pass_originals.end()) {
            orig_fn = it->second;
        }
    }
    if (orig_fn == 0) {
        orig_fn = g_lgui_pass_execute_original; // fallback, should not normally happen
    }
    const auto og = (void(*)(void*, void*))orig_fn;

    if (n < 6 || (n % 1200) == 0) {
        std::string raw{}, hits{};
        lgui_dump_pass_seh(pass, raw, hits);
        SPDLOG_INFO("[LGUI_SWAP] execute: pass={:x} vt={:x} real={:x} copy={:x} orig_rhi={:x} refs: {}", (uintptr_t)pass, this_vt,
            (uintptr_t)g_lgui_swap.rdg_texture, (uintptr_t)g_lgui_swap.shadow_copy, (uintptr_t)g_lgui_swap.original_rhi, hits.empty() ? "<none>" : hits);
        SPDLOG_INFO("[LGUI_SWAP]   pass raw=[{}]", raw);
    }

    // LGUI_BOUNDS readback: the UI is painted into exactly (0,0)-(hmd_w,hmd_h) of ui_target regardless of every a2 patch
    // made at record time, so the viewport rect LGUI passes to RHISetViewport is captured by the pass lambda (by value or
    // through the FViewInfo pointer). Patch it here, right before Execute records the RHI commands, and restore afterwards.
    //   level 0: int32 (hw,hh) / (2hw,hh) pairs and (0,0,hw,hh) rects stored inline in the pass object
    //   level 1: the same patterns inside any object the pass points to (FViewInfo::UnscaledViewRect / ViewRect etc.)
    // RESULT: LGUI_BOUNDS bbox unchanged with these patches active -> the viewport is not reachable from the pass. Disabled.
    constexpr bool LGUI_PATCH_EXECUTE_RECTS = false;

    // Every int in the FViewInfo and FSceneView now reads 3840x2160 (residue scan empty) yet the paint stays hmd_w wide.
    // The one eye-sized object still reachable is the REAL ViewFamilyTexture (a3, Desc.Extent = hmd_w x hmd_h) through the
    // 11 a2 refs we leave on it (0x3d0+). If LGUI's Execute takes its RHISetViewport size from that texture's extent, patching
    // it for the duration of this Execute only (restored right after) gives it the ui_target size without touching any
    // other pass. The shadow copy already carries 3840x2160.
    // RESULT: by Execute time g_lgui_swap.rdg_texture is already freed (read back 0 x 0x01010101) -> unsafe, disabled.
    // The extent is patched at record time in lgui_slot24_hook instead.
    constexpr bool LGUI_PATCH_REAL_A3_EXTENT_DURING_EXECUTE = false;
    int32_t saved_ext[2]{};
    int32_t* real_ext = nullptr;

    if (LGUI_PATCH_REAL_A3_EXTENT_DURING_EXECUTE && g_lgui_swap.rdg_texture != nullptr && g_lgui_swap.ui_target != nullptr &&
        !IsBadReadPtr(g_lgui_swap.rdg_texture, 0x60) && !IsBadReadPtr(g_lgui_swap.ui_target, 0x60))
    {
        real_ext = (int32_t*)((uintptr_t)g_lgui_swap.rdg_texture + 0x54);
        const auto ui_w = *(int32_t*)((uintptr_t)g_lgui_swap.ui_target + 0x54);
        const auto ui_h = *(int32_t*)((uintptr_t)g_lgui_swap.ui_target + 0x58);
        if (ui_w >= 64 && ui_h >= 64 && ui_w <= 16384 && ui_h <= 16384) {
            saved_ext[0] = real_ext[0];
            saved_ext[1] = real_ext[1];
            real_ext[0] = ui_w;
            real_ext[1] = ui_h;
            static int32_t last_w = -1, last_h = -1;
            if (saved_ext[0] != last_w || saved_ext[1] != last_h) {
                last_w = saved_ext[0];
                last_h = saved_ext[1];
                SPDLOG_INFO("[LGUI_EXEC] real a3 extent {}x{} -> {}x{} for this Execute", saved_ext[0], saved_ext[1], ui_w, ui_h);
            }
        } else {
            real_ext = nullptr;
        }
    }
    std::vector<std::pair<int32_t*, int32_t>> saved{};
    std::string where{};

    // Ground truth: hook ID3D12GraphicsCommandList::RSSetViewports / RSSetScissorRects (vtable slots 21 / 22) for the
    // duration of LGUI's Execute. Whatever viewport LGUI sets is what clips the draw; log it and, if it is eye-sized,
    // widen it to the ui_target (the render target bound at that moment is our 3840x2160 copy).
    // Use an atomic counter, not a thread_local flag: UE can translate RHI commands into real D3D12 calls on a worker
    // thread pool that is not the thread calling Execute, so a thread_local gate would never see those calls.
    // Only maintain the gate/install the hook when the (disabled-by-default) viewport experiment is enabled - the
    // shared-vtable hook is otherwise pure per-draw-call overhead across the whole renderer.
    if (LGUI_ENABLE_D3D12_VIEWPORT_HOOK) {
        g_lgui_in_execute_count.fetch_add(1, std::memory_order_relaxed);
        lgui_try_hook_d3d12_cmdlist();
    }

    og(pass, rhi_cmd_list);

    if (LGUI_ENABLE_D3D12_VIEWPORT_HOOK) {
        g_lgui_in_execute_count.fetch_sub(1, std::memory_order_relaxed);
    }

    if (LGUI_PATCH_EXECUTE_RECTS && g_hook != nullptr && g_hook->get_render_target_manager() != nullptr && pass != nullptr && !IsBadReadPtr(pass, 0x300)) {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();
        const auto hw = (int32_t)VR::get()->get_hmd_width();
        const auto hh = (int32_t)VR::get()->get_hmd_height();

        if (ui_target != nullptr && !IsBadReadPtr(ui_target, 0x60) && hw > 0 && hh > 0) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + 0x54);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + 0x58);

            if (ui_w >= 64 && ui_w <= 16384 && ui_h >= 64 && ui_h <= 16384) {
                auto patch_block = [&](uintptr_t base, uint32_t len, const char* tag) {
                    for (uint32_t off = 0; off + 8 <= len; off += 4) {
                        auto p = (int32_t*)(base + off);
                        const bool pair = (p[0] == hw || p[0] == hw * 2) && p[1] == hh;
                        if (!pair) continue;

                        // Rect form: (0,0,w,h) -> patch max only. Size form: (w,h).
                        saved.emplace_back(p, p[0]);
                        saved.emplace_back(p + 1, p[1]);
                        p[0] = ui_w;
                        p[1] = ui_h;
                        where += fmt::format("{}+{:x}{} ", tag, off, off >= 8 && p[-1] == 0 && p[-2] == 0 ? "(rect)" : "");
                        off += 4;
                    }
                };

                patch_block((uintptr_t)pass, 0x300, "pass");

                for (int k = 0; k < 96; ++k) {
                    const auto v = ((uintptr_t*)pass)[k];
                    if (v < 0x10000 || (v & 7) != 0 || v == (uintptr_t)pass || IsBadReadPtr((void*)v, 0x1000)) continue;
                    if (v == (uintptr_t)g_lgui_swap.rdg_texture || v == (uintptr_t)g_lgui_swap.shadow_copy) continue;
                    patch_block(v, 0x1000, fmt::format("[{}]", k).c_str());
                }

                static size_t last_count = SIZE_MAX;
                static int32_t last_hw = 0, last_hh = 0;
                if (n < 6 || saved.size() / 2 != last_count || hw != last_hw || hh != last_hh) {
                    last_count = saved.size() / 2;
                    last_hw = hw;
                    last_hh = hh;
                    SPDLOG_INFO("[LGUI_EXEC] patched {} size/rect entries (hmd={}x{} -> {}x{}) at [{}]", saved.size() / 2, hw, hh, ui_w, ui_h, where.empty() ? "-" : where);
                }
            }
        }
    }

    if (real_ext != nullptr) {
        real_ext[0] = saved_ext[0];
        real_ext[1] = saved_ext[1];
    }

    for (auto& [p, v] : saved) *p = v;
}

static void lgui_try_hook_pass_vtable(uintptr_t pass) {
    if (pass < 0x10000 || IsBadReadPtr((void*)pass, sizeof(uintptr_t))) {
        return;
    }

    const auto vt = *(uintptr_t*)pass;
    if (IsBadReadPtr((void*)vt, sizeof(uintptr_t) * (LGUI_PASS_EXECUTE_SLOT + 1))) {
        return;
    }

    {
        std::scoped_lock lock{g_lgui_pass_originals_mutex};
        if (g_lgui_pass_originals.find(vt) != g_lgui_pass_originals.end()) {
            return; // this specific vtable is already hooked
        }
    }

    const auto m = utility::get_module_within((void*)vt);
    if (!m.has_value() || *m != utility::get_executable()) {
        return;
    }

    auto slot_ptr = (uintptr_t*)(vt + sizeof(uintptr_t) * LGUI_PASS_EXECUTE_SLOT);
    const auto fn = *slot_ptr;
    if (fn == 0 || !utility::get_module_within((void*)fn).has_value()) {
        return;
    }

    DWORD old{};
    if (VirtualProtect(slot_ptr, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
        {
            std::scoped_lock lock{g_lgui_pass_originals_mutex};
            g_lgui_pass_originals[vt] = fn;
        }
        g_lgui_pass_vtable = vt;
        g_lgui_pass_execute_original = fn;
        *slot_ptr = (uintptr_t)&lgui_pass_execute_hook;
        VirtualProtect(slot_ptr, sizeof(uintptr_t), old, &old);
        SPDLOG_INFO("[LGUI_SWAP] hooked pass Execute: vtable rva={:x} slot{} fn rva={:x} (total hooked vtables={})", vt - (uintptr_t)*m, LGUI_PASS_EXECUTE_SLOT, fn - (uintptr_t)*m, g_lgui_pass_originals.size());
    }
}

static void* lgui_slot24_hook(void* self, void* a2, void* a3, void* a4, void* a5, void* a6) {
    const auto vtable = *(uintptr_t*)self;
    const auto it = g_lgui_slot24_originals.find(vtable);

    if (it == g_lgui_slot24_originals.end()) {
        return nullptr;
    }

    // TRIGGER: any invocation of this hook means LGUI's RDG draw pass exists and is executing this
    // frame. The user has observed the LGUI-driven UI/quad go fully blank during certain skill/VFX
    // animations and return once the animation ends - when the UI is blanked, LGUI produces no draw
    // pass and this hook simply stops being called for that window. That makes "time since this hook
    // last fired" a much more direct signal for "is a skill/VFX animation currently playing" than
    // bCinematicMode, which only reflects Sequencer/Matinee cutscenes and never fired for this case.
    // See report_lgui_draw_heartbeat()/get_lgui_draw_silence_duration_ms().
    if (g_hook != nullptr) {
        g_hook->report_lgui_draw_heartbeat();
    }

    static uint32_t call_count = 0;
    const auto n = call_count++;

    // ==== LGUI DIAGNOSTIC TOGGLE ====================================================================
    // Master switch for the EXPENSIVE per-draw LGUI diagnostics on the render thread: the a2 eye-size
    // "residue" scan (walks up to 0x2000 bytes every qualifying draw building a big std::string) and the
    // verbose [LGUI_REFS] summary logging. These are investigation-only and are a confirmed lag source
    // (string formatting + scans on the render thread). Leave FALSE for normal play; flip to TRUE only
    // when actively re-probing LGUI's size sources. The functional ref redirect/patch logic below is NOT
    // gated by this - only the logging/scanning is.
    constexpr bool LGUI_DIAG_VERBOSE = false;

    // NOTE: earlier speculative "pass registry" hunting at a2+0x378 -> owner+0x1168 was removed. Confirmed dead
    // end: real diagnostics mapped a2's actual layout (see below) and it is not an FRDGBuilder / pass array at
    // that offset - the "num" values read there were garbage (huge/negative), never valid TArray counts.
    // Ground-truth layout of a2 (per-view render-thread state block, ~0x8000 bytes, NOT FRDGBuilder/FSceneView):
    //   +0/+4         packed (1/w, 1/h) floats of the eye view (same as a5)
    //   +0x3e0/+0x410/+0x4a0/+0x4f8  eye view rects (0,0,hw,hh); patching changes layout but not bound texture
    //   +0x778/+0xe08 further (hw,hh) pairs; +0x66c lone hw; patching these changed nothing visible
    //   +0x270/+0x348/+0x3a0  pointers to a3 - these ARE what LGUI's Execute pass dereferences to find its
    //                         render target. This is the real, working redirect point (used below).
    //   +0x3d0/+0x400/+0x4e8  also pointers to a3, read by another pass (menu background blur); redirecting
    //                         these crashes when a menu opens - leave them on the real texture.
    //   +0x12d0..+0x2b38      8 more a3 refs, not needed for the redirect, left alone.
    // a3 is the FRDGTexture for ViewFamilyTexture (shared stereo scene output); +0x10 RHI resource, +0x54/+0x58
    // Desc.Extent. Swapping a3's RHI pointer redirects the whole scene (too broad); passing a copy as a3 does
    // nothing (LGUI doesn't use the argument itself, only the a2 refs above).

    // Hook the LGUI TRDGLambdaPass::Execute up front (vtable exe+276032d0, slots 228bc610/228bde10 observed in the
    // registry diff log) so the execute-time diagnostics run regardless of which record-time path returns below.
    if (g_lgui_pass_execute_original == 0) {
        static bool tried_static = false;
        if (!tried_static) {
            tried_static = true;
            const auto exe = (uintptr_t)utility::get_executable();
            const auto vt = exe + 0x276032d0;
            if (!IsBadReadPtr((void*)vt, sizeof(uintptr_t) * 4)) {
                const auto s0 = ((uintptr_t*)vt)[0], s1 = ((uintptr_t*)vt)[1];
                const auto m0 = utility::get_module_within((void*)s0), m1 = utility::get_module_within((void*)s1);
                SPDLOG_INFO("[LGUI_SWAP] static vtable check: vt={:x} slot0 rva={:x} slot1 rva={:x}", vt,
                    m0.has_value() ? s0 - exe : 0, m1.has_value() ? s1 - exe : 0);
                if (m0.has_value() && m1.has_value() && s0 - exe == 0x228bc610 && s1 - exe == 0x228bde10) {
                    uintptr_t fake_pass = vt;
                    lgui_try_hook_pass_vtable((uintptr_t)&fake_pass);
                }
            }
        }
    }

    // PERF: this large per-draw decode/dump block (module lookups, IsBadReadPtr walks, fmt::format string
    // building) is investigation-only. It was running unconditionally every 600 draws (and the first 40),
    // adding steady-state render-thread cost even in normal play. Gate it behind LGUI_DIAG_VERBOSE so it is
    // fully compiled out of the hot path unless actively re-probing.
    if (LGUI_DIAG_VERBOSE && (n < 40 || (n % 600) == 0)) {
        const auto module = utility::get_module_within((void*)vtable);
        const auto vrva = module.has_value() ? vtable - (uintptr_t)*module : vtable;
        const auto ret = (uintptr_t)_ReturnAddress();
        const auto ret_mod = utility::get_module_within((void*)ret);

        // Hypothesis A: a3 is FSceneViewFamily (PostRenderViewFamily_RenderThread). Validate via views[k]->Family == a3.
        // Hypothesis B: a3 is FSceneView (PostRenderView_RenderThread); its first member is Family.
        std::string decoded{};

        if (a3 != nullptr && !IsBadReadPtr(a3, 0x100)) {
            const auto fam = (sdk::FSceneViewFamily*)a3;
            const auto views = fam->get_views();

            if (views != nullptr && !IsBadReadPtr(views, sizeof(*views)) && views->count > 0 && views->count <= 4 && views->data != nullptr && !IsBadReadPtr(views->data, sizeof(void*) * views->count)) {
                bool ok = true;
                std::string vs{};
                for (int32_t k = 0; k < views->count; ++k) {
                    const auto v = (uintptr_t)views->data[k];
                    if (v == 0 || IsBadReadPtr((void*)v, 0xB00) || *(uintptr_t*)v != (uintptr_t)a3) { ok = false; break; }
                    vs += fmt::format("{:x}(pass={}),", v, *(uint32_t*)(v + 0xAF0));
                }
                if (ok) {
                    decoded = fmt::format("a3=FAMILY nviews={} views=[{}]", views->count, vs);
                }
            }

            if (decoded.empty() && !IsBadReadPtr(a3, 0xB00)) {
                const auto fam2 = *(uintptr_t*)a3;
                if (fam2 != 0 && !IsBadReadPtr((void*)fam2, 0x100)) {
                    const auto views2 = ((sdk::FSceneViewFamily*)fam2)->get_views();
                    int32_t idx = -1;
                    if (views2 != nullptr && !IsBadReadPtr(views2, sizeof(*views2)) && views2->count > 0 && views2->count <= 4) {
                        for (int32_t k = 0; k < views2->count; ++k) {
                            if ((uintptr_t)views2->data[k] == (uintptr_t)a3) { idx = k; break; }
                        }
                    }
                    if (idx >= 0) {
                        decoded = fmt::format("a3=VIEW family={:x} nviews={} view_index={} pass={}", fam2, views2->count, idx, *(uint32_t*)((uintptr_t)a3 + 0xAF0));
                    }
                }
            }
        }

        if (decoded.empty()) {
            decoded = "a3=UNKNOWN";
        }

        // Raw dumps so we can identify the arg types by hand (stereo pass 0xAF0 for views, TArray<FSceneView*> for families)
        auto dump_qwords = [](void* p, int count) -> std::string {
            std::string s{};
            if (p == nullptr || IsBadReadPtr(p, sizeof(uintptr_t) * count)) return "<bad>";
            for (int k = 0; k < count; ++k) s += fmt::format("{:x},", ((uintptr_t*)p)[k]);
            return s;
        };
        auto probe_view = [](void* p) -> std::string {
            if (p == nullptr || IsBadReadPtr(p, 0xB00)) return "<bad>";
            return fmt::format("pass@af0={} fam@0={:x}", *(uint32_t*)((uintptr_t)p + 0xAF0), *(uintptr_t*)p);
        };

        if (n < 6) {
            // a3/a4 share a vtable (16759ec60) and look like FRDGResource: [vtable, const TCHAR* Name, ...]
            auto rdg_name = [](void* p) -> std::string {
                if (p == nullptr || IsBadReadPtr(p, 0x10)) return "<bad>";
                const auto name = *(wchar_t**)((uintptr_t)p + 8);
                if (name == nullptr || IsBadReadPtr(name, 64)) return "<noname>";
                std::wstring w{name, wcsnlen(name, 64)};
                return utility::narrow(w);
            };
            SPDLOG_INFO("[LGUI_DRAW]   a3 raw=[{}] {} name=\"{}\"", dump_qwords(a3, 12), probe_view(a3), rdg_name(a3));
            SPDLOG_INFO("[LGUI_DRAW]   a4 raw=[{}] {} name=\"{}\"", dump_qwords(a4, 12), probe_view(a4), rdg_name(a4));
            // a5 is packed data (two floats: looks like 1/w, 1/h of the target), a6 == 0
            const auto a5v = (uintptr_t)a5;
            float a5f[2]{};
            memcpy(a5f, &a5v, sizeof(a5f));
            SPDLOG_INFO("[LGUI_DRAW]   a5={:x} as_floats=({}, {}) inv=({}, {}) a6={:x}", a5v, a5f[0], a5f[1],
                a5f[0] != 0.0f ? 1.0f / a5f[0] : 0.0f, a5f[1] != 0.0f ? 1.0f / a5f[1] : 0.0f, (uintptr_t)a6);

            // LGUI gets no FSceneView here, so its viewport/projection must come from its own state: dump the extension object
            if (n < 2) {
                SPDLOG_INFO("[LGUI_DRAW]   self[0..23]=[{}]", dump_qwords(self, 24));
                SPDLOG_INFO("[LGUI_DRAW]   self[24..47]=[{}]", dump_qwords((void*)((uintptr_t)self + 24 * 8), 24));
                SPDLOG_INFO("[LGUI_DRAW]   self[48..71]=[{}]", dump_qwords((void*)((uintptr_t)self + 48 * 8), 24));

                // self+0x38.. looks like float matrices (1.0/-1.0/100.0/1280.x): decode as floats
                std::string fl{};
                for (uint32_t off = 0x38; off < 0x110; off += 4) {
                    fl += fmt::format("{:x}={:.3f} ", off, *(float*)((uintptr_t)self + off));
                }
                SPDLOG_INFO("[LGUI_DRAW]   self floats: {}", fl);

                // Find where LGUI caches the viewport/canvas size: scan for plausible pixel sizes (ints/floats in [600, 8192])
                std::string sizes{};
                for (uint32_t off = 0; off < 0x800; off += 4) {
                    if (IsBadReadPtr((void*)((uintptr_t)self + off), 4)) break;
                    const auto iv = *(int32_t*)((uintptr_t)self + off);
                    const auto fv = *(float*)((uintptr_t)self + off);
                    if (iv >= 600 && iv <= 8192) sizes += fmt::format("{:x}=i{} ", off, iv);
                    else if (fv >= 600.0f && fv <= 8192.0f && std::isfinite(fv)) sizes += fmt::format("{:x}=f{:.1f} ", off, fv);
                }
                SPDLOG_INFO("[LGUI_DRAW]   self size-like values: {}", sizes);
                SPDLOG_INFO("[LGUI_DRAW]   hmd={}x{} (a5 is constant across resolutions -> likely NOT the RT size)", VR::get()->get_hmd_width(), VR::get()->get_hmd_height());

                // FRDGTexture desc: dump 32 dwords of a3 to find Extent (w,h ints), and compare with a4
                auto dump_dwords = [](void* p, int count) -> std::string {
                    std::string s{};
                    if (p == nullptr || IsBadReadPtr(p, sizeof(uint32_t) * count)) return "<bad>";
                    for (int k = 0; k < count; ++k) {
                        const auto v = ((uint32_t*)p)[k];
                        if (v >= 64 && v <= 16384) s += fmt::format("[{}]={} ", k, v);
                    }
                    return s;
                };
                SPDLOG_INFO("[LGUI_DRAW]   a3 int-like dwords: {}", dump_dwords(a3, 64));
                SPDLOG_INFO("[LGUI_DRAW]   a4 int-like dwords: {}", dump_dwords(a4, 64));

                // ViewFamilyTexture extent is 2*hmd_w x hmd_h yet LGUI's layout doesn't follow it. Find where LGUI caches
                // its viewport size: scan objects pointed to by self (2 levels) for any pixel-size-like dwords [600, 8192].
                auto scan_sizes = [](uintptr_t p, uint32_t len) -> std::string {
                    std::string hits{};
                    for (uint32_t off = 0; off < len; off += 4) {
                        const auto v = *(uint32_t*)(p + off);
                        if (v >= 600 && v <= 8192) hits += fmt::format("{:x}={} ", off, v);
                    }
                    return hits;
                };
                for (int k = 1; k < 24; ++k) {
                    const auto p = ((uintptr_t*)self)[k];
                    if (p < 0x10000 || IsBadReadPtr((void*)p, 0x400)) continue;
                    const auto pvt = *(uintptr_t*)p;
                    const auto pm = utility::get_module_within((void*)pvt);
                    const auto hits = scan_sizes(p, 0x400);
                    SPDLOG_INFO("[LGUI_DRAW]   self[{}]={:x} vt_rva={:x} sizes: {}", k, p, pm.has_value() ? pvt - (uintptr_t)*pm : 0, hits.empty() ? "<none>" : hits);

                    for (int j = 0; j < 32; ++j) {
                        const auto q = ((uintptr_t*)p)[j];
                        if (q < 0x10000 || IsBadReadPtr((void*)q, 0x200)) continue;
                        const auto h2 = scan_sizes(q, 0x200);
                        if (!h2.empty()) SPDLOG_INFO("[LGUI_DRAW]     self[{}][{}]={:x} sizes: {}", k, j, q, h2);
                    }
                }

                // FRDGTexture: [2] should be the pooled RT / RHI resource. Compare against UEVR's targets.
                auto describe_rhi = [&](void* rdg) -> std::string {
                    if (rdg == nullptr || IsBadReadPtr(rdg, 0x40)) return "<bad>";
                    const auto p2 = ((uintptr_t*)rdg)[2];
                    std::string s = fmt::format("[2]={:x}", p2);
                    if (p2 != 0 && !IsBadReadPtr((void*)p2, 0x60)) {
                        const auto vt = *(uintptr_t*)p2;
                        const auto m = utility::get_module_within((void*)vt);
                        s += fmt::format(" vt_rva={:x} q=[{}]", m.has_value() ? vt - (uintptr_t)*m : vt, dump_qwords((void*)p2, 12));
                    }
                    return s;
                };
                const auto ui_target = g_hook != nullptr && g_hook->get_render_target_manager() != nullptr ? (uintptr_t)g_hook->get_render_target_manager()->get_ui_target() : 0;
                SPDLOG_INFO("[LGUI_DRAW]   a3 {} | ui_target={:x}", describe_rhi(a3), ui_target);
                SPDLOG_INFO("[LGUI_DRAW]   a4 {}", describe_rhi(a4));

                // Stock LGUI takes the viewport from FSceneView::UnscaledViewRect in PostRenderView_RenderThread(FRDGBuilder&, FSceneView&).
                // self holds only the design resolution (1280x768) and a3/a4 are RDG textures, so a2 is the only remaining
                // candidate for the view (or GraphBuilder). Dump it: FSceneView has Family at +0, view rects as int32 pairs.
                if (a2 != nullptr && !IsBadReadPtr(a2, 0x1000)) {
                    const auto a2vt = *(uintptr_t*)a2;
                    const auto a2m = utility::get_module_within((void*)a2vt);
                    SPDLOG_INFO("[LGUI_DRAW]   a2 q0_rva={:x} raw=[{}]", a2m.has_value() ? a2vt - (uintptr_t)*a2m : 0, dump_qwords(a2, 16));

                    std::string rects{};
                    const auto hw = (int32_t)VR::get()->get_hmd_width();
                    const auto hh = (int32_t)VR::get()->get_hmd_height();
                    for (uint32_t off = 0; off < 0x1000; off += 4) {
                        const auto v = *(int32_t*)((uintptr_t)a2 + off);
                        if (v == hw || v == hh || v == hw * 2) rects += fmt::format("{:x}={} ", off, v);
                    }
                    SPDLOG_INFO("[LGUI_DRAW]   a2 hmd-size hits (hmd={}x{}): {}", hw, hh, rects.empty() ? "<none>" : rects);

                    // Also try a2 as a view: does a2->Family (+0) contain a2 in its Views array?
                    const auto fam = *(uintptr_t*)a2;
                    if (fam != 0 && !IsBadReadPtr((void*)fam, 0x100)) {
                        const auto views = ((sdk::FSceneViewFamily*)fam)->get_views();
                        if (views != nullptr && !IsBadReadPtr(views, sizeof(*views)) && views->count > 0 && views->count <= 4 && views->data != nullptr && !IsBadReadPtr(views->data, sizeof(void*) * views->count)) {
                            for (int32_t k = 0; k < views->count; ++k) {
                                if ((uintptr_t)views->data[k] == (uintptr_t)a2) {
                                    SPDLOG_INFO("[LGUI_DRAW]   a2 IS FSceneView: family={:x} nviews={} view_index={}", fam, views->count, k);
                                }
                            }
                        }
                    }
                }
            }
        }

        SPDLOG_INFO("[LGUI_DRAW] #{} vtable_rva={:x} self={:x} rdg={:x} a3={:x} a4={:x} ret_rva={:x} frame={} {}",
            n, vrva, (uintptr_t)self, (uintptr_t)a2, (uintptr_t)a3, (uintptr_t)a4,
            ret_mod.has_value() ? ret - (uintptr_t)*ret_mod : ret, VR::get()->get_frame_count(), decoded);
    }

    // EXPERIMENT: LGUI gets no view rect; it appears to size its ortho canvas from the target's Desc.Extent
    // (a3 dwords [21],[22] == 2*hmd_w, hmd_h). A 16:9 canvas laid out over a 6296-wide target is 3541 tall,
    // so only the top-left quarter lands in the left eye ("blown up top-left corner"). Temporarily present the
    // extent as one eye wide while LGUI records its pass; restore afterwards.
    // RESULT: patching extent.x had no visible effect -> LGUI does not derive its layout from Desc.Extent.
    constexpr bool LGUI_PATCH_EXTENT = false;
    constexpr int LGUI_EXTENT_X_DWORD = 21;
    uint32_t saved_extent_x = 0;
    bool patched = false;

    if (LGUI_PATCH_EXTENT && a3 != nullptr && !IsBadReadPtr(a3, 0x80)) {
        const auto hw = VR::get()->get_hmd_width();
        auto& ext_x = ((uint32_t*)a3)[LGUI_EXTENT_X_DWORD];
        if (hw > 0 && ext_x == hw * 2) {
            saved_extent_x = ext_x;
            ext_x = hw;
            patched = true;
            if (n < 5) SPDLOG_INFO("[LGUI_DRAW]   patched a3 extent.x {} -> {}", saved_extent_x, hw);
        }
    }

    // a2 is NOT an FSceneView: its first qword equals a5 (packed 1/w,1/h floats), it is stable across frames (58883b3d0),
    // and it is >0x1000 readable with the eye size repeated at 0x3e0/0x410/0x4a0/0x4f8. That layout matches a per-view
    // uniform-parameter block (ViewSizeAndInvSize / BufferSizeAndInvSize / ViewRect...). Dump that block as int+float pairs.
    if (n < 3 && a2 != nullptr && !IsBadReadPtr(a2, 0x520)) {
        std::string head{};
        for (uint32_t off = 0; off < 0x60; off += 4) head += fmt::format("{:x}={:.4f} ", off, *(float*)((uintptr_t)a2 + off));
        SPDLOG_INFO("[LGUI_DRAW]   a2 head floats: {}", head);

        std::string rb{};
        for (uint32_t off = 0x3d0; off < 0x510; off += 4) {
            const auto iv = *(int32_t*)((uintptr_t)a2 + off);
            const auto fv = *(float*)((uintptr_t)a2 + off);
            if (iv > -16384 && iv < 16384) rb += fmt::format("{:x}=i{} ", off, iv);
            else rb += fmt::format("{:x}=f{:.4f} ", off, fv);
        }
        SPDLOG_INFO("[LGUI_DRAW]   a2 size block: {}", rb);

        // Nothing in the block equals the real target width (2*hmd_w). Look for it as an inverse (1/w, 1/h) or ratio
        // (hmd_w / 2hmd_w = 0.5) anywhere in the block, and dump every "interesting" float in 0x60..0x3d0.
        const auto hw = (float)VR::get()->get_hmd_width();
        const auto hh = (float)VR::get()->get_hmd_height();
        std::string inv{};
        std::string mid{};
        for (uint32_t off = 0; off < 0x1000; off += 4) {
            const auto fv = *(float*)((uintptr_t)a2 + off);
            if (!std::isfinite(fv) || fv == 0.0f) continue;
            auto approx = [&](float a, float b) { return std::fabs(a - b) <= std::fabs(b) * 0.002f; };
            if (approx(fv, 1.0f / hw)) inv += fmt::format("{:x}=1/hmd_w ", off);
            else if (approx(fv, 1.0f / hh)) inv += fmt::format("{:x}=1/hmd_h ", off);
            else if (approx(fv, 1.0f / (hw * 2.0f))) inv += fmt::format("{:x}=1/(2hmd_w) ", off);
            else if (approx(fv, hw * 2.0f)) inv += fmt::format("{:x}=2hmd_w ", off);
            else if (approx(fv, hw / hh)) inv += fmt::format("{:x}=hmd_aspect ", off);
            else if (approx(fv, (hw * 2.0f) / hh)) inv += fmt::format("{:x}=2hmd_aspect ", off);
            if (off >= 0x60 && off < 0x3d0 && std::fabs(fv) > 1e-6f && std::fabs(fv) < 1e6f && fv != 1.0f) mid += fmt::format("{:x}={:.4f} ", off, fv);
        }
        SPDLOG_INFO("[LGUI_DRAW]   a2 hmd-derived floats: {}", inv.empty() ? "<none>" : inv);
        SPDLOG_INFO("[LGUI_DRAW]   a2 mid floats: {}", mid);
    }

    // EXPERIMENT 3: a2's size block only knows the eye size (hmd_w x hmd_h) while the RDG target is 2*hmd_w wide.
    // Present the target as one eye wide to LGUI by patching every X == hmd_w in the block to 2*hmd_w before the draw
    // (restored afterwards). If the UI un-folds / changes scale, this block is the layout source.
    // EXPERIMENT 3 RESULT: patching the 4 rects to full width drew a single centered UI across the double-wide target
    // (visible in both eyes but not converged) -> the a2 rect block IS LGUI's layout source.
    // EXPERIMENT 4: draw LGUI once per eye. The rects are FIntRect {Min.X, Min.Y, Max.X, Max.Y} with Max at the offsets
    // below. Pass 1 uses [0, hw), pass 2 uses [hw, 2hw); both eyes then receive the identical UI image.
    // FIX: redirect the LGUI draw into UEVR's ui_target so the UI is presented as the detached OpenXR quad layer
    // (UI_Distance / UI_Size / UI_X_Offset / UI_Y_Offset / UI_FollowView) instead of being baked into the eye texture.
    // a3 is the FRDGTexture for ViewFamilyTexture: +0x10 = FRHITexture* ResourceRHI, +0x54 = Desc.Extent (confirmed in log).
    // LGUI records RDG passes that hold the FRDGTexture pointer, so we hand it a per-frame shadow copy whose RHI pointer and
    // extent describe ui_target (desktop resolution). Viewport rects in a2 are captured by value at record time.
    // RESULT: rects patched + RHI/extent swapped in the RDG copy, still no visual change -> LGUI does not take its render
    // target from a3 at all. Stock LGUI fetches it via InView.Family->RenderTarget->GetRenderTargetTexture(), i.e. the
    // FViewport vtable slot that UEVR's "AHUD UI Compatibility" option (viewport_get_render_target_texture_hook) redirects
    // to ui_target. Disabled; test that option instead.
    // EXPERIMENT 8 RESULT (persistent swap of the real a3 RHI): the UI *did* land in ui_target, but so did the whole scene
    // composite (ViewFamilyTexture is shared by every pass writing the final image) -> eyes went blank. So RDG resolves
    // the RT from the FRDGTexture object at execute time and LGUI honours the texture it is given; we just must not touch
    // the shared object. Re-enable the private shadow copy (the first attempt likely ran while ui_target was still null).
    // RESULT: shadow copy has no visible effect (LGUI does not take its RT from the a3 pointer). Disabled so the pass
    // registry diff further below can run (this branch returns early and starved it).
    constexpr bool LGUI_REDIRECT_TO_UI_TARGET = false;
    constexpr uint32_t LGUI_RECT_MAX_OFFS_UI[] = {0x3e0, 0x410, 0x4a0, 0x4f8};
    constexpr uint32_t RDG_RESOURCE_RHI_OFF = 0x10;
    constexpr uint32_t TEX_EXTENT_OFF = 0x54;

    // EXPERIMENT 10: reinterpretation. Persistent swap of the *real* a3 object redirected LGUI, the shadow copy passed as
    // a3 did not, and no pass registry grows during the call. So LGUI reaches the real FRDGTexture through another
    // reference: a2 (eye rects at 0x3e0.. = FSceneView::*ViewRect -> a2 is the FViewInfo, not the builder) or its Family.
    // Find every qword == a3 in a2 / *(a2) and point those at the shadow copy for the duration of the call: whatever LGUI
    // captures from there then resolves to ui_target at execute time, while the real texture stays untouched.
    constexpr bool LGUI_PATCH_A3_REFS = true;

    // DIAG/PERF: this is the actual redirect hook that forces LGUI to record its draw against ui_target
    // instead of the real scene RDG texture (via the a2/a3 shadow-copy swap below). VR::is_lgui_ui_redirect_disabled()
    // must gate HERE, not just at the copy/present site in D3D12Component.cpp, otherwise LGUI still gets redirected
    // (and still pays whatever cost that causes) even though nothing ever copies ui_target back into the VR view,
    // leaving the UI blank instead of falling back to its original (HMD-stuck) render path.
    // GUARD (nested-submenu crash): the redirect below hands LGUI a shadow copy of `a3` and patches every a2 slot
    // that points at it, then relies on that shadow copy staying valid until RDG's deferred Execute for THIS pass
    // runs (which can happen off a worker thread, well after this function returns). That is only safe if the
    // number of distinct `a3` values redirected per frame is bounded - the copy/eviction logic (below) assumes
    // stale slots are only ever reclaimed by a NEW frame's a3, not by extra, concurrent a3s from ADDITIONAL UI
    // layers within the SAME frame. Stereo rendering legitimately produces up to 2 distinct a3 values per frame
    // (one LGUI pass per eye), so limiting to a single a3/frame incorrectly blocked the second eye's redirect,
    // leaving that eye's UI drawn into its real (un-redirected) per-eye scene texture - i.e. still attached to
    // the HMD, at its original scale, and outside the quad layer's input routing (the "double UI, one eye stuck
    // to HMD" regression). Opening a submenu inside a submenu instead adds a 3rd+ concurrent RDG pass (an
    // additional FRDGTexture/a3) before an earlier pass's Execute has consumed its shadow copy, so patching that
    // extra a3's refs can shift/evict a slot an earlier pass's still-pending Execute is about to read - the
    // read/write access-violation family observed (rva 0x23e1f8xx/0x23e2aaxx, faulting near +0xd0 into a shadow
    // copy). Allow up to 4 distinct a3 values per frame (2 eyes + nested popup/submenu passes); any additional
    // a3 this frame is left untouched (falls back to its normal, un-redirected render path) instead of risking
    // that corruption. This is paired with execute-safe slot eviction below (slots used in the current or
    // immediately preceding frame are never reclaimed), which is what makes raising this cap safe.
    static uint64_t s_lgui_redirect_frame = UINT64_MAX;
    static constexpr size_t LGUI_MAX_REDIRECTS_PER_FRAME = 4;
    static std::array<uintptr_t, LGUI_MAX_REDIRECTS_PER_FRAME> s_lgui_redirect_a3_this_frame{};
    static size_t s_lgui_redirect_count_this_frame = 0;
    const auto lgui_current_frame = (uint64_t)VR::get()->get_frame_count();

    if (lgui_current_frame != s_lgui_redirect_frame) {
        s_lgui_redirect_frame = lgui_current_frame;
        s_lgui_redirect_a3_this_frame.fill(0);
        s_lgui_redirect_count_this_frame = 0;
    }

    bool lgui_a3_allowed_this_frame = false;
    bool lgui_a3_already_registered_this_frame = false;
    for (size_t i = 0; i < s_lgui_redirect_count_this_frame; ++i) {
        if (s_lgui_redirect_a3_this_frame[i] == (uintptr_t)a3) {
            lgui_a3_allowed_this_frame = true;
            lgui_a3_already_registered_this_frame = true;
            break;
        }
    }
    if (!lgui_a3_allowed_this_frame && s_lgui_redirect_count_this_frame < LGUI_MAX_REDIRECTS_PER_FRAME) {
        lgui_a3_allowed_this_frame = true;
    }

    if (LGUI_PATCH_A3_REFS && !VR::get()->is_lgui_ui_redirect_disabled() && lgui_a3_allowed_this_frame &&
        g_hook != nullptr && g_hook->get_render_target_manager() != nullptr &&
        a2 != nullptr && !IsBadReadPtr(a2, 0x1000) && a3 != nullptr && !IsBadReadPtr(a3, 0x200))
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();

        if (ui_target != nullptr && !IsBadReadPtr(ui_target, TEX_EXTENT_OFF + 8)) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + TEX_EXTENT_OFF);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + TEX_EXTENT_OFF + 4);
            const auto hw = (int32_t)VR::get()->get_hmd_width();
            const auto hh = (int32_t)VR::get()->get_hmd_height();

            if (ui_w >= 64 && ui_w <= 16384 && ui_h >= 64 && ui_h <= 16384) {
                // Copy is padded so readers touching fields past what we duplicated still land in valid (zeroed) memory.
                //
                // ROOT CAUSE (submenu-open crash/hang): this used to be a fixed 32-slot ring buffer indexed purely
                // by a rolling per-call counter (`copies[copy_idx++ % COPY_COUNT]`), with NO relationship to how
                // long any given frame's `a3` (FRDGTexture) shadow copy might still be referenced by other deferred
                // render-graph passes. Opening this specific nested submenu adds extra deferred UI passes that
                // capture and read back one of these patched pointers (see LGUI_PATCH_A3_REFS below) more than 32
                // draw calls later. By then the ring buffer slot had already been recycled and overwritten by an
                // unrelated later frame's RDG texture data - not freed memory, but silently stale/wrong-frame data
                // (mismatched vtable pointer, RHI resource, extent, etc.), which is why crash addresses/values
                // varied so much between runs. The previous developer's own comment above already diagnosed this:
                // "some other pass reads the stale copy through one of these slots".
                //
                // FIX: key the shadow copy by the real `a3` pointer instead of a rolling counter, so the SAME
                // source texture always maps to the SAME persistent shadow copy buffer for as long as that a3
                // object exists (we simply refresh its contents every frame instead of relocating it), and only
                // reclaim/reuse a slot for a genuinely different a3 once we've evicted the oldest live entry, not
                // on a fixed cadence unrelated to how long other passes keep referencing it.
                constexpr size_t COPY_SIZE = 0x200;
                constexpr size_t COPY_STRIDE = 0x800;
                constexpr size_t COPY_COUNT = 64;
                static uint8_t copies[COPY_COUNT][COPY_STRIDE]{};
                static std::array<uintptr_t, COPY_COUNT> copy_owner{};      // a3 pointer currently occupying this slot (0 = free)
                static std::array<uint64_t, COPY_COUNT> copy_last_used{};   // monotonic "frame" stamp of last use, for LRU eviction
                static std::unordered_map<uintptr_t, size_t> copy_slot_by_owner{};
                static uint64_t copy_clock = 0;
                ++copy_clock;

                const auto a3_key = (uintptr_t)a3;
                size_t slot;

                if (const auto it = copy_slot_by_owner.find(a3_key); it != copy_slot_by_owner.end()) {
                    slot = it->second;
                } else {
                    // Find a free slot, or evict the least-recently-used occupied one that is EXECUTE-SAFE to
                    // reclaim, i.e. not still owned by a pass whose deferred RDG Execute may still be pending
                    // for the current or immediately preceding "copy_clock" tick. copy_clock advances once per
                    // redirect call (not once per frame), so a slot last touched within
                    // LGUI_SLOT_EXECUTE_SAFE_WINDOW ticks of "now" is treated as still potentially in flight and
                    // is skipped for eviction. This is what makes raising LGUI_MAX_REDIRECTS_PER_FRAME above 2
                    // safe: nested popup/submenu passes can no longer steal a slot out from under an earlier
                    // pass's still-pending Execute (the crash/corruption this whole scheme guards against).
                    constexpr uint64_t LGUI_SLOT_EXECUTE_SAFE_WINDOW = COPY_COUNT * 2;
                    slot = SIZE_MAX;
                    uint64_t oldest = UINT64_MAX;
                    bool found_free = false;

                    for (size_t i = 0; i < COPY_COUNT; ++i) {
                        if (copy_owner[i] == 0) {
                            slot = i;
                            found_free = true;
                            break;
                        }
                        const bool recently_used = (copy_clock - copy_last_used[i]) < LGUI_SLOT_EXECUTE_SAFE_WINDOW;
                        if (recently_used) continue; // not execute-safe to reclaim yet
                        if (copy_last_used[i] < oldest) {
                            oldest = copy_last_used[i];
                            slot = i;
                        }
                    }

                    if (slot == SIZE_MAX) {
                        // Every slot is still within the execute-safe window (extremely unlikely given
                        // COPY_COUNT=64 vs. a cap of 4 redirects/frame). Fall back to strict LRU rather than
                        // dropping this redirect, since that would be worse than the small residual risk.
                        slot = 0;
                        oldest = copy_last_used[0];
                        for (size_t i = 1; i < COPY_COUNT; ++i) {
                            if (copy_last_used[i] < oldest) {
                                oldest = copy_last_used[i];
                                slot = i;
                            }
                        }
                        found_free = false;
                    }

                    if (!found_free) {
                        copy_slot_by_owner.erase(copy_owner[slot]);
                    }

                    copy_owner[slot] = a3_key;
                    copy_slot_by_owner[a3_key] = slot;
                }

                copy_last_used[slot] = copy_clock;
                auto copy = copies[slot];
                memset(copy, 0, COPY_STRIDE);
                memcpy(copy, a3, COPY_SIZE);
                *(void**)(copy + RDG_RESOURCE_RHI_OFF) = ui_target;
                if (!lgui_a3_already_registered_this_frame) {
                    s_lgui_redirect_a3_this_frame[s_lgui_redirect_count_this_frame++] = a3_key;
                }
                g_lgui_swap.rdg_texture = a3;
                g_lgui_swap.shadow_copy = copy;
                g_lgui_swap.ui_target = ui_target;
                g_lgui_swap.rdg_texture_vtable = *(uintptr_t*)a3;
                g_lgui_swap.original_rhi = *(void**)((uintptr_t)a3 + RDG_RESOURCE_RHI_OFF);

                // LGUI sets its raster viewport from Desc.Extent of the target (top-left aligned, not from any rect in a2) and its
                // canvas layout / ortho projection from the game-thread viewport size (FViewport::GetSizeXY, sampled in
                // game_viewport_client_draw_hook). Every observed distortion is a mismatch between those two, so give the copy
                // exactly the game viewport size (scaled to fit ui_target if it is larger). Until V has been sampled, fall
                // back to the old per-mode aspect estimate.
                {
                    const auto hw0 = (int32_t)VR::get()->get_hmd_width();
                    const auto hh0 = (int32_t)VR::get()->get_hmd_height();
                    const bool nsf = VR::get()->is_native_stereo_fix_enabled();
                    const auto vp = g_hook->get_game_viewport_size();
                    int32_t ext_w = ui_w, ext_h = ui_h;
                    float vp_aspect = 0.0f;
                    const char* src = "none";

                    if (vp.width > 0 && vp.height > 0) {
                        src = "game_viewport";
                        vp_aspect = (float)vp.width / (float)vp.height;
                        ext_w = vp.width;
                        ext_h = vp.height;
                        if (ext_w > ui_w || ext_h > ui_h) {
                            ext_h = ui_h;
                            ext_w = (int32_t)((float)ui_h * vp_aspect + 0.5f);
                            if (ext_w > ui_w) { ext_w = ui_w; ext_h = (int32_t)((float)ui_w / vp_aspect + 0.5f); }
                        }
                    } else if (hw0 > 0 && hh0 > 0) {
                        src = "hmd_estimate";
                        vp_aspect = (float)(nsf ? hw0 : hw0 * 2) / (float)hh0;
                        ext_h = ui_h;
                        ext_w = (int32_t)((float)ui_h * vp_aspect + 0.5f);
                        if (ext_w > ui_w) { ext_w = ui_w; ext_h = (int32_t)((float)ui_w / vp_aspect + 0.5f); }
                    }

                    // Measured across NSF on/off x 2D/VR: the patched extent and a2 rects are identical in every mode
                    // (7680x2160 / 3840x2160) yet the visible result differs per mode, so none of them size the UI. The
                    // symptoms all match LGUI laying its canvas out in pixel space at the game-thread stereo view size and
                    // drawing it top-left aligned into whatever target it gets:
                    //   NSF on  2D: 1985x1116 px in 3840x2160 -> correct aspect, top-left quarter only
                    //   NSF on  VR: 2229x2637 px in 3840x2160 -> tall/thin on the left, clipped at the bottom
                    //   NSF off VR: 4458x2637 px (double-wide family) -> overflows both axes
                    //   NSF off 2D: 3970x1116 px -> fills the width
                    // That size is the per-eye view rect with NSF (AdjustViewRect keeps x=0, one eye per family) and the
                    // full double-wide family extent without it. Rescaling the copy is a no-op for layout (see the
                    // LGUI_PATCH_EXTENT result above), so keep the copy at the real ui_target size and instead report the
                    // drawn region so the quad layer crops exactly what LGUI produced.
                    const auto fam_w = *(int32_t*)((uintptr_t)a3 + TEX_EXTENT_OFF);
                    const auto fam_h = *(int32_t*)((uintptr_t)a3 + TEX_EXTENT_OFF + 4);
                    int32_t rect_vw = 0, rect_vh = 0;
                    if (*(uintptr_t*)((uintptr_t)a2 + 0x3d0) == (uintptr_t)a3) { // a2 already validated readable for 0x1000
                        const auto r = (int32_t*)((uintptr_t)a2 + 0x3d8);
                        rect_vw = r[2] - r[0];
                        rect_vh = r[3] - r[1];
                    }

                    int32_t region_w = ext_w, region_h = ext_h;
                    const char* region_src = src;
                    // LGUI still lays its canvas out at the pre-patch per-eye/family view rect size (hw x hh with NSF,
                    // 2*hw x hh without) and paints top-left aligned into ui_target, regardless of the rect patches
                    // above (confirmed by [LGUI_BOUNDS]: painted bbox size tracks hmd_w x hmd_h, not ui_target).
                    // Reporting the full ui_target size here (as before) told the quad to show the WHOLE 3840x2160
                    // canvas although only that small top-left sub-rect has real content -> squished-looking UI with
                    // no visible scale-up. Report the real painted region instead so the quad crops tightly to it.
                    // With LGUI_LETTERBOX_RECT enabled below, the a2 FScreenPassTexture rects (which drive LGUI's canvas
                    // layout) are patched to a centered 16:9 rect instead of the raw eye-shaped view_rect, so report that
                    // rect here too - it is what LGUI will actually paint into this frame.
                    if (rect_vw > 0 && rect_vh > 0) {
                        region_w = rect_vw;
                        region_h = rect_vh;
                        region_src = "view_rect";
                    } else {
                        region_w = ui_w;
                        region_h = ui_h;
                        region_src = "ui_target";
                    }
                    region_w = std::min(region_w, ui_w);
                    region_h = std::min(region_h, ui_h);

                    ext_w = ui_w;
                    ext_h = ui_h;

                    *(int32_t*)(copy + TEX_EXTENT_OFF) = ext_w;
                    *(int32_t*)(copy + TEX_EXTENT_OFF + 4) = ext_h;
                    g_hook->set_ui_draw_extent(region_w, region_h);

                    static int32_t last_region_w = -1, last_region_h = -1;
                    static bool last_nsf = false;
                    if (LGUI_DIAG_STEADY_STATE && (n < 3 || (n % 600) == 0 || region_w != last_region_w || region_h != last_region_h || nsf != last_nsf)) {
                        last_region_w = region_w;
                        last_region_h = region_h;
                        last_nsf = nsf;
                        SPDLOG_INFO("[LGUI_REFS]   copy extent {}x{} region {}x{} (region_src={} vp_src={} game_vp={}x{} family={}x{} view_rect={}x{} nsf={} hmd={}x{} ui_target={}x{})",
                            ext_w, ext_h, region_w, region_h, region_src, src, vp.width, vp.height, fam_w, fam_h, rect_vw, rect_vh, nsf, hw0, hh0, ui_w, ui_h);
                    }
                }

                uint32_t a2_len = 0;
                while (a2_len < 0x8000 && !IsBadReadPtr((void*)((uintptr_t)a2 + a2_len), 0x1000)) a2_len += 0x1000;

                // Bisect: patching all 14 refs redirected the UI (success) but crashed (null deref in game code) when the in-game
                // menu opened -> some other pass reads the stale copy through one of these slots. Only patch offsets below
                // LGUI_REF_MAX_OFF (the view-parameter group 0x270..0x4e8) first; the 0x12d0+ group is left on the real texture.
                // RESULT: with only 3d0/400/4e8 the UI went back into the scene (no crash) -> the draw target comes from
                // 270/348/3a0. Flip the bisect: patch only that group, the exception handler now logs registers for the crash.
                // 0x3d0/0x400/0x4e8: redirecting them crashed again (render thread null deref at +0xd0 into the shadow copy from
                // another pass). Keep them on the real texture.
                constexpr uint32_t LGUI_REF_MAX_OFF = 0x3d0;
                constexpr uint32_t LGUI_REF_MIN_OFF = 0x0;

                std::vector<uintptr_t*> refs{};
                std::string where{};
                const uint32_t skipped = 0; // no longer counted; scan is capped to the functional range below
                // PERF: only offsets in [LGUI_REF_MIN_OFF, LGUI_REF_MAX_OFF) are ever used functionally, so cap the
                // scan there instead of walking the whole ~0x8000 a2 buffer. The old full walk called IsBadReadPtr on
                // every 8-byte slot (thousands of costly checks per UI draw) - a confirmed steady-state lag source on
                // the render thread. The functional redirect group lives entirely below 0x3d0.
                const uint32_t ref_scan_end = std::min<uint32_t>(a2_len, LGUI_REF_MAX_OFF);
                for (uint32_t off = LGUI_REF_MIN_OFF; off + 8 <= ref_scan_end; off += 8) {
                    auto p = (uintptr_t*)((uintptr_t)a2 + off);
                    // The a2_len page-readability scan above only validates readability once, before this loop
                    // runs. During a live resolution change the engine can resize/free parts of this buffer
                    // between that check and here (observed crash: EXCEPTION_ACCESS_VIOLATION reading a stale
                    // pointer at this line). Re-validate each 8-byte slot immediately before dereferencing it.
                    if (IsBadReadPtr(p, sizeof(uintptr_t))) break;
                    if (*p != (uintptr_t)a3) continue;
                    refs.push_back(p);
                    where += fmt::format("a2+{:x} ", off);
                }

                // Stretch persists with every int (hw,hh) pair patched -> the ortho projection is built from a size we do not see as
                // ints (float aspect / 1/w / matrix) or from game-thread canvas data. Two-pronged:
                //  (a) diagnostics: log any float in a2 that equals hw, hh, hw/hh, hh/hw, 1/hw, 1/hh, 2/hw, 2/hh;
                //  (b) fallback: LGUI lays its canvas out at the FScreenPassTexture rects patched below (a2+0x3e0/0x410/0x4a0/0x4f8),
                //      which stock code sets to (0,0,hw,hh) - the narrow per-eye aspect. Patching those rects to a wider/letterboxed
                //      shape was measured to be a NO-OP: [LGUI_BOUNDS] painted region stayed at the per-eye size regardless, because
                //      LGUI builds its ortho canvas from the shared FSceneView view rect (which we cannot touch without breaking the
                //      real stereo scene render), not from these per-draw scratch rects. Disabled: it changes nothing and only adds
                //      per-frame cost. The narrow-aspect look is instead corrected purely at presentation time on the OpenXR quad
                //      (OverlayComponent::generate_slate_quad), which is independent of the scene render.
                constexpr bool LGUI_LETTERBOX_RECT = false;
                constexpr float LGUI_LETTERBOX_ASPECT = 16.0f / 9.0f;
                int32_t rect_x0 = 0, rect_y0 = 0, rect_w = ui_w, rect_h = ui_h;

                if (LGUI_LETTERBOX_RECT && hw > 0 && hh > 0) {
                    rect_h = ui_h;
                    rect_w = (int32_t)((float)ui_h * LGUI_LETTERBOX_ASPECT);
                    if (rect_w > ui_w) { rect_w = ui_w; rect_h = (int32_t)((float)ui_w / LGUI_LETTERBOX_ASPECT); }
                    rect_x0 = (ui_w - rect_w) / 2;
                    rect_y0 = (ui_h - rect_h) / 2;

                    // The letterbox rect patched into a2 below is what LGUI actually lays its canvas out at this
                    // frame, superseding the raw view_rect/ui_target guess reported above - update the draw extent
                    // (used by the OpenXR quad crop) to match it exactly.
                    g_hook->set_ui_draw_extent(rect_w, rect_h);
                }

                if (n < 3) {
                    std::string fl{};
                    const float cands[] = {(float)hw, (float)hh, (float)hw / (float)hh, (float)hh / (float)hw, 1.0f / (float)hw, 1.0f / (float)hh, 2.0f / (float)hw, 2.0f / (float)hh};
                    const char* names[] = {"w", "h", "w/h", "h/w", "1/w", "1/h", "2/w", "2/h"};
                    for (uint32_t off = 0; off + 4 <= 0x1000; off += 4) {
                        const auto v = *(float*)((uintptr_t)a2 + off);
                        for (int c = 0; c < 8; ++c) {
                            if (std::fabs(v - cands[c]) <= std::fabs(cands[c]) * 1e-4f) { fl += fmt::format("{:x}={} ", off, names[c]); break; }
                        }
                    }
                    SPDLOG_INFO("[LGUI_REFS]   float size candidates: {} | letterbox rect=({},{} {}x{})", fl.empty() ? "<none>" : fl, rect_x0, rect_y0, rect_w, rect_h);

                    // a2 appears to hold FScreenPassTexture entries {FRDGTexture* Texture; FIntRect ViewRect} (e.g. 0x3d0 -> rect at
                    // 0x3d8..0x3e4 = (0,0,hw,hh)). Dump every such entry whose pointer shares a3's vtable so the table is on record.
                    std::string spt{};
                    const auto a3_vt = *(uintptr_t*)a3;
                    const uint32_t spt_end = std::min<uint32_t>(a2_len, 0x1400);
                    for (uint32_t off = 0; off + 0x18 <= spt_end; off += 8) {
                        const auto p = *(uintptr_t*)((uintptr_t)a2 + off);
                        if (p < 0x10000 || (p & 7) != 0 || IsBadReadPtr((void*)p, 0x60) || *(uintptr_t*)p != a3_vt) continue;
                        const auto r = (int32_t*)((uintptr_t)a2 + off + 8);
                        spt += fmt::format("{:x}:{}{}({},{},{},{}) ", off, p == (uintptr_t)a3 ? "A3" : "tex", p == (uintptr_t)a3 ? "" : fmt::format("[{}x{}]", *(int32_t*)(p + TEX_EXTENT_OFF), *(int32_t*)(p + TEX_EXTENT_OFF + 4)), r[0], r[1], r[2], r[3]);
                    }
                    SPDLOG_INFO("[LGUI_REFS]   a2 screen-pass entries: {}", spt.empty() ? "<none>" : spt);
                }

                // OPTION 3 PROBE: find the a2 slot(s) that point at the FSceneView object LGUI reads for its canvas
                // extent. Distinct from the FScreenPassTexture scratch rects (LGUI_RECT_MAX_OFFS_UI, proven no-op) and
                // the a3-vtable texture refs: here we look for pointers to an object that CONTAINS a (0,0,hw,hh) FIntRect
                // but is NOT an a3-vtable texture. That object is the candidate FSceneView whose ViewRect actually drives
                // the paint. Probe only (no patching yet) so we can confirm the field exists and its exact offset before
                // risking a write. Gated behind LGUI_DIAG_VERBOSE plus a one-shot so it never adds steady-state cost.
                {
                    static bool sv_probed = false;
                    if (LGUI_DIAG_VERBOSE && !sv_probed && hw > 0 && hh > 0) {
                        const auto a3_vt = (a3 != nullptr && !IsBadReadPtr(a3, sizeof(uintptr_t))) ? *(uintptr_t*)a3 : 0;
                        std::string cand{};
                        const uint32_t scan_end = std::min<uint32_t>(a2_len, 0x2000);
                        for (uint32_t off = 0; off + 8 <= scan_end; off += 8) {
                            const auto p = *(uintptr_t*)((uintptr_t)a2 + off);
                            if (p < 0x10000 || (p & 7) != 0 || IsBadReadPtr((void*)p, 0x800)) continue;
                            if (a3_vt != 0 && *(uintptr_t*)p == a3_vt) continue; // skip texture objects
                            for (uint32_t ioff = 0; ioff + 16 <= 0x800; ioff += 4) {
                                const auto r = (int32_t*)(p + ioff);
                                if (r[0] == 0 && r[1] == 0 && r[2] == hw && r[3] == hh) {
                                    cand += fmt::format("a2+{:x}->obj+{:x} ", off, ioff);
                                    break;
                                }
                            }
                            if (cand.size() > 400) break;
                        }
                        sv_probed = true;
                        SPDLOG_INFO("[LGUI_SVPROBE] FSceneView-candidate ptrs holding (0,0,{},{}) rect: {}", hw, hh, cand.empty() ? "<none>" : cand);
                    }
                }

                std::vector<std::pair<int32_t*, int32_t>> saved{};
                for (auto off : LGUI_RECT_MAX_OFFS_UI) {
                    auto max_x = (int32_t*)((uintptr_t)a2 + off);
                    auto min_x = max_x - 2;
                    if (*(min_x + 1) == 0 && (*max_x == hw || *max_x == hw * 2) && *(max_x + 1) == hh) {
                        saved.emplace_back(min_x, *min_x);
                        saved.emplace_back(min_x + 1, *(min_x + 1));
                        saved.emplace_back(max_x, *max_x);
                        saved.emplace_back(max_x + 1, *(max_x + 1));
                        *min_x = rect_x0;
                        *(min_x + 1) = rect_y0;
                        *max_x = rect_x0 + rect_w;
                        *(max_x + 1) = rect_y0 + rect_h;
                    }
                }

                for (auto p : refs) *p = (uintptr_t)copy;

                // Stretch: UI fills the quad but is laid out for the portrait eye (2229x2637) -> projection/canvas size still comes
                // from an eye-sized field we have not patched. The a2 dump shows further (hw,hh) pairs at 0x778/0x77c, 0xe08/0xe0c
                // and a lone hw at 0x66c. Patch every remaining (hw,hh) pair (and lone hw/hh next to zero) in a2 to (ui_w, ui_h).
                // Re-enabled: LGUI_BOUNDS readback proves the UI is painted at exactly hmd_w x hmd_h top-left in ui_target,
                // so the layout size comes from an (hw,hh) field in a2. Earlier "no effect" verdicts were by eye; the bbox log
                // now measures whether these patches move the painted region.
                // Re-enabled together with the game-thread SetupView rect patch: the ortho projection (game thread) and the
                // raster viewport (render thread, from these a2 fields) must BOTH be 3840x2160. Alone, either one leaves the
                // paint clamped to the other's hmd rect, which is why each looked like a no-op in isolation.
                // Disabled: rewriting a dozen eye-size pairs up to 3840x2160 every draw is a proven no-op for the visible
                // painted region and is a suspected 4K-work lag source. Aspect is fixed at presentation on the quad instead.
                constexpr bool LGUI_PATCH_ALL_SIZE_PAIRS = false;
                uint32_t extra_patched = 0;
                std::string extra_where{};

                if (LGUI_PATCH_ALL_SIZE_PAIRS) {
                    // Residue scan showed (hw,hh) pairs beyond 0x1000 (0x11b0..0x1bf4) and a lone hw at 0x66c; cover the whole view.
                    const uint32_t patch_len = std::min<uint32_t>(a2_len, 0x2000);
                    for (uint32_t off = 0x60; off + 8 <= patch_len; off += 4) {
                        auto x = (int32_t*)((uintptr_t)a2 + off);
                        if (x[0] == hw && x[1] == hh) {
                            x[0] = rect_w;
                            x[1] = rect_h;
                            ++extra_patched;
                            extra_where += fmt::format("{:x} ", off);
                            off += 4;
                        } else if (x[0] == hw * 2 && x[1] == hh) {
                            x[0] = rect_w;
                            x[1] = rect_h;
                            ++extra_patched;
                            extra_where += fmt::format("{:x}(2w) ", off);
                            off += 4;
                        } else if (x[0] == hw && off == 0x66c) {
                            x[0] = rect_w;
                            ++extra_patched;
                            extra_where += fmt::format("{:x}(lone) ", off);
                        }
                    }
                    if (n < 5 || (n % 600) == 0) SPDLOG_INFO("[LGUI_REFS]   extra size pairs patched={} at [{}] -> {}x{} (hmd={}x{})", extra_patched, extra_where.empty() ? "-" : extra_where, rect_w, rect_h, hw, hh);
                }


                // Stretch fix: a5 (and a2+0/+4, same values) is a packed (1/w, 1/h) float pair describing the eye view, which
                // LGUI uses for its ortho canvas layout. Present the UI target's inverse size instead so the canvas fills 3840x2160.
                // Disabled: no-op for the visible layout, part of the same 4K-forcing patch set suspected of causing lag.
                constexpr bool LGUI_PATCH_INV_SIZE = false;
                void* a5_patched = a5;
                bool a2_inv_patched = false;

                if (LGUI_PATCH_INV_SIZE) {
                    const float inv[2] = {1.0f / (float)rect_w, 1.0f / (float)rect_h};
                    uint64_t packed = 0;
                    memcpy(&packed, inv, sizeof(packed));
                    a5_patched = (void*)packed;

                    const auto a2_inv = (float*)a2;
                    const auto old_inv = *(uint64_t*)a2;
                    if (old_inv == (uint64_t)a5 && a2_inv[0] > 0.0f && a2_inv[0] < 0.1f && a2_inv[1] > 0.0f && a2_inv[1] < 0.1f) {
                        a2_inv[0] = inv[0];
                        a2_inv[1] = inv[1];
                        a2_inv_patched = true;
                    }
                }

                // After every int/float patch: what in a2 (the FViewInfo) still encodes the eye size? This is what LGUI must be
                // reading for the canvas width, since LGUI_BOUNDS stays at hmd_w wide with everything above active.
                if (LGUI_DIAG_VERBOSE && (n < 3 || (n % 600) == 0)) {
                    const float fhw = (float)hw, fhh = (float)hh;
                    const float cands[] = {fhw, fhh, fhw / fhh, fhh / fhw, 1.0f / fhw, 1.0f / fhh, 2.0f / fhw, 2.0f / fhh, fhw * 2.0f, 2.0f * fhw / fhh, 0.5f / fhw};
                    const char* names[] = {"hw", "hh", "hw/hh", "hh/hw", "1/hw", "1/hh", "2/hw", "2/hh", "2hw", "2hw/hh", "0.5/hw"};
                    std::string fl{}, ints{};
                    const uint32_t scan_len = std::min<uint32_t>(a2_len, 0x2000);
                    for (uint32_t off = 0; off + 4 <= scan_len; off += 4) {
                        const auto u = *(uint32_t*)((uintptr_t)a2 + off);
                        const auto iv = (int32_t)u;
                        const auto fv = *(float*)&u;
                        if (iv == hw || iv == hh || iv == hw * 2) ints += fmt::format("{:x}={} ", off, iv);
                        for (int c = 0; c < 11; ++c) {
                            if (std::fabs(fv - cands[c]) <= std::fabs(cands[c]) * 1e-4f) { fl += fmt::format("{:x}={} ", off, names[c]); break; }
                        }
                    }
                    SPDLOG_INFO("[LGUI_REFS]   a2 post-patch eye-size residue: ints=[{}] floats=[{}] (a5={:x} a2+0 patched={})", ints.empty() ? "-" : ints, fl.empty() ? "-" : fl, (uintptr_t)a5, a2_inv_patched);
                }

                if (LGUI_DIAG_VERBOSE && (n < 5 || (n % 300) == 0)) {
                    SPDLOG_INFO("[LGUI_REFS] a3={:x} a2_len={:x} refs={} (skipped {}) at [{}] -> copy={:x} (ui_target={:x} {}x{}), rects patched={}",
                        (uintptr_t)a3, a2_len, refs.size(), skipped, where.empty() ? "-" : where, (uintptr_t)copy, (uintptr_t)ui_target, ui_w, ui_h, saved.size() / 3);
                }

                // Present the REAL family texture at ui_target size while LGUI records its pass (its lambda may capture the
                // viewport from Texture->Desc.Extent at record time through one of the 0x12d0+ refs we do not redirect).
                // Restored right after the call so every other pass still sees the true extent.
                constexpr bool LGUI_PATCH_REAL_A3_EXTENT_DURING_RECORD = true;
                int32_t saved_real_ext[2]{};
                int32_t* real_ext = nullptr;
                if (LGUI_PATCH_REAL_A3_EXTENT_DURING_RECORD) {
                    real_ext = (int32_t*)((uintptr_t)a3 + TEX_EXTENT_OFF);
                    saved_real_ext[0] = real_ext[0];
                    saved_real_ext[1] = real_ext[1];
                    real_ext[0] = ui_w;
                    real_ext[1] = ui_h;
                }

                // a4 is SceneDepthZ (eye-sized: hmd_w x hmd_h with NSF, 2*hmd_w x hmd_h without). Binding it as the depth
                // attachment alongside the 3840x2160 colour target clips the drawable area to the depth extent -> exactly the
                // measured LGUI_BOUNDS in every mode (2580x2160 / 2020x1084 / 3840x2160). Screen-space UI needs no scene depth.
                // RESULT: null depth accepted by LGUI (no fault) but LGUI_BOUNDS unchanged, and the depth was 1720x2036 while the
                // paint was 2020 wide -> depth attachment does not clip the draw. Disabled.
                constexpr bool LGUI_NULL_DEPTH = false;
                void* a4_patched = LGUI_NULL_DEPTH ? nullptr : a4;
                if (n < 3 && a4 != nullptr && !IsBadReadPtr(a4, TEX_EXTENT_OFF + 8)) {
                    SPDLOG_INFO("[LGUI_REFS]   a4 (depth) extent {}x{} -> passing {}", *(int32_t*)((uintptr_t)a4 + TEX_EXTENT_OFF), *(int32_t*)((uintptr_t)a4 + TEX_EXTENT_OFF + 4), LGUI_NULL_DEPTH ? "nullptr" : "as-is");
                }

                static bool null_depth_faulted = false;
                void* result = nullptr;
                const auto call_orig = (void*(*)(void*, void*, void*, void*, void*, void*))it->second;

                // EXPERIMENT: swap the REAL FSceneView::ViewRect to ui_target size for the duration of LGUI's
                // draw only, then restore synchronously below. See LGUI_PROBE_VIEWRECT_SWAP notes. Combined
                // with [LGUI_BOUNDS] this proves whether LGUI's paint extent follows the live view rect.
                std::vector<LguiRectPatch> probe_patches{};
                std::string probe_where{};
                if (LGUI_PROBE_VIEWRECT_SWAP && VR::get()->is_hmd_active()) {
                    lgui_probe_swap_view_rects(a3, probe_patches, probe_where);
                    if (!probe_patches.empty() && (n < 5 || (n % 300) == 0)) {
                        SPDLOG_INFO("[LGUI_VRPROBE] swapped {} real view rect field(s) to {}x{} for LGUI draw [{}]",
                            probe_patches.size(), ui_w, ui_h, probe_where.empty() ? "-" : probe_where);
                    }
                }

                if (a4_patched == nullptr && !null_depth_faulted) {
                    if (!lgui_call_draw_seh(call_orig, self, a2, copy, nullptr, a5_patched, a6, &result)) {
                        null_depth_faulted = true;
                        SPDLOG_ERROR("[LGUI_REFS] LGUI faulted with null depth; falling back to real depth from now on");
                        result = call_orig(self, a2, copy, a4, a5_patched, a6);
                    }
                } else {
                    result = call_orig(self, a2, copy, null_depth_faulted ? a4 : a4_patched, a5_patched, a6);
                }

                // Restore the real view rect(s) immediately so the actual 3D scene render is never affected.
                if (!probe_patches.empty()) {
                    lgui_restore_view_rects(probe_patches);
                }

                if (real_ext != nullptr) {
                    real_ext[0] = saved_real_ext[0];
                    real_ext[1] = saved_real_ext[1];
                    if (n < 3) SPDLOG_INFO("[LGUI_REFS]   real a3 extent presented as {}x{} during record (was {}x{})", ui_w, ui_h, saved_real_ext[0], saved_real_ext[1]);
                }

                // EXPERIMENT 11: restoring the refs immediately had no effect
                // swap of the real object did redirect. => the pass reads the field at RDG execute time, i.e. after we return.
                // Leave a2's refs/rects pointing at the copy; a2 is per-frame view state so it dies with the frame anyway.
                constexpr bool LGUI_KEEP_REFS_PATCHED = true;

                if (!LGUI_KEEP_REFS_PATCHED) {
                    for (auto p : refs) *p = (uintptr_t)a3;
                    for (auto& [p, v] : saved) *p = v;
                }

                return result;
            }
        }
    }

    if (LGUI_REDIRECT_TO_UI_TARGET && g_hook != nullptr && g_hook->get_render_target_manager() != nullptr &&
        a2 != nullptr && !IsBadReadPtr(a2, 0x520) && a3 != nullptr && !IsBadReadPtr(a3, 0x200))
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();

        if (ui_target != nullptr && !IsBadReadPtr(ui_target, TEX_EXTENT_OFF + 8)) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + TEX_EXTENT_OFF);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + TEX_EXTENT_OFF + 4);
            const auto hw = (int32_t)VR::get()->get_hmd_width();
            const auto hh = (int32_t)VR::get()->get_hmd_height();

            if (ui_w >= 64 && ui_w <= 16384 && ui_h >= 64 && ui_h <= 16384) {
                // Shadow copies must outlive this call (RDG executes the recorded passes later in the frame).
                constexpr size_t COPY_SIZE = 0x200;
                constexpr size_t COPY_COUNT = 16;
                static uint8_t copies[COPY_COUNT][COPY_SIZE]{};
                static uint32_t copy_idx = 0;
                auto copy = copies[copy_idx++ % COPY_COUNT];
                memcpy(copy, a3, COPY_SIZE);
                *(void**)(copy + RDG_RESOURCE_RHI_OFF) = ui_target;
                *(int32_t*)(copy + TEX_EXTENT_OFF) = ui_w;
                *(int32_t*)(copy + TEX_EXTENT_OFF + 4) = ui_h;
                g_lgui_swap.rdg_texture = a3;
                g_lgui_swap.shadow_copy = copy;
                g_lgui_swap.ui_target = ui_target;
                g_lgui_swap.rdg_texture_vtable = *(uintptr_t*)a3;
                g_lgui_swap.original_rhi = *(void**)((uintptr_t)a3 + RDG_RESOURCE_RHI_OFF);

                std::vector<std::pair<int32_t*, int32_t>> saved{};
                for (auto off : LGUI_RECT_MAX_OFFS_UI) {
                    auto max_x = (int32_t*)((uintptr_t)a2 + off);
                    auto min_x = max_x - 2;
                    if (*(min_x + 1) == 0 && (*max_x == hw || *max_x == hw * 2) && *(max_x + 1) == hh) {
                        saved.emplace_back(min_x, *min_x);
                        saved.emplace_back(max_x, *max_x);
                        saved.emplace_back(max_x + 1, *(max_x + 1));
                        *min_x = 0;
                        *max_x = ui_w;
                        *(max_x + 1) = ui_h;
                    }
                }

                if (n < 5 || (n % 300) == 0) {
                    SPDLOG_INFO("[LGUI_DRAW]   redirect -> ui_target={:x} {}x{} (was rhi={:x} {}x{}), rects patched={}",
                        (uintptr_t)ui_target, ui_w, ui_h, *(uintptr_t*)((uintptr_t)a3 + RDG_RESOURCE_RHI_OFF),
                        *(int32_t*)((uintptr_t)a3 + TEX_EXTENT_OFF), *(int32_t*)((uintptr_t)a3 + TEX_EXTENT_OFF + 4), saved.size() / 3);
                }

                const auto result = ((void*(*)(void*, void*, void*, void*, void*, void*))it->second)(self, a2, copy, a4, a5, a6);

                for (auto& [p, v] : saved) {
                    *p = v;
                }

                return result;
            }
        }

        if (n < 5) SPDLOG_INFO("[LGUI_DRAW]   redirect skipped: ui_target={:x}", (uintptr_t)ui_target);
    }

    constexpr bool LGUI_PER_EYE_DRAW = false;
    constexpr uint32_t LGUI_RECT_MAX_OFFS[] = {0x3e0, 0x410, 0x4a0, 0x4f8};

    // EXPERIMENT 8: persistent swap. RDG resolves the render target from FRDGTexture::GetRHI() (a3+0x10) when the pass is
    // *executed*, so we leave a3's RHI pointing at ui_target after this hook returns and restore it from the hooked
    // TRDGLambdaPass::Execute (vtable slot 1, installed below when the pass registry grows) once LGUI has drawn.
    constexpr bool LGUI_PERSIST_SWAP = false;

    if (LGUI_PERSIST_SWAP && g_hook != nullptr && g_hook->get_render_target_manager() != nullptr &&
        a2 != nullptr && !IsBadReadPtr(a2, 0x520) && a3 != nullptr && !IsBadReadPtr(a3, 0x80) && g_lgui_pass_execute_original != 0)
    {
        const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();

        if (ui_target != nullptr && !IsBadReadPtr(ui_target, TEX_EXTENT_OFF + 8)) {
            const auto ui_w = *(int32_t*)((uintptr_t)ui_target + TEX_EXTENT_OFF);
            const auto ui_h = *(int32_t*)((uintptr_t)ui_target + TEX_EXTENT_OFF + 4);
            const auto hw = (int32_t)VR::get()->get_hmd_width();
            const auto hh = (int32_t)VR::get()->get_hmd_height();

            if (ui_w >= 64 && ui_w <= 16384 && ui_h >= 64 && ui_h <= 16384) {
                std::vector<std::pair<int32_t*, int32_t>> saved{};
                for (auto off : LGUI_RECT_MAX_OFFS) {
                    auto max_x = (int32_t*)((uintptr_t)a2 + off);
                    auto min_x = max_x - 2;
                    if (*(min_x + 1) == 0 && (*max_x == hw || *max_x == hw * 2) && *(max_x + 1) == hh) {
                        saved.emplace_back(min_x, *min_x);
                        saved.emplace_back(max_x, *max_x);
                        saved.emplace_back(max_x + 1, *(max_x + 1));
                        *min_x = 0;
                        *max_x = ui_w;
                        *(max_x + 1) = ui_h;
                    }
                }

                auto& rhi_slot = *(void**)((uintptr_t)a3 + RDG_RESOURCE_RHI_OFF);
                g_lgui_swap.rdg_texture = a3;
                g_lgui_swap.original_rhi = rhi_slot;
                g_lgui_swap.ui_target = ui_target;
                g_lgui_swap.armed = true;
                rhi_slot = ui_target;

                // Extent too, in case RDG derives the viewport from the desc.
                const auto ext_x = (int32_t*)((uintptr_t)a3 + TEX_EXTENT_OFF);
                const auto saved_ext_x = *ext_x, saved_ext_y = *(ext_x + 1);
                *ext_x = ui_w;
                *(ext_x + 1) = ui_h;

                if (n < 5 || (n % 600) == 0) {
                    SPDLOG_INFO("[LGUI_SWAP] record: a3={:x} rhi {:x} -> ui_target={:x} {}x{} (was {}x{}), rects patched={}, fmt_dword={:x}",
                        (uintptr_t)a3, (uintptr_t)g_lgui_swap.original_rhi, (uintptr_t)ui_target, ui_w, ui_h, saved_ext_x, saved_ext_y,
                        saved.size() / 3, ((uint32_t*)a3)[LGUI_EXTENT_X_DWORD + 2]);
                }

                const auto result = ((void*(*)(void*, void*, void*, void*, void*, void*))it->second)(self, a2, a3, a4, a5, a6);

                for (auto& [p, v] : saved) {
                    *p = v;
                }

                *ext_x = saved_ext_x;
                *(ext_x + 1) = saved_ext_y;

                return result;
            }
        }
    }

    if (LGUI_PER_EYE_DRAW && a2 != nullptr && !IsBadReadPtr(a2, 0x520)) {
        const auto hw = (int32_t)VR::get()->get_hmd_width();
        const auto hh = (int32_t)VR::get()->get_hmd_height();
        std::vector<int32_t*> rects{};

        for (auto off : LGUI_RECT_MAX_OFFS) {
            auto max_x = (int32_t*)((uintptr_t)a2 + off);
            auto min_x = max_x - 2;
            if (*min_x == 0 && *max_x == hw && *(max_x + 1) == hh) {
                rects.push_back(max_x);
            }
        }

        if (n < 5) SPDLOG_INFO("[LGUI_DRAW]   per-eye draw: {} rects matched (hw={} hh={})", rects.size(), hw, hh);

        // EXPERIMENT 4 RESULT: each eye shows its half of a UI still laid out 2*hw wide -> the rect is only a viewport/scissor;
        // the layout width comes from the target texture itself (2*hw). Patching the RDG desc copy (a3 dword 21) did nothing,
        // so LGUI must read the RHI texture (a3[2]) size. EXPERIMENT 5: find every (2*hw, hh) pair in a3, a3[2], a4[2] and
        // present them as (hw, hh) for the duration of both eye draws.
        std::vector<std::pair<int32_t*, int32_t>> ext_patches{};
        auto patch_extents = [&](void* obj, uint32_t len, const char* tag) {
            if (obj == nullptr || IsBadReadPtr(obj, len)) return;
            std::string hits{};
            for (uint32_t off = 0; off + 8 <= len; off += 4) {
                auto p = (int32_t*)((uintptr_t)obj + off);
                if (*p == hw * 2 && *(p + 1) == hh) {
                    hits += fmt::format("{:x} ", off);
                    ext_patches.emplace_back(p, *p);
                    *p = hw;
                }
            }
            if (n < 5) SPDLOG_INFO("[LGUI_DRAW]   {} 2hw-extent hits: {}", tag, hits.empty() ? "<none>" : hits);
        };

        if (!rects.empty()) {
            // EXPERIMENT 5 RESULT: hits at a3+0x54 and a3[2]+0x54 but no visual change. RDG passes execute *after* this
            // hook returns, so anything restored here is invisible to the pass lambdas (the rects worked because they are
            // captured by value at AddPass time). EXPERIMENT 6: a3 is a transient per-graph FRDGTexture (address changes
            // every frame), so leave ITS extent patched for the whole graph; only restore the shared RHI texture (a3[2]).
            std::vector<std::pair<int32_t*, int32_t>> rhi_patches{};
            patch_extents(a3, 0x100, "a3(rdg)");
            const auto n_rdg = ext_patches.size();
            if (!IsBadReadPtr(a3, 0x20)) patch_extents((void*)((uintptr_t*)a3)[2], 0x200, "a3[2](rhi)");
            rhi_patches.assign(ext_patches.begin() + n_rdg, ext_patches.end());

            // Left eye: rects already [0, hw)
            ((void*(*)(void*, void*, void*, void*, void*, void*))it->second)(self, a2, a3, a4, a5, a6);

            // Right eye: [hw, 2hw)
            for (auto max_x : rects) { *(max_x - 2) = hw; *max_x = hw * 2; }
            const auto result = ((void*(*)(void*, void*, void*, void*, void*, void*))it->second)(self, a2, a3, a4, a5, a6);
            for (auto max_x : rects) { *(max_x - 2) = 0; *max_x = hw; }
            for (auto& [p, v] : rhi_patches) { *p = v; }
            return result;
        }
    }

    constexpr bool LGUI_PATCH_A2_SIZES = false;
    std::vector<std::pair<int32_t*, int32_t>> a2_patches{};

    if (LGUI_PATCH_A2_SIZES && a2 != nullptr && !IsBadReadPtr(a2, 0x520)) {
        const auto hw = (int32_t)VR::get()->get_hmd_width();
        const auto hh = (int32_t)VR::get()->get_hmd_height();
        if (hw > 0) {
            for (uint32_t off = 0x3d0; off < 0x510; off += 4) {
                auto p = (int32_t*)((uintptr_t)a2 + off);
                if (*p == hw && *(p + 1) == hh) {
                    a2_patches.emplace_back(p, *p);
                    *p = hw * 2;
                }
            }
            if (n < 5) SPDLOG_INFO("[LGUI_DRAW]   patched {} a2 X entries {} -> {}", a2_patches.size(), hw, hw * 2);
        }
    }

    // DIAG 7: the record-time patches on a3 are invisible to the pass lambdas (they re-read the FRDGTexture at execute time).
    // To swap the target at execute time we need the FRDGPass LGUI appends. Locate the FRDGBuilder by finding a
    // TArray<FRDGTexture*> that contains a3 (texture registry), then diff every pointer-TArray in that object across the
    // original call: the array that grows is the pass registry and the new entries are LGUI's passes.
    constexpr bool LGUI_FIND_RDG_PASS = false;
    struct ArrSnap { uintptr_t owner; uint32_t a2_off; uint32_t off; int32_t num; };
    std::vector<ArrSnap> arr_snaps{};
    auto dump_qwords_rdg = [](void* p, int count) -> std::string {
        std::string s{};
        if (p == nullptr || IsBadReadPtr(p, sizeof(uintptr_t) * count)) return "<bad>";
        for (int k = 0; k < count; ++k) s += fmt::format("{:x},", ((uintptr_t*)p)[k]);
        return s;
    };

    // The first calls happen during load (no canvases -> no passes added), so sample periodically instead.
    static uint32_t rdg_samples = 0;
    const bool rdg_sample_now = LGUI_FIND_RDG_PASS && g_lgui_pass_execute_original == 0 && rdg_samples < 6 && n >= 300 && (n % 300) == 0;

    // Targeted: pass registry is at (*(a2+0x378))+0x1168 (TArray<FRDGPass*>). Snapshot every call until Execute is hooked.
    constexpr uint32_t LGUI_PASS_OWNER_OFF = 0x378;
    constexpr uint32_t LGUI_PASS_ARR_OFF = 0x1168;
    std::optional<std::pair<uintptr_t, int32_t>> pass_arr_snap{};

    // DIAG 9: the vtable we hooked (276032d0) turned out to be a generic lambda pass with no captured state, so the LGUI
    // draw pass was never observed. Diff *every* pointer-TArray inside the builder block itself on every 10th call
    // (cheap: a2 is one contiguous 0x20000 block) and report all new entries, including their captured state, so we can
    // pick the vtable that actually carries LGUI's renderer/canvas pointers.
    // The registries are NOT inside a2 itself: a2 is the builder shell and the pass/texture registries live in separate
    // allocations pointed to from a2 (e.g. *(a2+0x378)+0x1168 grew 2->4 in the earlier diff). Scan a2 plus every
    // allocation referenced from a2's first 0x1000 bytes (each 0x2000), like the earlier diff that worked.
    // DISABLED: this scanner treats arbitrary heap qwords inside/reachable from a2 as TArray<T*> candidates
    // (data/num/max heuristics) and dereferences them again *after* the original call returns to diff their
    // length. It served its purpose (finding the pass-registry offsets used by LGUI_PATCH_A3_REFS above) but is
    // pure diagnostic instrumentation with no effect on the actual UI redirect. When a nested submenu closes
    // mid-frame the FRDGBuilder (a2) or an allocation it points to can be freed/reallocated between the
    // pre-call snapshot and this post-call diff, causing an access violation *inside IsBadReadPtr itself*
    // (observed fault module=KERNEL32.DLL, not the game exe) that the generalized game-exe UAF recovery above
    // correctly refuses to patch - and that unhandled AV was what hung the game (present-stall loop) rather
    // than crashing outright. Leave disabled now that the offsets have been captured.
    constexpr bool LGUI_DIFF_BUILDER_EVERY_FRAME = false;
    static uint32_t builder_diff_reports = 0;
    struct BuilderArr { uintptr_t owner; uint32_t a2_off; uint32_t off; uintptr_t data; int32_t num; };
    std::vector<BuilderArr> builder_arrs{};
    const bool builder_diff_now = LGUI_DIFF_BUILDER_EVERY_FRAME && builder_diff_reports < 12 && n >= 20 && (n % 20) == 0 &&
        a2 != nullptr && !IsBadReadPtr(a2, 0x1000);

    if (builder_diff_now) {
        builder_arrs.reserve(1024);
        std::vector<std::tuple<uintptr_t, uint32_t, uint32_t>> owners{};
        uint32_t a2_len = 0;
        while (a2_len < 0x20000 && !IsBadReadPtr((void*)((uintptr_t)a2 + a2_len), 0x1000)) a2_len += 0x1000;
        owners.emplace_back((uintptr_t)a2, 0xFFFFFFFF, a2_len);
        for (uint32_t off = 0; off < 0x1000; off += 8) {
            const auto p = *(uintptr_t*)((uintptr_t)a2 + off);
            if (p > 0x10000 && (p & 7) == 0 && p != (uintptr_t)a2 && !IsBadReadPtr((void*)p, 0x2000)) owners.emplace_back(p, off, 0x2000);
        }
        for (const auto& [c, a2_off, len] : owners) {
            for (uint32_t off = 0; off + 16 <= len; off += 8) {
                const auto data = *(uintptr_t*)(c + off);
                const auto num = *(int32_t*)(c + off + 8);
                const auto max = *(int32_t*)(c + off + 12);
                if (data < 0x10000 || (data & 7) != 0 || num < 0 || num > 16384 || max < num || max == 0 || max > 65536) continue;
                builder_arrs.push_back({c, a2_off, off, data, num});
            }
        }
    }

    if (a2 != nullptr && !IsBadReadPtr((void*)((uintptr_t)a2 + LGUI_PASS_OWNER_OFF), 8)) {
        const auto owner = *(uintptr_t*)((uintptr_t)a2 + LGUI_PASS_OWNER_OFF);
        if (owner > 0x10000 && !IsBadReadPtr((void*)(owner + LGUI_PASS_ARR_OFF), 16)) {
            pass_arr_snap = std::make_pair(owner + LGUI_PASS_ARR_OFF, *(int32_t*)(owner + LGUI_PASS_ARR_OFF + 8));
            if (n < 20 || n % 20 == 0) SPDLOG_INFO("[LGUI_SWAP] pass registry snapshot: owner={:x} num={}", owner, pass_arr_snap->second);
        } else if (n < 20 || n % 20 == 0) {
            SPDLOG_INFO("[LGUI_SWAP] pass registry owner unreadable: a2+378={:x}", owner);
        }
    }

    if (rdg_sample_now && a2 != nullptr && a3 != nullptr && !IsBadReadPtr(a2, 0x1000)) {
        ++rdg_samples;
        // a2 first: find how far it is readable (page-wise) so we cover the entire FRDGBuilder.
        uint32_t a2_len = 0;
        while (a2_len < 0x20000 && !IsBadReadPtr((void*)((uintptr_t)a2 + a2_len), 0x1000)) a2_len += 0x1000;
        SPDLOG_INFO("[LGUI_RDG] a2={:x} readable={:x}", (uintptr_t)a2, a2_len);

        std::vector<std::tuple<uintptr_t, uint32_t, uint32_t>> cands{{(uintptr_t)a2, 0xFFFFFFFF, a2_len}};

        for (uint32_t off = 0; off < 0x1000; off += 8) {
            const auto p = *(uintptr_t*)((uintptr_t)a2 + off);
            if (p > 0x10000 && (p & 7) == 0 && p != (uintptr_t)a2 && !IsBadReadPtr((void*)p, 0x2000)) {
                cands.emplace_back(p, off, 0x2000);
            }
        }

        for (const auto& [c, a2_off, len] : cands) {
            for (uint32_t off = 0; off + 16 <= len; off += 8) {
                const auto data = *(uintptr_t*)(c + off);
                const auto num = *(int32_t*)(c + off + 8);
                const auto max = *(int32_t*)(c + off + 12);

                if (data < 0x10000 || (data & 7) != 0 || num < 0 || num > 8192 || max < num || max > 65536 || max == 0 || IsBadReadPtr((void*)data, sizeof(uintptr_t) * max)) {
                    continue;
                }

                bool has_a3 = false;
                for (int32_t k = 0; k < num; ++k) {
                    if (((uintptr_t*)data)[k] == (uintptr_t)a3) { has_a3 = true; break; }
                }

                if (has_a3) {
                    SPDLOG_INFO("[LGUI_RDG] texture registry containing a3: owner={:x} (a2+{:x}) arr@+{:x} num={} max={}", c, a2_off, off, num, max);
                }

                arr_snaps.push_back({c, a2_off, off, num});
            }
        }

        SPDLOG_INFO("[LGUI_RDG] {} candidate owners, {} pointer arrays snapshotted", cands.size(), arr_snaps.size());
    }

    const auto result = ((void*(*)(void*, void*, void*, void*, void*, void*))it->second)(self, a2, a3, a4, a5, a6);

    if (builder_diff_now) {
        bool reported = false;
        for (const auto& s : builder_arrs) {
            if (IsBadReadPtr((void*)(s.owner + s.off), 16)) continue;
            const auto data = *(uintptr_t*)(s.owner + s.off);
            const auto num = *(int32_t*)(s.owner + s.off + 8);
            if (num <= s.num || num - s.num > 256 || data < 0x10000 || (data & 7) != 0 || IsBadReadPtr((void*)data, sizeof(uintptr_t) * num)) continue;

            reported = true;
            SPDLOG_INFO("[LGUI_PASS] owner={:x} (a2+{:x}) arr@+{:x}: {} -> {} (data {:x}{})", s.owner, s.a2_off, s.off, s.num, num, data, data != s.data ? " realloc" : "");

            for (int32_t k = s.num; k < std::min(num, s.num + 12); ++k) {
                const auto e = ((uintptr_t*)data)[k];
                if (e < 0x10000 || (e & 7) != 0 || IsBadReadPtr((void*)e, 0x100)) {
                    SPDLOG_INFO("[LGUI_PASS]   [{}]={:x}", k, e);
                    continue;
                }
                const auto vt = *(uintptr_t*)e;
                const auto m = utility::get_module_within((void*)vt);
                std::string q{};
                int nonzero = 0;
                for (int j = 0; j < 32; ++j) {
                    const auto v = ((uintptr_t*)e)[j];
                    if (v != 0) ++nonzero;
                    q += fmt::format("{:x},", v);
                }
                std::string tags{};
                for (int j = 0; j < 32; ++j) {
                    const auto v = ((uintptr_t*)e)[j];
                    if (v == (uintptr_t)a3) tags += fmt::format("[{}]=a3 ", j);
                    else if (v == (uintptr_t)a4) tags += fmt::format("[{}]=a4 ", j);
                    else if (v == (uintptr_t)self) tags += fmt::format("[{}]=self ", j);
                    else if (v == (uintptr_t)a2) tags += fmt::format("[{}]=a2 ", j);
                }
                SPDLOG_INFO("[LGUI_PASS]   [{}]={:x} vt_rva={:x} nonzero={} tags={} q=[{}]", k, e,
                    m.has_value() ? vt - (uintptr_t)*m : 0, nonzero, tags.empty() ? "-" : tags, q);
            }
        }
        if (reported) ++builder_diff_reports;
        static uint32_t diff_runs = 0;
        if ((diff_runs++ % 30) == 0) SPDLOG_INFO("[LGUI_PASS] diff run #{} arrays={} grew_any={}", diff_runs, builder_arrs.size(), reported);
    }

    if (pass_arr_snap.has_value()) {
        const auto [arr, old_num] = *pass_arr_snap;
        if (!IsBadReadPtr((void*)arr, 16)) {
            const auto data = *(uintptr_t*)arr;
            const auto num = *(int32_t*)(arr + 8);
            if (n < 20 || n % 20 == 0) SPDLOG_INFO("[LGUI_SWAP] pass registry after call: {} -> {}", old_num, num);
            if (num > old_num && num - old_num <= 64 && data > 0x10000 && !IsBadReadPtr((void*)data, sizeof(uintptr_t) * num)) {
                // Pick the first new entry whose vtable is in the game module (the registry also contains sentinel values).
                for (int32_t k = old_num; k < num; ++k) {
                    const auto e = ((uintptr_t*)data)[k];
                    if (e > 0x10000 && !IsBadReadPtr((void*)e, 0x10) && utility::get_module_within((void*)*(uintptr_t*)e).has_value()) {
                        SPDLOG_INFO("[LGUI_SWAP] pass registry grew {} -> {}, hooking entry [{}]={:x}", old_num, num, k, e);
                        lgui_try_hook_pass_vtable(e);
                        break;
                    }
                }
            }
        }
    }

    for (const auto& s : arr_snaps) {
        if (IsBadReadPtr((void*)(s.owner + s.off), 16)) continue;
        const auto data = *(uintptr_t*)(s.owner + s.off);
        const auto num = *(int32_t*)(s.owner + s.off + 8);

        if (num <= s.num || num - s.num > 512 || data < 0x10000 || IsBadReadPtr((void*)data, sizeof(uintptr_t) * num)) {
            continue;
        }

        SPDLOG_INFO("[LGUI_RDG] array grew: owner={:x} (a2+{:x}) arr@+{:x} {} -> {}", s.owner, s.a2_off, s.off, s.num, num);

        for (int32_t k = s.num; k < std::min(num, s.num + 8); ++k) {
            const auto e = ((uintptr_t*)data)[k];
            if (e < 0x10000 || IsBadReadPtr((void*)e, 0x80)) {
                SPDLOG_INFO("[LGUI_RDG]   [{}]={:x} <unreadable>", k, e);
                continue;
            }

            const auto vt = *(uintptr_t*)e;
            const auto m = utility::get_module_within((void*)vt);
            std::string slots{};
            if (m.has_value() && !IsBadReadPtr((void*)vt, sizeof(uintptr_t) * 4)) {
                for (int j = 0; j < 4; ++j) {
                    const auto fn = ((uintptr_t*)vt)[j];
                    const auto fm = utility::get_module_within((void*)fn);
                    slots += fmt::format("{:x},", fm.has_value() ? fn - (uintptr_t)*fm : fn);
                }
            }

            // FRDGPass: [0]=vtable, [1]=FRDGEventName (const TCHAR* in non-shipping, may be null/garbage in shipping)
            std::string name{"<none>"};
            const auto name_ptr = ((uintptr_t*)e)[1];
            if (name_ptr > 0x10000 && !IsBadReadPtr((void*)name_ptr, 64)) {
                const auto wn = (const wchar_t*)name_ptr;
                const auto len = wcsnlen(wn, 32);
                bool printable = len > 0;
                for (size_t q = 0; q < len && printable; ++q) printable = wn[q] >= 0x20 && wn[q] < 0x7f;
                if (printable) name = utility::narrow(std::wstring{wn, len});
            }

            SPDLOG_INFO("[LGUI_RDG]   [{}]={:x} vt_rva={:x} slots=[{}] name=\"{}\" q=[{}]", k, e,
                m.has_value() ? vt - (uintptr_t)*m : vt, slots, name, dump_qwords_rdg((void*)e, 12));
        }
    }

    for (auto& [p, v] : a2_patches) {
        *p = v;
    }

    if (patched) {
        ((uint32_t*)a3)[LGUI_EXTENT_X_DWORD] = saved_extent_x;
    }

    return result;
}

static void diag_dump_engine_view_extensions(sdk::FSceneViewFamily& view_family) {
    static uint32_t call_count = 0;
    static uint32_t dump_count = 0;

    // NOTE: despite the name, this function has a CRITICAL side effect - it installs the LGUI redirect
    // hooks (slot-24 draw hook + game-thread SetupView/BeginRenderViewFamily hooks) below. It must keep
    // running every qualifying call. Only the verbose [VIEWEXT_DIAG] logging is gated by
    // LGUI_DIAG_STEADY_STATE; the hook-installation path is NOT gated.
    if (dump_count >= 20 || (call_count++ % 300) != 0) {
        return;
    }

    ++dump_count;

    const auto exts = g_engine_view_extensions;

    if (exts == nullptr) {
        SPDLOG_INFO("[VIEWEXT_DIAG] g_engine_view_extensions is null");
        return;
    }

    const auto views = view_family.get_views();
    const auto rt = view_family.get_render_target();

    if (LGUI_DIAG_STEADY_STATE) {
        SPDLOG_INFO("[VIEWEXT_DIAG] dump #{}: family={:x} rt={:x} views={} exts.data={:x} count={} capacity={} our_vtable={:x} idx(is_active={} begin_render={} pre_render_rt={})",
            dump_count, (uintptr_t)&view_family, (uintptr_t)rt, views != nullptr ? views->count : -1,
            (uintptr_t)exts->extensions.data, exts->extensions.count, exts->extensions.capacity, (uintptr_t)g_view_extension_vtable.data(),
            SceneViewExtensionAnalyzer::is_active_this_frame_index, SceneViewExtensionAnalyzer::begin_render_viewfamily_index, SceneViewExtensionAnalyzer::pre_render_viewfamily_renderthread_index);
    }

    if (exts->extensions.data == nullptr || exts->extensions.count <= 0 || exts->extensions.count > 64) {
        return;
    }

    for (int32_t i = 0; i < exts->extensions.count; ++i) try {
        const auto ext = exts->extensions.data[i].reference;

        if (ext == nullptr || IsBadReadPtr((void*)ext, sizeof(void*))) {
            SPDLOG_INFO("[VIEWEXT_DIAG]   [{}] ext={:x} (invalid)", i, (uintptr_t)ext);
            continue;
        }

        const auto vtable = *(uintptr_t*)ext;
        const auto is_ours = vtable == (uintptr_t)g_view_extension_vtable.data();
        const auto module = utility::get_module_within((void*)vtable);
        std::string module_name = "<none>";
        uintptr_t rva = 0;

        if (module.has_value()) {
            if (const auto path = utility::get_module_path(*module); path.has_value()) {
                module_name = std::filesystem::path(*path).filename().string();
            }

            rva = vtable - (uintptr_t)*module;
        }

        // First few virtuals (RVA relative to module) so the user can cross-reference in a disassembler
        std::string fn_rvas{};
        for (auto j = 0; j < 6; ++j) {
            const auto fn = ((uintptr_t*)vtable)[j];
            if (fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) {
                fn_rvas += "?,";
                continue;
            }

            const auto fn_module = utility::get_module_within((void*)fn);
            fn_rvas += fmt::format("{:x},", fn_module.has_value() ? fn - (uintptr_t)*fn_module : fn);
        }

        if (LGUI_DIAG_STEADY_STATE) {
            SPDLOG_INFO("[VIEWEXT_DIAG]   [{}] ext={:x} vtable={:x} module={} vtable_rva={:x} ours={} fn_rvas=[{}]",
                i, (uintptr_t)ext, vtable, module_name, rva, is_ours, fn_rvas);
        }
        // One-time deep scan: list every virtual that isn't the shared default stub
        // LGUI's FLGUIHudRenderer registers late (after dump #1); its vtables sit next to the
        // "LGUIHudRenderer::AddHudPrimitive_RenderThread" string at rva 26ca09xx.
        // 26ca0830 = main menu drawer (destroyed on world transition), 26ca0760 = interaction/canvas,
        // 2769cd58 = in-game class that replaces [10] after loading (fully overridden virtuals).
        const bool is_lgui_candidate = rva == 0x26ca0830 || rva == 0x26ca0760;

        if (!is_ours && module.has_value() && (dump_count == 1 || is_lgui_candidate)) {
            // The default no-op virtuals all resolve to a single shared stub; detect it as the most frequent entry
            std::unordered_map<uintptr_t, int> freq{};
            for (auto j = 0; j < 40; ++j) {
                const auto fn = ((uintptr_t*)vtable)[j];
                if (fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) break;
                ++freq[fn];
            }

            uintptr_t stub = 0;
            int stub_count = 0;
            for (const auto& [fn, n] : freq) {
                if (n > stub_count) { stub = fn; stub_count = n; }
            }

            // Classes that override everything (e.g. 2769cd58) have no dominant stub; fall back to the
            // engine's shared ISceneViewExtension no-op seen on every other extension.
            if (stub_count < 3) {
                stub = (uintptr_t)*module + 0x202d0120;
                stub_count = 0;
            }

            std::string nontrivial{};
            for (auto j = 0; j < 40; ++j) {
                const auto fn = ((uintptr_t*)vtable)[j];
                if (fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) {
                    break;
                }

                const auto fn_module = utility::get_module_within((void*)fn);
                if (!fn_module.has_value() || *fn_module != *module) {
                    break;
                }

                if (fn == stub) {
                    continue;
                }

                const auto first_byte = *(uint8_t*)fn;
                if (first_byte == 0xC3) {
                    continue;
                }

                nontrivial += fmt::format("{}:{:x},", j, fn - (uintptr_t)*module);
            }

            if (LGUI_DIAG_STEADY_STATE) {
                SPDLOG_INFO("[VIEWEXT_DIAG]     [{}] stub_rva={:x} (x{}) nontrivial_virtuals=[{}]", i, stub - (uintptr_t)*module, stub_count, nontrivial);
            }

            // Experiment: replace virtuals with the extension's own no-op stub to confirm which
            // extension/slot draws the UI (it should vanish). Bisect mode: suppress every overridden
            // slot in [suppress_slot_min, suppress_slot_max] for every extension in the mask.
            // Already ruled out individually: 2:11, 1:12, 4:12.
            constexpr uint32_t suppress_ext_mask = 0; // bit i = extension index i
            constexpr int32_t suppress_slot_min = 1;
            constexpr int32_t suppress_slot_max = 39;
            // LGUI drawer (vtable rva 26ca0830) confirmed: suppressing all of its overrides removed the UI.
            // Slot 17 is IsActiveThisFrame so that run only proved the extension, not the draw slot.
            // Its real overrides are 17 (is_active), 24 (205a6830), 25 (205ad5c0); bisect 24/25 one at a time.
            // 24, 25, 24+25, 2+13 all ruled out (UI stayed). Only the original all-slot run removed it, so the
            // draw must be in the slots we assumed were engine defaults (15,16,18,22,23) or gated purely by 17.
            // {15,16,18,22,23} removed the UI. 18/22/23 share 202d0320 (one impl); 15=202d01c0, 16=202d01f0.
            // {18,22,23} on [10] alone killed the MAIN MENU UI only; in-game HUD stayed. Vtable patches are per-class,
            // so [9] (26ca0760, same 202d0320 at 18/22) is likely the HUD drawer. Suppress both classes now.
            // In-game, [10] is replaced by class 2769cd58 (all virtuals overridden) which kept drawing the HUD.
            // Run 2: 2769cd58 never appeared, only 26ca0830/26ca0760 exist in-game (both patched at 18/22/23),
            // yet the HUD still draws. All bisects so far were at the main menu; in-game the HUD may go through
            // a different slot (24/25 etc.). Suppress EVERY override (empty list = all) to test if it's these classes at all.
            // Suppressing ALL overrides on both classes removed menu + in-game HUD. Now bisect in-game:
            // menu = 18/22/23; HUD candidates = 24 (205a67a0/205a6830 per-class), 25 (205ad5c0 shared).
            // {24,25} on 26ca0760+26ca0830 (+2769cd58) removed menu AND in-game HUD. Split: test 25 (shared 205ad5c0) alone,
            // and leave 2769cd58 untouched (its layout doesn't look like ISceneViewExtension; slot 3 is unaligned).
            // 25 alone on both classes: UI stayed. So slot 24 (per-class 205a67a0 / 205a6830) is the draw path. CONFIRMED.
            // Now: pass-through hook on slot 24 to observe args/callers instead of stubbing it.
            if (is_lgui_candidate && !g_lgui_slot24_originals.contains(vtable)) {
                auto slot_ptr = &((uintptr_t*)vtable)[LGUI_DRAW_SLOT];
                const auto fn = *slot_ptr;
                DWORD old{};

                if (fn != 0 && fn != stub && VirtualProtect(slot_ptr, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                    g_lgui_slot24_originals[vtable] = fn;
                    *slot_ptr = (uintptr_t)&lgui_slot24_hook;
                    VirtualProtect(slot_ptr, sizeof(uintptr_t), old, &old);
                    SPDLOG_INFO("[LGUI_DRAW] hooked slot {} on vtable_rva={:x} (orig fn_rva={:x})", LGUI_DRAW_SLOT, rva, fn - (uintptr_t)*module);
                } else {
                    SPDLOG_ERROR("[LGUI_DRAW] failed to hook slot {} on vtable_rva={:x}", LGUI_DRAW_SLOT, rva);
                }

                // Game-thread callbacks: hook whatever this class overrides (the shared engine stub is skipped).
                auto hook_gt_slot = [&](int32_t slot, uintptr_t hook_fn, std::unordered_map<uintptr_t, uintptr_t>& originals, const char* name) {
                    if (originals.contains(vtable)) return;
                    auto sp = &((uintptr_t*)vtable)[slot];
                    const auto f = *sp;
                    if (f == 0 || f == stub || IsBadReadPtr((void*)f, sizeof(void*))) {
                        SPDLOG_INFO("[LGUI_GT] slot {} ({}) on vtable_rva={:x} is stub/empty, not hooked", slot, name, rva);
                        return;
                    }
                    const auto fm = utility::get_module_within((void*)f);
                    if (!fm.has_value() || *fm != *module) return;
                    DWORD o{};
                    if (VirtualProtect(sp, sizeof(uintptr_t), PAGE_READWRITE, &o)) {
                        originals[vtable] = f;
                        *sp = hook_fn;
                        VirtualProtect(sp, sizeof(uintptr_t), o, &o);
                        SPDLOG_INFO("[LGUI_GT] hooked slot {} ({}) on vtable_rva={:x} (orig fn_rva={:x})", slot, name, rva, f - (uintptr_t)*module);
                    }
                };
                hook_gt_slot(LGUI_SETUP_VIEW_SLOT, (uintptr_t)&lgui_setup_view_hook, g_lgui_setup_view_originals, "SetupView");
                hook_gt_slot(LGUI_BEGIN_FAMILY_SLOT, (uintptr_t)&lgui_begin_family_hook, g_lgui_begin_family_originals, "BeginRenderViewFamily");
            }

            constexpr std::array<int32_t, 0> lgui_suppress_slots{};
            static std::unordered_set<uintptr_t> lgui_suppressed_vtables{};
            const bool suppress_this = false;

            if ((suppress_this || ((suppress_ext_mask >> i) & 1) != 0) && stub != 0) {
                if (suppress_this) {
                    lgui_suppressed_vtables.insert(rva);
                }

                std::string suppressed{};

                for (auto s = suppress_slot_min; s <= suppress_slot_max && s < 40; ++s) {
                    if (suppress_this && !lgui_suppress_slots.empty() && std::find(lgui_suppress_slots.begin(), lgui_suppress_slots.end(), s) == lgui_suppress_slots.end()) {
                        continue;
                    }

                    auto slot_ptr = &((uintptr_t*)vtable)[s];
                    const auto fn = *slot_ptr;

                    if (fn == stub || fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) {
                        continue;
                    }

                    // Only touch this module's overrides, never the shared engine defaults (rva 202d01xx/202d03xx)
                    const auto fn_module = utility::get_module_within((void*)fn);
                    if (!fn_module.has_value() || *fn_module != *module) {
                        continue;
                    }

                    DWORD old{};
                    if (VirtualProtect(slot_ptr, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                        *slot_ptr = stub;
                        VirtualProtect(slot_ptr, sizeof(uintptr_t), old, &old);
                        suppressed += fmt::format("{}:{:x},", s, fn - (uintptr_t)*module);
                    } else {
                        SPDLOG_ERROR("[VIEWEXT_DIAG]     [{}] VirtualProtect failed for slot {}", i, s);
                    }
                }

                if (!suppressed.empty()) {
                    SPDLOG_INFO("[VIEWEXT_DIAG]     [{}] SUPPRESSED slots [{}]", i, suppressed);
                }
            }
        }
    } catch (...) {
        SPDLOG_ERROR("[VIEWEXT_DIAG]   [{}] exception while reading extension", i);
    }
}

// Analyzes all of the virtual functions for ISceneViewExtension
// We create the ISceneViewExtension ourselves and overwrite all of the virtual functions
// The class will count how many times each virtual is getting called
// and then when a threshold is reached, it finds the most called one
// the most called one is IsActiveThisFrame which we need to activate the ISceneViewExtension

template<int N>
void SceneViewExtensionAnalyzer::FillVtable<N>::fill(std::array<uintptr_t, 50>& table) {
    table[N] = (uintptr_t)&SceneViewExtensionAnalyzer::analysis_dummy_stage1<N>;
    FillVtable<N - 1>::fill(table);
}

template<int N>
void SceneViewExtensionAnalyzer::FillVtable<N>::fill2(std::array<uintptr_t, 50>& table) {
    table[N] = (uintptr_t)&SceneViewExtensionAnalyzer::analysis_dummy_stage2<N>;
    FillVtable<N - 1>::fill2(table);
}

// 4.25something to 4.27
// TODO: Add support for all versions via PDB dumps
constexpr auto INIT_OPTIONS_OFFSET = 0x50;

// Cross-reference resolver for FSceneViewInitOptions / FSceneView offsets.
// The SDK's vtable-walk heuristic lands on the wrong fields in this game (get_view_family() returns
// a non-pointer like 0xf7d602). After the real constructor has run we hold BOTH the raw init options
// and the fully constructed FSceneView, so we can find the fields by intersection:
//  - Family:  pointer present in both blocks whose first TArray contains this very view
//  - State:   pointer present in both blocks with a module-resident vtable, distinct from family
//  - StereoPass: uint32 that reads 1 for the first eye view of a frame and 2 for the second, in both blocks
namespace sceneview_xref {
struct Snapshot {
    std::array<uint8_t, 0x200> init_options{};
    std::array<uint8_t, 0x1000> view{};
};

static constexpr size_t snap_init_size() { return sizeof(Snapshot::init_options); }

static inline bool resolved{false};
static inline uint32_t attempts{0};
static inline std::optional<uint32_t> live_view_family_offset{};
static inline std::optional<uint32_t> live_stereo_pass_offset{};
// Engine-specific encodings of the eye passes as observed in view[0]/view[1]. Stock 4.25+ is 1/2, this
// game uses 2/3 (an extra leading enum value).
static inline uint32_t stereo_pass_left{1};
static inline uint32_t stereo_pass_right{2};
// Every dword (< 8) that differs between the two eye views: StereoPass, its cached copies, view index,
// "is primary" style flags. Used by the Pass2 "full eye identity" test to make the right-eye view
// carry the left eye's metadata for the duration of its render.
struct EyeField { uint32_t offset; uint32_t left; uint32_t right; };
static inline std::vector<EyeField> eye_fields{};
// Offset of FSceneViewStateInterface* State inside the constructed FSceneView. Found by locating the
// init options' state pointer (already resolved) inside the live view after the real constructor ran.
static inline std::optional<uint32_t> live_scene_state_offset{};

static bool plausible(uintptr_t p);
static bool has_module_vtable(uintptr_t p);

static inline void resolve_live_scene_state(sdk::FSceneView* view, void* init_state) {
    if (live_scene_state_offset.has_value() || view == nullptr || init_state == nullptr) {
        return;
    }

    // Reject non-canonical garbage (e.g. 0x100000000 read through a not-yet-corrected init_options
    // offset) and anything that is not a vtable'd object; matching it would pin the wrong offset.
    if (!has_module_vtable((uintptr_t)init_state)) {
        SPDLOG_INFO_ONCE("[VR] sceneview_xref: ignoring implausible init scene state {:x} for offset resolution", (uintptr_t)init_state);
        return;
    }

    for (uint32_t off = 0; off + sizeof(void*) <= 0x400; off += sizeof(void*)) {
        if (*(void**)((uintptr_t)view + off) == init_state) {
            live_scene_state_offset = off;
            SPDLOG_INFO("[VR] sceneview_xref: RESOLVED live scene state@{:x} (state={:x}) [view+0x0={:x} view+0x8={:x}]", off, (uintptr_t)init_state,
                *(uintptr_t*)((uintptr_t)view + 0x0), *(uintptr_t*)((uintptr_t)view + 0x8));
            return;
        }
    }
}

static bool plausible(uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFFFFFF && (p % sizeof(void*)) == 0 && !IsBadReadPtr((void*)p, 0x40);
}

static bool has_module_vtable(uintptr_t p) {
    if (!plausible(p)) return false;
    const auto vt = *(uintptr_t*)p;
    if (!plausible(vt) || !utility::get_module_within((void*)vt)) return false;
    const auto fn = *(uintptr_t*)vt;
    return fn != 0 && !IsBadReadPtr((void*)fn, sizeof(void*)) && utility::get_module_within((void*)fn).has_value();
}

static std::optional<uint32_t> find_ptr(const uint8_t* block, size_t size, uintptr_t value) {
    for (uint32_t i = 0; i + sizeof(void*) <= size; i += sizeof(void*)) {
        if (*(const uintptr_t*)(block + i) == value) return i;
    }
    return std::nullopt;
}

// Called from begin_render_viewfamily_real where we hold a VALIDATED FSceneViewFamily* and both
// constructed eye views. Locate the live FSceneView fields by value:
//  - Family: the offset in view[0] (and view[1]) holding exactly view_family
//  - StereoPass: uint32 that is 1 in view[0] and 2 in view[1]
static void resolve_live(sdk::FSceneViewFamily* family, sdk::FSceneView* v0, sdk::FSceneView* v1) {
    if (live_view_family_offset && live_stereo_pass_offset) return;
    if (family == nullptr || v0 == nullptr || v1 == nullptr) return;
    if (IsBadReadPtr(v0, 0x1000) || IsBadReadPtr(v1, 0x1000)) return;

    static uint32_t live_attempts = 0;
    if (++live_attempts > 300) return;

    if (!live_view_family_offset) {
        for (uint32_t i = 0; i + sizeof(void*) <= 0x1000; i += sizeof(void*)) {
            if (*(uintptr_t*)((uintptr_t)v0 + i) == (uintptr_t)family && *(uintptr_t*)((uintptr_t)v1 + i) == (uintptr_t)family) {
                live_view_family_offset = i;
                SPDLOG_INFO("[VR] sceneview_xref: RESOLVED live view family@{:x} (family={:x})", i, (uintptr_t)family);
                break;
            }
        }
    }

    if (!live_stereo_pass_offset) {
        // Strict stock encoding first (1 -> 2), then any consecutive small-int pair (a -> a+1, a >= 1) for
        // engines with a shifted enum. Prefer the lowest offset: the enum lives in the FSceneView header
        // section, copies deeper in the struct (uniform parameter caches) come later.
        std::vector<uint32_t> hits{};
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> loose{};
        for (uint32_t i = 0; i + 4 <= 0x1000; i += 4) {
            const auto a = *(uint32_t*)((uintptr_t)v0 + i);
            const auto b = *(uint32_t*)((uintptr_t)v1 + i);
            if (a == 1 && b == 2) hits.push_back(i);
            else if (a >= 1 && a < 8 && b == a + 1) loose.emplace_back(i, a, b);
        }

        std::string s{};
        for (auto h : hits) s += fmt::format("{:x} ", h);

        if (live_attempts <= 3 || live_attempts % 120 == 0) {
            SPDLOG_INFO("[VR] sceneview_xref: live stereo pass candidates (v0==1 && v1==2): [{}] family_off={:x}", s, live_view_family_offset.value_or(0xFFFFFFFF));

            // No strict 1/2 hit: this engine may use a different enum (pre-4.25 LEFT/RIGHT_EYE(_SIDE),
            // a custom one, or a uint8). Dump every dword that differs between the two eye views while
            // both are small integers - StereoPass MUST be among these since the views only differ in
            // eye-specific data (pass, matrices, rects).
            if (hits.empty()) {
                std::string diffs{};
                uint32_t n = 0;
                for (uint32_t i = 0; i + 4 <= 0x1000 && n < 48; i += 4) {
                    const auto a = *(uint32_t*)((uintptr_t)v0 + i);
                    const auto b = *(uint32_t*)((uintptr_t)v1 + i);
                    if (a != b && a < 0x100 && b < 0x100) {
                        diffs += fmt::format("{:x}:{}->{} ", i, a, b);
                        ++n;
                    }
                }
                SPDLOG_INFO("[VR] sceneview_xref: small-int dwords differing v0->v1: {}", diffs);
            }
        }

        // Standard FSceneView puts StereoPass within a few hundred bytes after Family/State/Drawer; take the
        // first hit after the family offset if known, otherwise the first hit at all.
        if (!hits.empty()) {
            live_stereo_pass_offset = hits.front();
            if (live_view_family_offset) {
                for (auto h : hits) {
                    if (h > *live_view_family_offset) { live_stereo_pass_offset = h; break; }
                }
            }
            stereo_pass_left = 1;
            stereo_pass_right = 2;
            SPDLOG_INFO("[VR] sceneview_xref: RESOLVED live stereo pass@{:x} ({} candidates, stock 1/2 encoding)", *live_stereo_pass_offset, hits.size());
        } else if (!loose.empty()) {
            const auto& [off, a, b] = loose.front();
            live_stereo_pass_offset = off;
            stereo_pass_left = a;
            stereo_pass_right = b;
            SPDLOG_INFO("[VR] sceneview_xref: RESOLVED live stereo pass@{:x} with engine-specific encoding left={} right={} ({} loose candidates)",
                off, a, b, loose.size());
        }

        if (live_stereo_pass_offset) {
            eye_fields.clear();
            std::string s2{};
            std::string skipped{};
            for (uint32_t i = 0; i + 4 <= 0x1000; i += 4) {
                const auto a = *(uint32_t*)((uintptr_t)v0 + i);
                const auto b = *(uint32_t*)((uintptr_t)v1 + i);
                if (a == b || a >= 8 || b >= 8) continue;

                // Reject anything that looks like the low dword of a pointer (high dword differs or is
                // non-zero on either side), and "X -> 0" pairs: those are handles/pointers that are null
                // on the right eye, not enums. Writing into them crashed the engine (0xc: 5->0, 0x154: 5->0).
                bool pointer_like = false;
                if ((i % 8) == 0 && i + 8 <= 0x1000) {
                    const auto ha = *(uint32_t*)((uintptr_t)v0 + i + 4);
                    const auto hb = *(uint32_t*)((uintptr_t)v1 + i + 4);
                    pointer_like = ha != 0 || hb != 0;
                }
                if (pointer_like || b == 0) {
                    skipped += fmt::format("{:x}:{}->{} ", i, a, b);
                    continue;
                }

                eye_fields.push_back({i, a, b});
                s2 += fmt::format("F{}={:x}:{}->{} ", eye_fields.size() - 1, i, a, b);
            }
            SPDLOG_INFO("[VR] sceneview_xref: eye identity fields (left->right): {} | skipped pointer-like/null-right: {}", s2, skipped);
        }
    }
}

// Called after the original constructor has run for a full-size game view. Maps the live offsets
// (resolved above) back into FSceneViewInitOptions by VALUE: the constructor copies Family and
// StereoPass verbatim from the init options, so whatever the constructed view holds at the live
// offsets must appear somewhere in the init options block.
static void feed(sdk::FSceneView* view, sdk::FSceneViewInitOptions* init_options, uint32_t frame) {
    if (resolved || attempts > 600) return;
    if (view == nullptr || init_options == nullptr) return;
    if (!live_view_family_offset) return; // wait for begin_render_viewfamily_real to resolve the live side
    if (IsBadReadPtr(init_options, sizeof(Snapshot::init_options)) || IsBadReadPtr(view, sizeof(Snapshot::view))) return;

    ++attempts;

    const auto io = (uintptr_t)init_options;
    const auto family_value = *(uintptr_t*)((uintptr_t)view + *live_view_family_offset);

    if (!plausible(family_value)) return;

    // Family: search past the projection data (view_rect is at +0x90 in this game, matches stock layout).
    const auto fam_off = find_ptr((const uint8_t*)io + 0x80, snap_init_size() - 0x80, family_value);
    if (!fam_off) {
        if (attempts <= 3 || attempts % 120 == 0) {
            SPDLOG_INFO("[VR] sceneview_xref: family value {:x} not found in init options (attempt {}), dumping 0x80..0x180:", family_value, attempts);
            for (uint32_t i = 0x80; i < 0x180; i += 0x20) {
                SPDLOG_INFO("  +{:x}: {:x} {:x} {:x} {:x}", i,
                    *(uint64_t*)(io + i), *(uint64_t*)(io + i + 8), *(uint64_t*)(io + i + 16), *(uint64_t*)(io + i + 24));
            }
        }
        return;
    }

    const auto init_family_off = 0x80 + *fam_off;

    // State: stock layout puts SceneViewStateInterface immediately after ViewFamily. Accept it if it is
    // null or a vtable'd object; otherwise scan forward a little.
    std::optional<uint32_t> init_state_off{};
    for (uint32_t s = init_family_off + sizeof(void*); s < init_family_off + 0x30 && s + sizeof(void*) <= snap_init_size(); s += sizeof(void*)) {
        const auto sc = *(uintptr_t*)(io + s);
        if (sc == 0 || has_module_vtable(sc)) { init_state_off = s; break; }
    }

    // StereoPass: the constructed view's value must also appear in the init options after the family.
    std::optional<uint32_t> init_stereo_off{};
    if (live_stereo_pass_offset) {
        const auto sp = *(uint32_t*)((uintptr_t)view + *live_stereo_pass_offset);
        if (sp == stereo_pass_left || sp == stereo_pass_right) {
            // Skip the FLinearColor block (1.0f == 0x3f800000, never 1 or 2) - only integer slots will match.
            for (uint32_t i = init_family_off + sizeof(void*); i + 4 <= snap_init_size(); i += 4) {
                if (*(uint32_t*)(io + i) == sp) { init_stereo_off = i; break; }
            }
        }
    }

    const auto prev_fam = sdk::FSceneViewInitOptionsBase::get_view_family_offset().value_or(0xFFFFFFFF);
    const auto prev_sp = sdk::FSceneViewInitOptionsBase::get_stereo_pass_offset().value_or(0xFFFFFFFF);
    sdk::FSceneViewInitOptionsBase::override_offsets(init_family_off, init_state_off, init_stereo_off);
    resolved = true;

    SPDLOG_INFO("[VR] sceneview_xref: RESOLVED init_options family@{:x} state@{:x} stereo_pass@{:x} (heuristic had family@{:x} stereo_pass@{:x}; family={:x})",
        init_family_off, init_state_off.value_or(0xFFFFFFFF), init_stereo_off.value_or(0xFFFFFFFF), prev_fam, prev_sp, family_value);
}
} // namespace sceneview_xref

bool FFakeStereoRenderingHook::is_in_viewport_client_draw() const {
    return m_in_viewport_client_draw && GameThreadWorker::get().is_same_thread();
}

// Tracks the FSceneViewFamily that begin_render_viewfamily_real most recently confirmed to be the
// real HMD stereo family (i.e. it had >=2 views, a valid scene interface, and a valid render
// target). sceneview_constructor() cannot tell an unrelated single-view scene capture (menu
// portrait captures, UI thumbnail captures, etc.) apart from the real stereo family just from
// stereo-pass value alone, and forcing those unrelated families into the same-pass path corrupts
// their render (black menu characters, black screens). Gate the same-pass branch on this pointer
// so it only ever touches the real HMD family.
static std::atomic<sdk::FSceneViewFamily*> g_last_real_stereo_view_family{nullptr};

// DIAG: correlates the raw view_index passed into calculate_stereo_view_offset() with the real
// FSceneView metadata later observed for that same view inside sceneview_constructor(). These are
// two separate engine call sites, but UE processes one view at a time on the game thread
// (ULocalPlayer::CalcSceneView -> GetProjectionData -> IStereoRendering::CalculateStereoViewOffset,
// immediately followed by that same view's FSceneView constructor) before moving on to the next
// view, so "most recently seen view_index this frame" reliably identifies which raw index produced
// the FSceneView that sceneview_constructor is currently looking at. This gives an authoritative
// view_index <-> {StereoPass, ViewRect, ViewFamily, Actor, PlayerIndex} mapping instead of inferring
// identity from call-site RVA or index parity alone (see NSF-VIEWINDEX-IDENTITY below).
static thread_local std::optional<int32_t> t_last_calc_stereo_view_index{};
static thread_local uint32_t t_last_calc_stereo_view_index_frame{0xFFFFFFFFu};

// FSceneView constructor hook
sdk::FSceneView* FFakeStereoRenderingHook::sceneview_constructor(sdk::FSceneView* view, sdk::FSceneViewInitOptions* init_options, void* a3, void* a4) {
    SPDLOG_INFO_ONCE("Called FSceneView constructor for the first time");

    auto& vr = VR::get();

    if (!g_hook->is_in_viewport_client_draw() || !vr->is_hmd_active()) {
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    if (g_hook->m_analyzing_view_extensions || !g_hook->m_has_view_extensions_installed) {
        SPDLOG_INFO_ONCE("FSceneView constructor was called before view extensions were installed, aborting");
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    if (init_options == nullptr) {
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    std::scoped_lock ___{g_hook->m_sceneview_data.mtx};

    const auto retaddr = (uintptr_t)_ReturnAddress();

    const bool first_time_from_this_retaddr = !g_hook->m_sceneview_data.seen_retaddrs.contains(retaddr);

    if (first_time_from_this_retaddr) {
        g_hook->m_sceneview_data.seen_retaddrs.insert(retaddr);
        SPDLOG_INFO("FSceneView constructor called from {:x}", retaddr);
    }

    // The offsets MUST be resolved before get_view_family() is usable: it returns nullptr until
    // s_view_family_offset has been found. The loading-screen bail-out below previously ran before
    // this call, so get_view_family() always returned nullptr, the bail-out always fired, and the
    // rest of this hook (stereo pass -> PRIMARY override, view-count hiding, scene state tracking)
    // never executed. Confirmed via logs: "[FSceneViewInitOptions] Found ... offset" never appeared.
    sdk::FSceneViewInitOptionsBase::update_offsets(init_options);

    // A raw pointer read out of FSceneViewInitOptions can be garbage for callers other than
    // ULocalPlayer::CalcSceneView (e.g. the game's own USceneCaptureComponent / reflection capture
    // views, which build their init options differently). Observed: a second constructor call site
    // appeared the moment the main world (AkiWorld_WP) came up and get_view_family() returned 0x3,
    // which then faulted at ->get_scene_interface() (read of 0x23). Never dereference it unvalidated.
    const auto is_plausible_ptr = [](const void* p) -> bool {
        const auto addr = (uintptr_t)p;
        return addr > 0x10000 && addr < 0x7FFFFFFFFFFF && (addr % sizeof(void*)) == 0 && !IsBadReadPtr(p, 0x40);
    };

    auto early_view_family = init_options->get_view_family();
    const bool view_family_plausible = is_plausible_ptr(early_view_family);

    if (first_time_from_this_retaddr) {
        // One-shot per call site: enough to identify what kind of view this caller is producing
        // (game view vs scene capture vs something else) without spamming every frame.
        SPDLOG_INFO("[VR] sceneview_constructor: new call site {:x} -> view_family={:x} plausible={} stereo_pass={} view_rect=({},{})-({},{})",
            retaddr, (uintptr_t)early_view_family, view_family_plausible, (int32_t)init_options->get_stereo_pass(),
            init_options->view_rect[0], init_options->view_rect[1], init_options->view_rect[2], init_options->view_rect[3]);
    }

    // DIAG: NSF-VIEWINDEX-IDENTITY. Attributes the raw view_index most recently seen by
    // calculate_stereo_view_offset() (same frame, same game thread) to the ground-truth FSceneView
    // metadata this constructor call is producing for that view: real engine StereoPass, view rect
    // size/position, whether its ViewFamily is the confirmed real HMD stereo family (as opposed to an
    // unrelated single-view scene capture), and its owning actor/player index. This is authoritative -
    // unlike GetViewPassForIndex (synthetic, parity-based) or caller-RVA alone (only proves which UE
    // loop iterates the indices, not what each index semantically is).
    if (VR::get() != nullptr && VR::get()->is_diag_log_raw_view_index_enabled()) {
        const bool index_is_current_frame = t_last_calc_stereo_view_index.has_value() && t_last_calc_stereo_view_index_frame == g_frame_count;
        const int32_t attributed_view_index = index_is_current_frame ? *t_last_calc_stereo_view_index : -2;

        const bool is_known_real_stereo_family = view_family_plausible &&
            early_view_family == g_last_real_stereo_view_family.load(std::memory_order_relaxed);

        const int32_t rect_w = init_options->view_rect[2] - init_options->view_rect[0];
        const int32_t rect_h = init_options->view_rect[3] - init_options->view_rect[1];

        static thread_local int32_t s_last_logged_attributed_index = -3;
        static thread_local uint32_t s_last_logged_frame = 0xFFFFFFFFu;

        // Throttle: one line per (attributed_index, frame) pair rather than one per call site, since
        // this needs to show every distinct index's per-frame identity, not just the first sighting.
        if (attributed_view_index != s_last_logged_attributed_index || g_frame_count != s_last_logged_frame) {
            s_last_logged_attributed_index = attributed_view_index;
            s_last_logged_frame = g_frame_count;

            SPDLOG_WARN("[VR][NSF-VIEWINDEX-IDENTITY] frame={} view_index={} (attributed_this_frame={}) retaddr={:x} "
                "stereo_pass={} view_family={:x} is_real_stereo_family={} rect=({},{})-({},{}) size={}x{} "
                "scene_state={:x} actor={:x} player_index={}",
                g_frame_count, attributed_view_index, index_is_current_frame, retaddr,
                (int32_t)init_options->get_stereo_pass(), (uintptr_t)early_view_family, is_known_real_stereo_family,
                init_options->view_rect[0], init_options->view_rect[1], init_options->view_rect[2], init_options->view_rect[3],
                rect_w, rect_h, (uintptr_t)init_options->get_scene_state(),
                (uintptr_t)init_options->actor, init_options->player_index);
        }
    }

    if (view_family_plausible) {
        sdk::FSceneViewFamily::update_offsets(early_view_family, nullptr);
    }

    // SOLUTION 2: BYPASS STEREO MODIFICATIONS DURING LOADING SCREENS
    // =================================================================
    sdk::FSceneInterface* early_scene_interface = nullptr;

    if (view_family_plausible) {
        try {
            early_scene_interface = early_view_family->get_scene_interface();
        } catch (...) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] sceneview_constructor: exception reading scene interface from view_family={:x} (retaddr={:x})",
                (uintptr_t)early_view_family, retaddr);
            early_scene_interface = nullptr;
        }
    }

    if (!view_family_plausible || early_scene_interface == nullptr) {
        // Either this caller's init options don't carry a usable view family (non-game view), or the
        // scene interface isn't active yet (game is streaming/loading assets).
        // Let standard Unreal SceneView construction proceed untouched!
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] sceneview_constructor: bypassing (retaddr={:x} view_family={:x} plausible={} scene_interface={:x})",
            retaddr, (uintptr_t)early_view_family, view_family_plausible, (uintptr_t)early_scene_interface);
        auto result = g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);

        // Only full-size views are the eye views we want to learn the layout from (skip 64x64 captures).
        const int32_t w = init_options->view_rect[2] - init_options->view_rect[0];
        const int32_t h = init_options->view_rect[3] - init_options->view_rect[1];
        if (!view_family_plausible && w >= 256 && h >= 256) {
            sceneview_xref::feed(view, init_options, g_frame_count);
        }

        return result;
    }

    const auto is_ue5 = g_hook->has_double_precision();
    auto init_options_ue5 = (sdk::FSceneViewInitOptionsUE5*)init_options;

    const auto init_options_scene_state = init_options->get_scene_state();

    // Validate pointer address range before using it as a map key
    const auto scene_state_addr = reinterpret_cast<uintptr_t>(init_options_scene_state);
    const bool is_valid_scene_state = (scene_state_addr > 0x10000 && scene_state_addr < 0x7FFFFFFFFFFF && scene_state_addr % 8 == 0 &&
                                       !IsBadReadPtr(init_options_scene_state, sizeof(void*)));

    if (init_options_scene_state != nullptr && is_valid_scene_state) {
        if (is_ue5) {
            auto& vio_entry = g_hook->m_sceneview_data.view_init_options_ue5[init_options_scene_state];
            memcpy(&vio_entry, init_options, sizeof(sdk::FSceneViewInitOptionsUE5));
        } else {
            auto& vio_entry = g_hook->m_sceneview_data.view_init_options_ue4[init_options_scene_state];
            memcpy(&vio_entry, init_options, sizeof(sdk::FSceneViewInitOptionsUE4));
        }
    }

    auto& known_scene_states = g_hook->m_sceneview_data.known_scene_states;
    auto& last_frame_count = g_hook->m_sceneview_data.last_frame_count;
    auto& last_index = g_hook->m_sceneview_data.last_index;

    if (last_frame_count != g_frame_count || last_index > 1) {
        last_index = 0;
    }

    last_frame_count = g_frame_count;

    const auto true_index = vr->is_using_afr() ? (g_frame_count + last_index) % 2 : last_index;

    if (vr->is_splitscreen_compatibility_enabled() || vr->is_sceneview_compatibility_enabled()) {
        int32_t w = vr->get_hmd_width();
        int32_t h = vr->get_hmd_height();

        int32_t x = 0;
        int32_t y = 0;

        if (!vr->is_using_afr() && true_index == 1 && !vr->is_native_stereo_fix_enabled()) {
            x += w;
        }

        FIntRect view_rect{x, y, x + w, y + h};

        vr->get_runtime()->update_matrices(0.1f, 10000.0f);

        const auto proj_mat = vr->get_projection_matrix((VRRuntime::Eye)(true_index));

        auto& init_options_view_origin = is_ue5 ? *(glm::vec3*)&init_options_ue5->view_origin : init_options->view_origin;
        auto& init_options_view_rect = is_ue5 ? init_options_ue5->view_rect : init_options->view_rect;
        auto& init_options_constrained_view_rect = is_ue5 ? init_options_ue5->constrained_view_rect : init_options->constrained_view_rect;
        auto& init_options_projection_matrix = init_options->projection_matrix;
        auto& init_options_projection_matrix_ue5 = init_options_ue5->projection_matrix;

        auto& init_options_view_rotation_matrix = init_options->view_rotation_matrix;
        auto& init_options_view_rotation_matrix_ue5 = init_options_ue5->view_rotation_matrix;

        const auto conversion_mat = glm::mat4 {
            0, 0, 1, 0,
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, 1
        };

        const auto conversion_mat_inverse = glm::inverse(conversion_mat);

        // We need to "undo" the operations done to create the rotation matrix so we can get the original angle
        // const auto view_rot_mat = conversion_mat * make_inverse_rot_matrix(euler); <-- this is the result of the conversion
        glm::vec3 euler{};

        if (is_ue5) {
            euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * glm::mat4{init_options_view_rotation_matrix_ue5}));
        } else {
            euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * init_options_view_rotation_matrix));
        }

        auto euler_d = glm::vec<3, double>{euler};
        auto euler_pointer = is_ue5 ? (Rotator<float>*)&euler_d : (Rotator<float>*)&euler;

        g_hook->calculate_stereo_view_offset_(true_index + 1, euler_pointer, 100.0f, &init_options_view_origin);

        if (is_ue5) {
            euler = euler_d;
        }

        const auto view_rot_mat = conversion_mat * utility::math::ue_inverse_rotation_matrix(euler);

        *(FIntRect*)&init_options_view_rect = view_rect;
        *(FIntRect*)&init_options_constrained_view_rect = view_rect;

        if (is_ue5) {
            init_options_view_rotation_matrix_ue5 = view_rot_mat;

            if (!vr->is_using_2d_screen()) {
                init_options_projection_matrix_ue5 = proj_mat;
            }
        } else {
            init_options_view_rotation_matrix = view_rot_mat;

            if (!vr->is_using_2d_screen()) {
                init_options_projection_matrix = proj_mat;
            }
        }
    }

    const auto init_options_stereo_pass = init_options->get_stereo_pass();

    std::optional<uint32_t> views_original_count{};
    bool same_pass_entered = false;
    static thread_local int32_t s_same_pass_reentrancy_depth = 0;

    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] sceneview_constructor: same_pass_enabled={} force_primary_enabled={} init_options_stereo_pass={} scene_capture_rt={}",
        vr->is_native_stereo_fix_same_pass_enabled(), vr->is_native_stereo_fix_same_pass_force_primary_enabled(), (int32_t)init_options_stereo_pass,
        (void*)g_hook->get_render_target_manager()->get_scene_capture_render_target());

    // DIAG: only fires when the force-primary diagnostic toggle is on, so this is decisive/verbose
    // logging that is safe to leave enabled specifically while re-testing that path (not gated by
    // _EVERY_N_SEC, so every single FSceneView constructor call is visible during the test).
    if (vr->is_native_stereo_fix_same_pass_force_primary_enabled()) {
        SPDLOG_INFO(
            "[VR][SAME-PASS-FORCE] sceneview_constructor ENTRY: init_options={:x} view={:x} init_options_stereo_pass={} "
            "view_family={:x} scene_state={:x}",
            (uintptr_t)init_options, (uintptr_t)view, (int32_t)init_options_stereo_pass,
            (uintptr_t)init_options->get_view_family(), (uintptr_t)init_options_scene_state);
    }

    // =========================================================================
    // LEVEL TRANSITION / LOAD GUARD
    // =========================================================================
    bool is_actively_loading = false;

    // Get the active engine and world instance using UEVR's UEngine wrapper
    auto engine = sdk::UEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;

    if (world == nullptr) {
        is_actively_loading = true;
    } else {
        // Safely check the bIsTearingDown property on the world
        static auto world_class = world->get_class();
        if (world_class != nullptr) {
            static auto is_tearing_down_prop = world_class->find_property(L"bIsTearingDown");
            if (is_tearing_down_prop != nullptr) {
                // bIsTearingDown is a packed bitfield (uint8:1) shared with other unrelated
                // bools in the same byte. Reading it as a whole bool* is unsafe and can produce
                // false positives/negatives depending on neighboring bits. Mask to bit 0 instead,
                // matching the check used in begin_render_viewfamily_real.
                auto tearing_down_ptr = (uint8_t*)((uintptr_t)world + is_tearing_down_prop->get_offset());
                if (tearing_down_ptr != nullptr && (*tearing_down_ptr & 1) != 0) {
                    is_actively_loading = true;
                }
            }
        }
    }

    // bIsTearingDown only covers the OLD world's teardown, not the new world's streaming/loading
    // screen. If the game thread's engine tick hasn't run recently, treat that as authoritative
    // proof we're on a loading screen / mid level-transition, regardless of the bitfield state.
    const bool tick_stalled_svc = vr->is_engine_tick_stalled();
    // Neither bIsTearingDown nor tick-stall catch the window where the new level is still
    // streaming in after the tick resumes but before the player controller exists again.
    const bool no_player_controller_svc = is_local_player_controller_missing();
    // A player controller can exist well before the player is actually possessing a pawn in the
    // world (initial boot loading screen, cutscenes, level streaming). This is a stronger signal
    // than controller-existence alone and cannot be satisfied by the transient boot/loading world.
    const bool no_local_pawn_svc = is_local_pawn_missing();
    const bool boot_phase_svc = is_boot_phase_active();
    is_actively_loading = is_actively_loading || tick_stalled_svc || no_player_controller_svc || no_local_pawn_svc || boot_phase_svc;

    if (are_loading_guards_disabled()) {
        is_actively_loading = false;
    }

    if (vr->is_native_stereo_fix_enabled() && vr->is_native_stereo_fix_same_pass_enabled() &&
        init_options_stereo_pass > EStereoscopicPass::eSSP_PRIMARY) {

        // The same-pass secondary-view path does meaningfully more work per frame (full depth/shadow
        // scene render) than a simple eye copy. Enabling it the instant the scene capture target
        // becomes valid can still land inside the tail end of level streaming, and that extra
        // render-thread work can starve streaming of the cycles it needs to finish, manifesting as a
        // hard stall on the loading screen. Require a short grace period after readiness on top of
        // the existing loading guard before committing to this path.
        const bool in_grace_period = g_hook->get_render_target_manager()->is_scene_capture_in_grace_period();

        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] sceneview_constructor: is_actively_loading={} tick_stalled={} no_local_pawn={} boot_phase={} in_grace_period={}",
            is_actively_loading, tick_stalled_svc, no_local_pawn_svc, boot_phase_svc, in_grace_period);

        // Only alter view counts if the target exists AND the level isn't actively tearing down / loading
        // AND we're past the post-readiness grace period AND this is confirmed to be the real HMD
        // stereo family, not an unrelated single-view scene capture (menu portraits, UI thumbnails,
        // etc.) that also passes through this constructor with a non-PRIMARY stereo pass. Without
        // this check the same-pass branch was hijacking those unrelated families, blanking them out
        // (black menu characters, black in-game screens) - see the views.count==0 evidence in the log.
        auto candidate_view_family = init_options->get_view_family();
        const bool is_real_stereo_family = candidate_view_family != nullptr &&
            candidate_view_family == g_last_real_stereo_view_family.load(std::memory_order_relaxed);

        if (g_hook->get_render_target_manager()->get_scene_capture_render_target() != nullptr && !is_actively_loading &&
            !in_grace_period && is_real_stereo_family) {
            // DIAG/EXPERIMENTAL RE-ENABLE: forcing StereoPass=PRIMARY on the secondary (right-eye)
            // view was confirmed via diagnostics in an earlier session to be the actual root cause
            // of the black-screen reports (main menu characters black, world visible behind HUD on
            // loading screen, black in-game both eyes). Log evidence (2026-09-06 session) proved
            // is_real_stereo_family=true for this exact family/branch, ruling out the earlier
            // "hijacked unrelated family" theory - the Views.count==0/1 readings were simply the
            // engine constructing Pass1 then Pass2 in-order (count hasn't been incremented yet at
            // construction time), not a corrupted state. The real problem is structural: with two
            // views flagged PRIMARY in the same family, whatever per-family shadow/base-pass setup
            // keys off StereoPass==PRIMARY can only fully render one of them, silently dropping or
            // misdirecting the other eye's render - exactly matching the reported symptoms. There
            // is no known safe way to make the engine treat two views as PRIMARY simultaneously
            // without patching that internal (unsymboled) engine logic directly, so this override
            // stays gated behind a SEPARATE, off-by-default diagnostic toggle
            // (is_native_stereo_fix_same_pass_force_primary_enabled()) instead of the main
            // "Use Same Stereo Pass" toggle, so it can be deliberately re-tested with decisive
            // logging without silently reintroducing the black-screen regression for normal users.
            if (vr->is_native_stereo_fix_same_pass_force_primary_enabled()) {
                const auto view_family_for_log = candidate_view_family;
                const auto views_for_log = view_family_for_log != nullptr ? view_family_for_log->get_views() : nullptr;
                const auto views_count_for_log = views_for_log != nullptr ? views_for_log->count : (uint32_t)0xFFFFFFFF;

                SPDLOG_INFO(
                    "[VR][SAME-PASS-FORCE] About to force StereoPass=PRIMARY: view_family={:x} views_ptr={:x} "
                    "views_count={} init_options_stereo_pass(before)={} scene_capture_rt={:x} is_actively_loading={} "
                    "in_grace_period={} is_real_stereo_family={}",
                    (uintptr_t)view_family_for_log, (uintptr_t)views_for_log, views_count_for_log,
                    (int32_t)init_options_stereo_pass,
                    (uintptr_t)g_hook->get_render_target_manager()->get_scene_capture_render_target(),
                    is_actively_loading, in_grace_period, is_real_stereo_family);

                init_options->set_stereo_pass((uint32_t)EStereoscopicPass::eSSP_PRIMARY);

                SPDLOG_INFO(
                    "[VR][SAME-PASS-FORCE] Forced StereoPass=PRIMARY: view_family={:x} init_options_stereo_pass(after)={}",
                    (uintptr_t)view_family_for_log, (int32_t)init_options->get_stereo_pass());
            } else {
                SPDLOG_INFO_EVERY_N_SEC(
                    2, "[VR] sceneview_constructor: same-pass branch conditions met but override is disabled (see comment) - leaving stereo pass untouched");
            }
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] sceneview_constructor: skipping view count override (loading={} in_grace_period={} is_real_stereo_family={} candidate_view_family={:x})",
                is_actively_loading, in_grace_period, is_real_stereo_family, (uintptr_t)candidate_view_family);
        }
    }

    // 1. Declare the boolean flag first
        bool new_scene_state_inserted_this_frame = false;


        // 3. Process scene state insertion
        if (init_options_scene_state != nullptr && is_valid_scene_state &&
            !g_hook->m_sceneview_data.known_scene_states.contains(init_options_scene_state)) {
            SPDLOG_INFO("Inserting new valid scene state {:x}", (uintptr_t)init_options_scene_state);
            known_scene_states.insert(init_options_scene_state);
            new_scene_state_inserted_this_frame = true;
        } else if (init_options_scene_state == nullptr) {
            SPDLOG_ERROR_ONCE("Scene state passed to FSceneView constructor is null");

            if ((int32_t)init_options_stereo_pass < 0) {
                SPDLOG_ERROR_ONCE("Stereo pass is negative");
            }
        }

        // 4. Ghosting fix check (uses the variable declared above)
        // AFR only. Do NOT apply to Native Stereo Fix's secondary view: the engine already gives
        // each stereo view its own persistent FSceneViewState (ULocalPlayer::ViewStates[index]),
        // and swapping the right eye onto the left eye's state makes two different frusta share one
        // TAA/occlusion/HZB history, which manifests as smeared/misaligned vision during camera motion.
        if (init_options_scene_state != nullptr && is_valid_scene_state && !new_scene_state_inserted_this_frame &&
            vr->is_ghosting_fix_enabled() && !known_scene_states.empty() &&
            vr->is_using_afr() && true_index == 1) {
            init_options->set_stereo_pass(EStereoscopicPass::eSSP_PRIMARY);

            // Set the scene state to the one that isn't the current one
            for (auto scene_state : known_scene_states) {
                if (scene_state != init_options_scene_state) {
                    SPDLOG_INFO_ONCE("Setting scene state to {:x}", (uintptr_t)scene_state);
                    init_options->set_scene_state(scene_state);
                    break;
                }
            }
        }

    last_index++;

    auto result = g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);

    if (vr->is_native_stereo_fix_same_pass_force_primary_enabled()) {
        SPDLOG_INFO(
            "[VR][SAME-PASS-FORCE] sceneview_constructor EXIT: init_options={:x} result_view={:x} "
            "init_options_stereo_pass(final)={} is_null_result={}",
            (uintptr_t)init_options, (uintptr_t)result, (int32_t)init_options->get_stereo_pass(), result == nullptr);
    }

    // Restore the hidden view count now that this constructor call (and anything nested inside it)
    // has finished, so the reentrancy guard above only ever sees a corrupted count if something
    // genuinely re-entered while we still had it hidden.
    if (same_pass_entered) {
        auto view_family_restore = init_options->get_view_family();
        auto views_restore = view_family_restore != nullptr ? view_family_restore->get_views() : nullptr;

        if (views_restore != nullptr && views_original_count.has_value()) {
            views_restore->count = views_original_count.value();
        }

        --s_same_pass_reentrancy_depth;
    }

    // Only trust the init-options state pointer for live-offset discovery once sceneview_xref has
    // corrected the init-options layout for this game; before that get_scene_state() reads garbage.
    if (is_valid_scene_state && sceneview_xref::resolved) {
        sceneview_xref::resolve_live_scene_state(view, init_options->get_scene_state());
    }

    return result;
}

void FFakeStereoRenderingHook::setup_view_family(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("SetupViewFamily");

    static bool once = true;

    if (once) {
        SPDLOG_INFO("Called SetupViewFamily for the first time");
        once = false;
    }

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    //vr->update_hmd_state(true, vr->get_runtime()->internal_frame_count + 1);
}

void FFakeStereoRenderingHook::setup_viewpoint(ISceneViewExtension* extension, void* player_controller, void* view_info) {
    ZoneScopedN("SetupViewPoint");
    SPDLOG_INFO_ONCE("Called SetupViewPoint for the first time");

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_ghosting_fix_enabled() || g_hook->m_fixed_localplayer_view_count) {
        return;
    }

    // Using this as a way to get to the localplayer
    static bool attempted_hook{false};

    // Fix localplayer view count
    if (!attempted_hook) {
        SPDLOG_INFO("Attempting to find caller of ISceneViewExtension::SetupViewPoint");

        attempted_hook = true;
        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto caller = utility::find_virtual_function_start(return_address);

        if (!caller) {
            SPDLOG_ERROR("Failed to find caller of ISceneViewExtension::SetupViewPoint");
            return;
        }

        // No need to StartDisabled on this because we're on the same thread.
        g_hook->m_localplayer_get_viewpoint_hook = safetyhook::create_inline(*caller, (uintptr_t)&localplayer_setup_viewpoint);
        
        if (!g_hook->m_localplayer_get_viewpoint_hook) {
            SPDLOG_ERROR("Failed to hook ISceneViewExtension::SetupViewPoint");
            return;
        }

        SPDLOG_INFO("Hooked ISceneViewExtension::SetupViewPoint");
    }
}

void FFakeStereoRenderingHook::localplayer_setup_viewpoint(void* localplayer, void* view_info, void* pass) {
    ZoneScopedN("LocalPlayerSetupViewPoint");
    SPDLOG_INFO_ONCE("Called LocalPlayerSetupViewPoint for the first time");

    if (!g_hook->m_fixed_localplayer_view_count) {
        static bool attempted = false;

        if (!attempted) {
            attempted = true;

            if (localplayer != nullptr && !IsBadReadPtr(localplayer, sizeof(void*))) try {
                g_hook->post_init_properties((uintptr_t)localplayer);
            } catch(...) {
                SPDLOG_ERROR("[LocalPlayerSetupViewPoint] Failed to post init properties");
            }
        }
    }

    g_hook->m_localplayer_get_viewpoint_hook.call<void>(localplayer, view_info, pass);
}

void FFakeStereoRenderingHook::begin_render_viewfamily_real(
    void* render_module, sdk::FCanvas* canvas, sdk::FSceneViewFamily* view_family_candidate) {
    ZoneScopedN("BeginRenderViewFamilyReal");

    SPDLOG_INFO_ONCE("Called BeginRenderViewFamilyReal for the first time");

    if (!g_framework->is_game_data_intialized()) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    // Advance the boot-phase frame-pacing tracker exactly once per real frame.
    update_boot_phase_tracking();

    auto& vr = VR::get();
    auto rtm = g_hook->get_render_target_manager();

    // Proactively tear down the scene capture the instant we detect its owning world has changed
    // or is tearing down, rather than relying on GC to notice a stale actor for us. See
    // is_scene_capture_world_stale() for why this replaced rooting the actor/component.
    if (rtm->is_scene_capture_world_stale()) {
        auto engine_ptr = sdk::UEngine::get();
        auto current_world = engine_ptr != nullptr ? engine_ptr->get_world() : nullptr;
        SPDLOG_INFO("[VR] begin_render_viewfamily_real: scene capture's world is stale (changed or tearing down), destroying proactively "
                    "(current_world={})",
                    (void*)current_world);
        rtm->destroy_scene_capture();
    }

    // Automatically suspend/resume Native Stereo Fix across level transitions, replicating the
    // manual A/B toggle workflow that was confirmed to avoid the scene-capture-actor lifecycle
    // conflict: flip it off (falling back to the existing, already-working "disabled" compositing
    // path) the instant a transition is detected, and flip it back on once the world has settled.
    // This is deliberately independent of are_loading_guards_disabled(), since that toggle governs
    // whether create_scene_capture() is allowed to run - it says nothing about whether Native
    // Stereo Fix itself should be temporarily disabled to avoid the actor conflict entirely.
    {
        const bool tick_stalled = vr->is_engine_tick_stalled();
        const bool no_player_controller = is_local_player_controller_missing();
        const bool no_local_pawn = is_local_pawn_missing();
        const bool boot_phase = is_boot_phase_active();
        const bool world_stale = rtm->is_scene_capture_world_stale();
        const bool should_suspend = vr->is_native_stereo_fix_auto_suspend_enabled() &&
            (tick_stalled || no_player_controller || no_local_pawn || boot_phase || world_stale);

        // DIAG: log the individual auto-suspend flags any time they change (or periodically while
        // suspended), so we can identify exactly which flag is getting stuck true and permanently
        // defeating Native Stereo Fix after the HUD/pawn appears, instead of only seeing the
        // aggregate should_suspend transition.
        {
            static bool s_last_tick_stalled = false;
            static bool s_last_no_player_controller = false;
            static bool s_last_no_local_pawn = false;
            static bool s_last_boot_phase = false;
            static bool s_last_world_stale = false;
            static bool s_have_last = false;

            const bool changed = !s_have_last || tick_stalled != s_last_tick_stalled ||
                no_player_controller != s_last_no_player_controller || no_local_pawn != s_last_no_local_pawn ||
                boot_phase != s_last_boot_phase || world_stale != s_last_world_stale;

            if (changed) {
                s_have_last = true;
                s_last_tick_stalled = tick_stalled;
                s_last_no_player_controller = no_player_controller;
                s_last_no_local_pawn = no_local_pawn;
                s_last_boot_phase = boot_phase;
                s_last_world_stale = world_stale;

                SPDLOG_INFO("[NSF_SUSPEND_FLAGS] should_suspend={} currently_suspended={} | tick_stalled={} no_player_controller={} no_local_pawn={} boot_phase={} world_stale={}",
                    should_suspend, vr->is_native_stereo_fix_suspended(), tick_stalled, no_player_controller, no_local_pawn, boot_phase, world_stale);
            } else if (should_suspend) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[NSF_SUSPEND_FLAGS] (heartbeat) should_suspend={} currently_suspended={} | tick_stalled={} no_player_controller={} no_local_pawn={} boot_phase={} world_stale={}",
                    should_suspend, vr->is_native_stereo_fix_suspended(), tick_stalled, no_player_controller, no_local_pawn, boot_phase, world_stale);
            }
        }

        if (should_suspend != vr->is_native_stereo_fix_suspended()) {
            if (vr->is_diag_verbose_logging_enabled()) {
                SPDLOG_INFO("[VR] begin_render_viewfamily_real: {} Native Stereo Fix (tick_stalled={} no_player_controller={} no_local_pawn={} boot_phase={} world_stale={})",
                    should_suspend ? "suspending" : "resuming", tick_stalled, no_player_controller, no_local_pawn, boot_phase, world_stale);
            }
            vr->set_native_stereo_fix_suspended(should_suspend);

            if (should_suspend) {
                rtm->destroy_scene_capture();
            }
        }
    }

    if (!vr->is_hmd_active() || !vr->is_native_stereo_fix_enabled() || vr->should_mirror_right_eye_this_frame()) {
        // Mirror mode intentionally takes the same no-scene-capture path as native stereo fix
        // being disabled: no actor is spawned, and the right eye falls back to the existing
        // "mirror the left/game texture" compositing already present in D3D11Component/D3D12Component.
        // should_mirror_right_eye_this_frame() covers both the manual mirror toggle and the
        // auto-mirror-on-cinematic fallback (bCinematicMode).
        rtm->destroy_scene_capture();

        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    struct TArrayViewViewFamily {
        sdk::FSceneViewFamily** data;
        uint32_t count;
    };

    const auto uses_tarrayview = sdk::FSceneViewFamily::has_vtable() && *(void**)view_family_candidate != sdk::FSceneViewFamily::get_vtable_ptr();
    const auto ue5_view_family_array = (TArrayViewViewFamily*)view_family_candidate;

    if (uses_tarrayview && ue5_view_family_array->data == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }
    
    // UE5 passes an TArrayView of ViewFamily pointers instead of a single ViewFamily
    sdk::FSceneViewFamily* view_family = uses_tarrayview ? ue5_view_family_array->data[0] : view_family_candidate;

    // PATCH 1: BAIL OUT IF SCENE IS NULL (LOADING / LEVEL TRANSITION)
    if (view_family == nullptr || view_family->get_scene_interface() == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    auto views_ptr = view_family->get_views();
    // ADD THE SIZE/COUNT CHECK HERE:
    if (views_ptr == nullptr || views_ptr->size() == 0) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    auto& views = *views_ptr;

    // Record this as the confirmed real HMD stereo family (>=2 views, valid scene interface/RT
    // already established above) so sceneview_constructor's same-pass branch can distinguish it
    // from unrelated single-view scene captures (menu portraits, UI thumbnails, etc.) that also
    // flow through the FSceneView constructor with a non-PRIMARY stereo pass.
    if (views.count >= 2) {
        g_last_real_stereo_view_family.store(view_family, std::memory_order_relaxed);
    }
    const auto prev_count = views.count;

    const auto rt = rtm->get_scene_capture_utexture();
    const auto rtrsrc = rt != nullptr ? (sdk::FTextureRenderTargetResource*)rt->get_resource() : nullptr;
    const auto rtfrt = rtrsrc != nullptr ? rtrsrc->as_render_target() : nullptr;

    if (rtfrt == nullptr) {
        // DIAGNOSTIC: pinpoint exactly which link in the chain broke, since "rtfrt null" alone
        // doesn't tell us whether the utexture, its resource, or the render-target cast failed.
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[VR] begin_render_viewfamily_real chain: rt(utex)={:x} rtrsrc={:x} rtfrt={:x}",
            (uintptr_t)rt, (uintptr_t)rtrsrc, (uintptr_t)rtfrt);

        // --- LEVEL TRANSITION GUARD ---
        // Check if the world is currently loading/tearing down before spawning a new scene capture component!
        bool is_loading = false;
        bool is_tearing_down = false;
        auto engine = sdk::UEngine::get();
        auto world = engine != nullptr ? engine->get_world() : nullptr;

        if (world == nullptr) {
            is_loading = true;
        } else {
            static auto world_class = world->get_class();
            if (world_class != nullptr) {
                static auto is_tearing_down_prop = world_class->find_property(L"bIsTearingDown");
                if (is_tearing_down_prop != nullptr) {
                    auto prop_addr = (uint8_t*)((uintptr_t)world + is_tearing_down_prop->get_offset());
                    if (prop_addr != nullptr && (*prop_addr & 1) != 0) {
                        is_tearing_down = true;
                        is_loading = true;
                    }
                }
            }
        }

        // bIsTearingDown only reflects the OLD world being torn down. It says nothing about
        // whether the NEW world's level streaming / loading screen is still active, which is
        // when create_scene_capture()'s actor-spawn + full render target/swapchain reallocation
        // cascade is most dangerous. Use engine tick staleness as an additional, more reliable signal.
        const auto tick_stalled = VR::get()->is_engine_tick_stalled();
        // Also catch the streaming window after the tick resumes but before the player controller
        // has respawned, which is when the scene-capture actor keeps getting GC'd/invalidated.
        const auto no_player_controller = is_local_player_controller_missing();
        // None of the above catch the INITIAL boot/load into the game: world is valid, tick is
        // running, player controller exists, yet the engine is still churning through asset
        // streaming at a fraction of normal frame rate, and the transient boot world explicitly
        // destroys our scene-capture actor between those rare frames. Suppress based on frame pacing.
        const auto boot_phase = is_boot_phase_active();
        is_loading = is_loading || tick_stalled || no_player_controller || boot_phase;

        if (are_loading_guards_disabled()) {
            is_loading = false;
        }

        SPDLOG_INFO_EVERY_N_SEC(2,
            "[VR] begin_render_viewfamily_real: rtfrt null, is_loading={} (tearing_down={} tick_stalled={} no_player_controller={} boot_phase={} world_null={})",
            is_loading, is_tearing_down, tick_stalled, no_player_controller, boot_phase, world == nullptr);

        // If actively loading, DO NOT call create_scene_capture()!
        // Simply let standard single-pass Unreal rendering proceed without creating actors.
        if (!is_loading) {
            static auto last_create_time = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            const auto since_last = now - last_create_time;
            SPDLOG_INFO("[VR] create_scene_capture() invoked from begin_render_viewfamily_real ({}ms since last invocation)",
                std::chrono::duration_cast<std::chrono::milliseconds>(since_last).count());
            last_create_time = now;
            rtm->create_scene_capture();
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] Skipping create_scene_capture() - level transition/load in progress");
        }

        views.count = 1;
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        views.count = prev_count;
        return;
    }

    auto view_family_target = view_family->get_render_target();

    if (view_family_target == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    bool wants_swap = false;

    // NOTE: SPDLOG_INFO_EVERY_N_SEC only prints once per window, so it cannot show true per-frame
    // cadence/stalls - it previously created the illusion of a fixed ~2s frame interval when in
    // reality it was just the log throttle firing. Track real per-frame deltas here instead so we
    // can see min/max/avg/count over each window and know if frames are actually pacing normally.
    {
        static std::chrono::steady_clock::time_point s_window_start{};
        static std::chrono::steady_clock::time_point s_last_frame{};
        static uint32_t s_frame_count_in_window{0};
        static double s_min_dt_ms{1e9};
        static double s_max_dt_ms{0.0};

        const auto now = std::chrono::steady_clock::now();

        if (s_window_start.time_since_epoch().count() == 0) {
            s_window_start = now;
            s_last_frame = now;
        } else {
            const auto dt_ms = std::chrono::duration<double, std::milli>(now - s_last_frame).count();
            s_last_frame = now;
            s_min_dt_ms = std::min(s_min_dt_ms, dt_ms);
            s_max_dt_ms = std::max(s_max_dt_ms, dt_ms);
            ++s_frame_count_in_window;
        }

        if (now - s_window_start >= std::chrono::seconds(2)) {
            const auto window_ms = std::chrono::duration<double, std::milli>(now - s_window_start).count();
            const auto avg_dt_ms = s_frame_count_in_window > 0 ? window_ms / s_frame_count_in_window : 0.0;

            SPDLOG_INFO("[VR] begin_render_viewfamily_real FRAME PACING over {:.0f}ms: frames={} min={:.1f}ms max={:.1f}ms avg={:.1f}ms",
                window_ms, s_frame_count_in_window, s_min_dt_ms, s_max_dt_ms, avg_dt_ms);

            s_window_start = now;
            s_frame_count_in_window = 0;
            s_min_dt_ms = 1e9;
            s_max_dt_ms = 0.0;
        }
    }

    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] begin_render_viewfamily_real: views.count={} prev_count={} rt_valid={}", views.count, prev_count, rtfrt != nullptr);

    // =================================================================
    // PATCH 2: GUARDED VIEW COUNT CHECK (REPLACES YOUR OLD IF BLOCK)
    // =================================================================
    if (views.count > 1) {
        auto view_0 = views.data[0];
        auto view_1 = views.data[1];

        // Check if views are valid and actually rendering world geometry (not a 0x0 loading rect)
        bool is_valid_scene_view = (view_0 != nullptr && view_1 != nullptr);
        if (is_valid_scene_view) {
            auto init_options_0 = (sdk::FSceneViewInitOptions*)((uintptr_t)view_0 + INIT_OPTIONS_OFFSET);

            // Read the rect memory directly as 4 32-bit integers: [left, top, right, bottom]
            const int32_t* rect_raw = (const int32_t*)&init_options_0->view_rect;

            const int32_t width = rect_raw[2] - rect_raw[0];  // right - left
            const int32_t height = rect_raw[3] - rect_raw[1]; // bottom - top

            // If width or height is invalid, the game is drawing a loading screen or canvas UI
            if (width <= 0 || height <= 0) {
                is_valid_scene_view = false;
            }
        }
        // If loading or viewports aren't populated, fall back to native single-pass safely
        if (!is_valid_scene_view) {
            g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
            return;
        }

        views.count = 1;
        wants_swap = true;

        // We now hold a validated FSceneViewFamily and both constructed eye views - use them to pin
        // down the live FSceneView field offsets by value (see sceneview_xref).
        sceneview_xref::resolve_live(view_family, view_0, view_1);

        auto runtime = vr->get_runtime();
        const auto frame_count = runtime->internal_frame_count;

        // Clone VR state from last frame to this frame
        if (runtime->is_openxr()) {
            auto openxr = (runtimes::OpenXR*)runtime;
            std::scoped_lock __{openxr->sync_assignment_mtx};

            const auto last_frame = (frame_count) % runtimes::OpenXR::QUEUE_SIZE;
            const auto now_frame = (frame_count + 1) % runtimes::OpenXR::QUEUE_SIZE;
            openxr->pipeline_states[now_frame] = openxr->pipeline_states[last_frame];
            openxr->pipeline_states[now_frame].frame_count = now_frame;
        } else {
            auto openvr = (runtimes::OpenVR*)runtime;
            std::unique_lock __{openvr->pose_mtx};

            const auto last_frame = (frame_count) % openvr->pose_queue.size();
            const auto now_frame = (frame_count + 1) % openvr->pose_queue.size();
            openvr->pose_queue[now_frame] = openvr->pose_queue[last_frame];
        }
    
    // =================================================================

        /*auto init_options = (sdk::FSceneViewInitOptions*)((uintptr_t)view_family.views.data[0] + INIT_OPTIONS_OFFSET);
        init_options->stereo_pass = 0;

        auto init_options2 = (sdk::FSceneViewInitOptions*)((uintptr_t)view_family.views.data[1] + INIT_OPTIONS_OFFSET);
        init_options2->stereo_pass = 0;

        std::array<uint8_t, 0x500> init_options_copy{};
        std::array<uint8_t, 0x500> init_options_copy2{};

        memcpy(init_options_copy.data(), init_options, 0x500);
        view_family.views.data[0]->constructor((sdk::FSceneViewInitOptions*)init_options_copy.data()); // Triggers our hook as well

        memcpy(init_options_copy2.data(), init_options2, 0x500);
        view_family.views.data[1]->constructor((sdk::FSceneViewInitOptions*)init_options_copy2.data());*/
    }

    // Pass 1: Render Left Eye
    // DIAG: confirm the engine is actually being asked to render a view here at all, and that the
    // view handed to it has a non-degenerate rect (width/height > 0). If this pass is skipped or
    // repeatedly logs a 0-sized rect while the right-eye pass (below, post-swap) keeps logging a
    // valid rect, the engine itself is failing to produce left-eye content upstream of any of our
    // copy/compositor code - i.e. NOT a bug in D3D12Component's AFR/NSF compositing.
    //
    // Eye-desync guard: we can't currently rely on a per-view "this is a camera cut" signal (the
    // FSceneView::FSceneView constructor hook fails to install), so we approximate one here by
    // comparing this frame's Pass1 (left) view rect against last frame's. A rect change (menu
    // open/close resolution swap, splitscreen layout change, etc.) is exactly the kind of frame
    // where Pass2 (right eye) borrowing/reusing Pass1 state below (shadow eye-field copy, nulled
    // view state) is most likely to desync the two eyes for a frame or two. When detected, we skip
    // those borrow/null hacks for this single frame so Pass2 renders fully on its own state instead
    // of a stale/mismatched one.
    //
    // NOTE: an earlier version of this guard also tried to detect camera cuts/refocus by a raw
    // frame-to-frame view_origin distance threshold. Logs showed that heuristic misfiring on
    // ordinary fast camera/character movement (dashing/running easily exceeds any fixed uu/frame
    // threshold), so it was forcing a Pass2 view-state reset almost every frame during motion -
    // likely making the desync worse rather than better. Removed; the rect-change signal below is
    // kept since it's a discrete, reliable event (resolution/layout only changes on real UI
    // transitions, never during normal camera motion).
    bool nsf_pass2_hacks_safe_this_frame = true;
    bool nsf_force_reset_pass2_state_this_frame = false;
    {
        static uint32_t diag_pass1_count = 0;
        ++diag_pass1_count;

        static bool has_prev_pass1_rect = false;
        static int32_t prev_pass1_rect[4] = {0, 0, 0, 0};

        if (wants_swap && views.data[0] != nullptr) {
            auto init_options_pass1 = (sdk::FSceneViewInitOptions*)((uintptr_t)views.data[0] + INIT_OPTIONS_OFFSET);
            const int32_t* rect_raw_pass1 = (const int32_t*)&init_options_pass1->view_rect;
            const int32_t pass1_w = rect_raw_pass1[2] - rect_raw_pass1[0];
            const int32_t pass1_h = rect_raw_pass1[3] - rect_raw_pass1[1];

            if (has_prev_pass1_rect &&
                (rect_raw_pass1[0] != prev_pass1_rect[0] || rect_raw_pass1[1] != prev_pass1_rect[1] ||
                 rect_raw_pass1[2] != prev_pass1_rect[2] || rect_raw_pass1[3] != prev_pass1_rect[3])) {
                nsf_pass2_hacks_safe_this_frame = false;

                // DIAG (animation-transition double-vision): forcing a hard reset of Pass2's live
                // view state on this transient frame (nulling FSceneViewState) was found to itself be
                // the likely source of a one-frame stereo divergence right at the transition, since it
                // discards occlusion/TAA history for one eye only while the other eye keeps its state.
                // Leaving nsf_force_reset_pass2_state_this_frame=false here means we still skip the
                // borrow hacks (still safe) but no longer forcibly null Pass2's state on this frame.
                nsf_force_reset_pass2_state_this_frame = false;

                // The rect jump also means the view target is about to need reallocating to the new
                // (portrait, single-camera) size. The normal reallocation path debounces for 5 frames
                // (~1s) to avoid thrashing on transient 2D-screen backbuffer resizes, but that same
                // debounce leaves the render target mismatched against the engine's already-changed
                // view rect for the whole debounce window during this animation-transition case,
                // which manifests as the double-vision persisting/slowly resolving. Force the
                // reallocation to happen immediately instead of waiting out the debounce.
                g_hook->set_should_recreate_textures(true);

                SPDLOG_INFO("[VR] NSF: Pass1 view_rect changed since last frame (({},{})-({},{}) -> ({},{})-({},{})); "
                    "skipping Pass2 state-borrow hacks this frame to avoid eye desync (forcing immediate view target reallocation, current generation={})",
                    prev_pass1_rect[0], prev_pass1_rect[1], prev_pass1_rect[2], prev_pass1_rect[3],
                    rect_raw_pass1[0], rect_raw_pass1[1], rect_raw_pass1[2], rect_raw_pass1[3],
                    g_hook->get_view_target_generation());
            }

            memcpy(prev_pass1_rect, rect_raw_pass1, sizeof(prev_pass1_rect));
            has_prev_pass1_rect = true;

            if (diag_pass1_count <= 20 || diag_pass1_count % 301 == 1 || pass1_w <= 0 || pass1_h <= 0) {
                SPDLOG_INFO("[DIAG] begin_render_viewfamily_real Pass1 (left, #{}): view_rect=({},{})-({},{}) w={} h={}",
                    diag_pass1_count, rect_raw_pass1[0], rect_raw_pass1[1], rect_raw_pass1[2], rect_raw_pass1[3], pass1_w, pass1_h);
            }
        }
    }
    g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);

    if (wants_swap) {
        // Swap out the existing render target for our custom one
        // Also, the entire point of swapping the render target
        // instead of "just" re-using the existing one is that doing that causes a 90% FPS drop
        // because the engine is still working on the old render target
        const auto original_target = view_family_target;

        view_family->set_render_target(rtfrt);

        auto scene = (sdk::FScene*)view_family->get_scene_interface();

        if (scene != nullptr) {
            // Decrement frame count to fix motion vectors in the right eye.
            // NOTE: this manipulates the Scene's global frame counter, which whole-scene shadow
            // caching/scheduling also keys off of (e.g. a "shadows already rendered this frame"
            // skip-optimization for a given light, reset once per frame). Since Pass1 and Pass2
            // both render through the SAME FSceneViewFamily/Scene without the engine's normal
            // per-frame increment happening between them, decrementing (or leaving it alone)
            // still leaves Pass2 looking like "the same or an earlier frame" to that optimization,
            // which may be why large-world shadows only update for Pass1 (left eye). Use the
            // "DIAG: NSF Pass2 Frame Count Mode" debug combo to A/B test None/Increment against
            // the current default (Decrement) - Increment intentionally looks like "a new frame"
            // to that logic, at the risk of a motion vector artifact, to isolate the true cause.
            //
            // DIAG: animation-blur investigation. The scene's frame count also gates many
            // double-buffered animation/render systems (GPU skin cache "previous frame" bone
            // buffer selection, cloth/anim-blueprint interpolation alpha, TAA/motion-vector
            // history). If Pass2's mutated count causes those systems to read a STALE buffer
            // (the pose from before an animation-driven camera transition/reset instead of the
            // just-updated one), that would manifest as exactly what's reported: the right eye
            // (or whichever eye is Pass2 this frame) momentarily showing an old/default pose
            // during the transition, resolving once motion stops. Log the scene frame count
            // immediately before/after the mutation, tagged with the active mode and a
            // high-resolution timestamp, so a captured log can be correlated against the exact
            // moment a visual glitch is observed.
            const auto pre_mutation_frame_count = scene->get_frame_count();
            const auto mutation_mode = vr->get_diag_nsf_pass2_frame_count_mode();

            switch (mutation_mode) {
            case 1: // None
                break;
            case 2: // Increment
                scene->increment_frame_count();
                break;
            default: // Decrement (original behavior)
                scene->decrement_frame_count();
                break;
            }

            static const char* const mode_names[] = {"Decrement", "None", "Increment"};
            const auto mode_name = (mutation_mode >= 0 && mutation_mode <= 2) ? mode_names[mutation_mode] : "Unknown";
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            SPDLOG_INFO_EVERY_N_SEC(1,
                "[NSF-ANIM-DIAG] Pass2 scene frame_count mutation: mode={} pre={} post={} g_frame_count={} t_us={}",
                mode_name, pre_mutation_frame_count, scene->get_frame_count(), g_frame_count, now_us);
        }

        std::swap(views[0], views[1]);

        // DIAG: same check as Pass 1 above, but for the post-swap (right eye / scene capture)
        // pass. Only meaningful when Native Stereo Fix is enabled, since this whole function
        // early-returns otherwise (see the is_native_stereo_fix_enabled() check near the top).
        {
            static uint32_t diag_pass2_count = 0;
            ++diag_pass2_count;

            if (views.data[0] != nullptr) {
                auto init_options_pass2 = (sdk::FSceneViewInitOptions*)((uintptr_t)views.data[0] + INIT_OPTIONS_OFFSET);
                const int32_t* rect_raw_pass2 = (const int32_t*)&init_options_pass2->view_rect;
                const int32_t pass2_w = rect_raw_pass2[2] - rect_raw_pass2[0];
                const int32_t pass2_h = rect_raw_pass2[3] - rect_raw_pass2[1];

                if (diag_pass2_count <= 20 || diag_pass2_count % 301 == 1 || pass2_w <= 0 || pass2_h <= 0) {
                    SPDLOG_INFO("[DIAG] begin_render_viewfamily_real Pass2 (right/NSF, #{}): view_rect=({},{})-({},{}) w={} h={}",
                        diag_pass2_count, rect_raw_pass2[0], rect_raw_pass2[1], rect_raw_pass2[2], rect_raw_pass2[3], pass2_w, pass2_h);
                }
            }
        }

        // Call it again
        // Right Eye Shadow Fix: the right-eye view was constructed as the SECONDARY eye. Whole-scene
        // shadow setup only runs for the shadow-owning (primary) view; secondaries are expected to
        // reuse it, but during Pass2 the right eye renders alone and finds none - hence no world
        // shadows in the right eye. While Pass2 renders, give the live FSceneView the left eye's
        // identity metadata (StereoPass + its cached copy, view index, primary flag - all discovered
        // at runtime by sceneview_xref; camera/rects untouched) and restore afterwards. Both the enum
        // and its cached copy must be flipped for the engine to honor it.
        auto pass2_view = views.data[0];
        std::vector<std::pair<uint32_t*, uint32_t>> pass2_restore{};

        if (vr->is_native_stereo_fix_right_eye_shadows_enabled() && pass2_view != nullptr && nsf_pass2_hacks_safe_this_frame) {
            const auto& fields = sceneview_xref::eye_fields;

            if (!fields.empty()) {
                const auto mask = vr->get_diag_nsf_pass2_eye_field_mask();

                for (size_t i = 0; i < fields.size(); ++i) {
                    if (i < 32 && (mask & (1u << i)) == 0) continue;
                    const auto& f = fields[i];
                    auto p = (uint32_t*)((uintptr_t)pass2_view + f.offset);
                    if (*p == f.right) {
                        pass2_restore.emplace_back(p, *p);
                        *p = f.left;
                    }
                }

                SPDLOG_INFO_EVERY_N_SEC(2, "[VR] NSF right-eye shadow fix: view={:x} flipped {}/{} eye fields (mask={:x})",
                    (uintptr_t)pass2_view, pass2_restore.size(), fields.size(), mask);
            } else {
                SPDLOG_INFO_EVERY_N_SEC(2, "[VR] NSF right-eye shadow fix: eye fields not yet resolved by sceneview_xref (view={:x})", (uintptr_t)pass2_view);
            }
        }

        // View State diagnostic / A/B for the "distant foliage frozen in the second-rendered eye" issue.
        // HISM foliage stores occlusion/visibility results in FSceneViewState and reads them back next
        // frame. If both passes resolve to the same state, Pass2 consumes Pass1's results and renders a
        // stale culling set for far clusters. Nulling Pass2's State disables occlusion history for that
        // pass entirely (everything visible, no TAA history) - a clean test of the hypothesis.
        void** pass2_state_slot = nullptr;
        void* pass2_state_saved = nullptr;

        if (pass2_view != nullptr && sceneview_xref::live_scene_state_offset.has_value()) {
            const auto off = sceneview_xref::live_scene_state_offset.value();
            pass2_state_slot = (void**)((uintptr_t)pass2_view + off);
            void* pass1_state = views.data[1] != nullptr ? *(void**)((uintptr_t)views.data[1] + off) : nullptr;

            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] NSF view state: pass1(left)={:x} pass2(right)={:x} shared={} known_states={}",
                (uintptr_t)pass1_state, (uintptr_t)*pass2_state_slot, pass1_state == *pass2_state_slot,
                g_hook->m_sceneview_data.known_scene_states.size());

            if ((vr->is_native_stereo_fix_null_pass2_view_state_enabled() && nsf_pass2_hacks_safe_this_frame) ||
                nsf_force_reset_pass2_state_this_frame) {
                pass2_state_saved = *pass2_state_slot;
                *pass2_state_slot = nullptr;

                if (nsf_force_reset_pass2_state_this_frame) {
                    SPDLOG_INFO("[VR] NSF: forcing Pass2 view state reset this frame due to detected camera cut/refocus");
                }
            } else {
                pass2_state_slot = nullptr;
            }
        } else if (pass2_view != nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] NSF view state: live scene state offset not yet resolved");
        }

        // Foliage wind investigation: find per-frame state Pass2 never sees fresh. Sample both eye views and the
        // family for N frames; report (a) view dwords that change every frame AND are identical between eyes
        // (a per-frame value that is stamped once for the family, not per view), (b) family dwords that change
        // every frame. Camera matrices differ between eyes so they are excluded by (a) automatically.
        if (vr->diag_nsf_frame_diff_logger() && pass2_view != nullptr && views.data[1] != nullptr) {
            constexpr size_t VIEW_SPAN = 0x1000 / 4;
            constexpr size_t FAMILY_SPAN = 0x400 / 4;
            constexpr uint32_t SAMPLE_FRAMES = 60;

            struct Diff {
                uint32_t frames{0};
                std::array<uint32_t, VIEW_SPAN> prev_left{}, prev_right{};
                std::array<uint32_t, FAMILY_SPAN> prev_family{};
                std::array<uint32_t, VIEW_SPAN> view_changes{}, view_eye_equal{};
                std::array<uint32_t, FAMILY_SPAN> family_changes{};
            };
            static std::unique_ptr<Diff> d{};
            if (!d) d = std::make_unique<Diff>();

            const auto left = (const uint32_t*)views.data[1];  // post-swap: [1] is the left eye (Pass1) view
            const auto right = (const uint32_t*)pass2_view;
            const auto fam = (const uint32_t*)view_family;

            if (d->frames > 0) {
                for (size_t i = 0; i < VIEW_SPAN; ++i) {
                    if (left[i] != d->prev_left[i] && right[i] != d->prev_right[i]) d->view_changes[i]++;
                    if (left[i] == right[i]) d->view_eye_equal[i]++;
                }
                for (size_t i = 0; i < FAMILY_SPAN; ++i) {
                    if (fam[i] != d->prev_family[i]) d->family_changes[i]++;
                }
            }

            memcpy(d->prev_left.data(), left, sizeof(d->prev_left));
            memcpy(d->prev_right.data(), right, sizeof(d->prev_right));
            memcpy(d->prev_family.data(), fam, sizeof(d->prev_family));
            d->frames++;

            if (d->frames > SAMPLE_FRAMES) {
                const uint32_t n = d->frames - 1;
                SPDLOG_INFO("[NSF-DIFF] ---- {} frames sampled. view={:x}/{:x} family={:x} state_off={:x} stereo_off={:x} ----",
                    n, (uintptr_t)left, (uintptr_t)right, (uintptr_t)fam,
                    sceneview_xref::live_scene_state_offset.value_or(0), sceneview_xref::live_stereo_pass_offset.value_or(0));

                SPDLOG_INFO("[NSF-DIFF] FSceneView dwords changing every frame AND equal in both eyes (candidates for stale per-frame state):");
                for (size_t i = 0; i < VIEW_SPAN; ++i) {
                    if (d->view_changes[i] >= n * 9 / 10 && d->view_eye_equal[i] >= n * 9 / 10) {
                        SPDLOG_INFO("[NSF-DIFF]   view+{:04x}: u32={} f32={:.6f}", i * 4, left[i], *(const float*)&left[i]);
                    }
                }

                SPDLOG_INFO("[NSF-DIFF] FSceneView dwords changing every frame but DIFFERENT per eye (matrices/eye-specific, for reference):");
                uint32_t shown = 0;
                for (size_t i = 0; i < VIEW_SPAN && shown < 40; ++i) {
                    if (d->view_changes[i] >= n * 9 / 10 && d->view_eye_equal[i] < n / 10) {
                        SPDLOG_INFO("[NSF-DIFF]   view+{:04x}: L f32={:.4f} R f32={:.4f}", i * 4, *(const float*)&left[i], *(const float*)&right[i]);
                        ++shown;
                    }
                }

                SPDLOG_INFO("[NSF-DIFF] FSceneViewFamily dwords changing every frame:");
                for (size_t i = 0; i < FAMILY_SPAN; ++i) {
                    if (d->family_changes[i] >= n * 9 / 10) {
                        SPDLOG_INFO("[NSF-DIFF]   family+{:04x}: u32={} f32={:.6f}", i * 4, fam[i], *(const float*)&fam[i]);
                    }
                }

                SPDLOG_INFO("[NSF-DIFF] ---- done ----");
                d.reset();
                vr->diag_nsf_frame_diff_logger() = false;
            }
        }

        // DIAG: animation-blur investigation, continued. Time the actual nested engine render
        // call for Pass2 (right eye). If this call occasionally takes far longer than usual
        // (e.g. during a camera-transition animation with heavier skinning/blend work), the HMD
        // pose/animation state sampled for Pass2 could be measurably staler than what Pass1 saw
        // moments earlier for the same nominal frame, producing a transient blur/mismatch that
        // clears once per-frame cost normalizes. Logged only when notably slower than a rolling
        // baseline to avoid spamming on ordinary frame-to-frame jitter.
        const auto pass2_render_start = std::chrono::steady_clock::now();
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        const auto pass2_render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pass2_render_start).count();

        {
            static double s_pass2_avg_ms = 0.0;
            static bool s_have_pass2_avg = false;

            if (!s_have_pass2_avg) {
                s_pass2_avg_ms = pass2_render_ms;
                s_have_pass2_avg = true;
            }

            if (pass2_render_ms > s_pass2_avg_ms * 2.5 && pass2_render_ms > 2.0) {
                SPDLOG_INFO("[NSF-ANIM-DIAG] Pass2 render call spike: {:.2f}ms (rolling avg {:.2f}ms) g_frame_count={}",
                    pass2_render_ms, s_pass2_avg_ms, g_frame_count);
            }

            // Exponential moving average so the baseline adapts slowly without being skewed by
            // any single spike frame.
            s_pass2_avg_ms = (s_pass2_avg_ms * 0.95) + (pass2_render_ms * 0.05);
        }

        if (pass2_state_slot != nullptr) {
            *pass2_state_slot = pass2_state_saved;
        }

        for (auto& [p, v] : pass2_restore) {
            *p = v;
        }

        // Restore view order & original target
        std::swap(views[0], views[1]);
        view_family->set_render_target(original_target);
    }

    views.count = prev_count;
}

void FFakeStereoRenderingHook::begin_render_viewfamily(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("BeginRenderViewFamily");

    SPDLOG_INFO_ONCE("Called BeginRenderViewFamily for the first time");

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    sdk::FSceneViewFamily::update_offsets(&view_family, g_hook->get_render_target_manager()->get_viewport());
    auto si = view_family.get_scene_interface();

    if (si != nullptr) {
        sdk::FScene::update_offsets((sdk::FScene*)si);
    }

    if (!g_hook->has_engine_tick_hook()) {
        // Alternative place of running game thread work.
        GameThreadWorker::get().execute();
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    const auto frame_count = *(uint32_t*)((uintptr_t)&view_family + SceneViewExtensionAnalyzer::frame_count_offset);
    auto views_ptr = view_family.get_views();

    //vr->update_hmd_state(true, frame_count);
    auto runtime = vr->get_runtime();
    runtime->internal_frame_count = frame_count;
    runtime->on_pre_render_game_thread(frame_count);

    // This is a HACKHACKHACK to get splitscreen working on around 4.20 to 4.27 something
    // This is completely borked on UE5
    // We can probably do it better inside the sceneview constructor hook, but that needs to be handled with care
    if (vr->is_splitscreen_compatibility_enabled() && views_ptr != nullptr) {
        auto& views = *views_ptr;
        
        // B = dst, A = src
        static auto copy_init_options_from = [](const sdk::FSceneView& a, sdk::FSceneView& b) {
            std::scoped_lock _{g_hook->m_sceneview_data.mtx};
            auto init_options_a = (sdk::FSceneViewInitOptions*)((uintptr_t)&a + INIT_OPTIONS_OFFSET);
            auto init_options_b = (sdk::FSceneViewInitOptions*)((uintptr_t)&b + INIT_OPTIONS_OFFSET);

            auto& cached_init_options = g_hook->m_sceneview_data.view_init_options_ue4;

            if (auto it = cached_init_options.find(init_options_a->scene_view_state); it != cached_init_options.end()) {
                const auto& vio_entry = it->second;
                //memcpy(init_options_b, &vio_entry, sizeof(sdk::FSceneViewInitOptionsUE4));
                init_options_b->view_origin = vio_entry.view_origin;
                init_options_b->view_rotation_matrix = vio_entry.view_rotation_matrix;
                *(FIntRect*)&init_options_b->view_rect = *(FIntRect*)&vio_entry.view_rect;
                *(FIntRect*)&init_options_b->constrained_view_rect = *(FIntRect*)&vio_entry.constrained_view_rect;
                init_options_b->projection_matrix = vio_entry.projection_matrix;
                return;
            }

            // Otherwise just do this crap
            init_options_b->view_origin = init_options_a->view_origin;
            init_options_b->view_rotation_matrix = init_options_a->view_rotation_matrix;
            *(FIntRect*)&init_options_b->view_rect = *(FIntRect*)&init_options_a->view_rect;
            *(FIntRect*)&init_options_b->constrained_view_rect = *(FIntRect*)&init_options_a->constrained_view_rect;
            init_options_b->projection_matrix = init_options_a->projection_matrix;
        };

        auto do_splitscreen = [&](int32_t view_index) {
            int32_t w = vr->get_hmd_width();
            int32_t h = vr->get_hmd_height();

            int32_t x = 0;
            int32_t y = 0;

            const auto true_index = vr->is_using_afr() ? (frame_count + 1) % 2 : view_index;

            if (!vr->is_using_afr() && true_index == 1) {
                x += w;
            }

            auto view = views.data[view_index % views.count];

            FIntRect view_rect{x, y, x + w, y + h};

            auto& vr = VR::get();

            VR::get()->get_runtime()->update_matrices(0.1f, 10000.0f);

            const auto proj_mat = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));

            std::array<uint8_t, 0x500> init_options_copy{};

            auto init_options = (sdk::FSceneViewInitOptions*)((uintptr_t)view + INIT_OPTIONS_OFFSET);

            auto& init_options_view_origin = init_options->view_origin;
            auto& init_options_view_rotation_matrix = init_options->view_rotation_matrix;
            auto& init_options_view_rect = *(FIntRect*)&init_options->view_rect;
            auto& init_options_constrained_view_rect = *(FIntRect*)&init_options->constrained_view_rect;
            auto& init_options_projection_matrix = init_options->projection_matrix;
            auto& init_options_stereo_pass = init_options->stereo_pass;

            // ADDENDUM: The sceneview constructor hook handles the rotation logic now.
            /*const auto conversion_mat = glm::mat4 {
                0, 0, 1, 0,
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 0, 1
            };

            const auto conversion_mat_inverse = glm::inverse(conversion_mat);*/

            // We need to "undo" the operations done to create the rotation matrix so we can get the original angle
            // const auto view_rot_mat = conversion_mat * make_inverse_rot_matrix(euler); <-- this is the result of the conversion
            //auto euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * init_options_view_rotation_matrix));
            //g_hook->calculate_stereo_view_offset_(true_index + 1, (Rotator<float>*)&euler, 100.0f, &init_options_view_origin);
            //const auto view_rot_mat = conversion_mat * utility::math::ue_inverse_rotation_matrix(euler);
            //init_options_view_rotation_matrix = view_rot_mat;

            init_options_view_rect = view_rect;
            init_options_constrained_view_rect = view_rect;
            init_options_projection_matrix = proj_mat;

            memcpy(init_options_copy.data(), init_options, 0x500);
            view->constructor((sdk::FSceneViewInitOptions*)init_options_copy.data()); // Triggers our hook as well
        };

        const auto requested_index = vr->get_requested_splitscreen_index();
        const auto final_index = std::min<uint32_t>(views.count - 1, requested_index);
        const auto other_index = final_index != 0 ? 0 : 1;

        if (final_index > 0) {
            if (views.count > 1) {
                copy_init_options_from(*views.data[final_index], *views.data[other_index]);
            }

            if (!vr->is_using_afr()) {
                if (views.count > 1) {
                    do_splitscreen(other_index);
                } else {
                    do_splitscreen(0);
                }
            } else {
                do_splitscreen(0);
            }
        }
    }

    // If we couldn't find GetDesiredNumberOfViews, we need to set the view count to 1 as a workaround
    // TODO: Check if this can cause a memory leak, I don't know who is resonsible
    // for destroying the views in the array
    // This check might seem kind of arbitrary, but sometimes (rarely) the offset
    // for the views can be wrong so if the count is some sane number
    // then we can assume that the offset is correct
    if (vr->is_using_afr() && views_ptr != nullptr && views_ptr->count >= 2 && views_ptr->count <= 4) {
        SPDLOG_INFO_ONCE("Setting view count to 1 (from {})", views_ptr->count);
        views_ptr->count = 1;
    }


    using BeginRenderViewFamilyRealFn = void(*)(void*, sdk::FCanvas*, sdk::FSceneViewFamily*);
    static BeginRenderViewFamilyRealFn begin_rendering_view_family_real_fn = nullptr;
    // NOTE: this used to be a one-shot latch (a static "already_tried" bool) gated on
    // vr->is_native_stereo_fix_enabled() being true at the exact moment this function first ran.
    // If NSF happened to be disabled (or not yet toggled on) on that single frame, "already_tried"
    // was permanently set to true and the real BeginRenderViewFamily hook was NEVER attempted again
    // for the rest of the session - silently disabling the entire downstream visual path (scene
    // capture, AFR eye compositing, etc.) even though the game kept running normally. Retry every
    // frame instead, regardless of NSF state, until the hook is actually installed.
    if (begin_rendering_view_family_real_fn == nullptr && !g_hook->m_render_module_begin_render_viewfamily_hook) {
        // Get callstack
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
        uintptr_t mid = 0;

        for (int i = 1; i < depth; i++) {
            SPDLOG_INFO(" {:x}", (uintptr_t)stack[i]);
            mid = stack[i];
            break;
        }

        if (mid != 0) {
            const auto candidate = utility::find_virtual_function_start(mid);

            if (candidate) {
                begin_rendering_view_family_real_fn = (BeginRenderViewFamilyRealFn)*candidate;

                if (begin_rendering_view_family_real_fn != nullptr) {
                    SPDLOG_INFO("Found BeginRenderingViewFamily real function at {:x}", (uintptr_t)begin_rendering_view_family_real_fn);

                    g_hook->m_render_module_begin_render_viewfamily_hook = safetyhook::create_inline((uintptr_t)begin_rendering_view_family_real_fn, (uintptr_t)&begin_render_viewfamily_real);

                    if (g_hook->m_render_module_begin_render_viewfamily_hook) {
                        SPDLOG_INFO("Hooked BeginRenderingViewFamily real function");
                    } else {
                        SPDLOG_ERROR("Failed to hook BeginRenderingViewFamily real function");
                    }
                } else {
                    SPDLOG_ERROR("Failed to find BeginRenderingViewFamily real function");
                }
            } else {
                SPDLOG_ERROR("Failed to find BeginRenderingViewFamily real function");
            }
        }
    }
}

void FFakeStereoRenderingHook::pre_render_viewfamily_renderthread(ISceneViewExtension* extension, sdk::FRHICommandListBase* cmd_list, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("PreRenderViewFamily_RenderThread");

    utility::ScopeGuard _{[]() {
        RenderThreadWorker::get().execute();
    }};
    
    SPDLOG_INFO_ONCE("Called PreRenderViewFamily_RenderThread for the first time");
    
    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    static size_t execution_count{0};

    // This should 100% only get executed if the headset is on, because
    // FFakeStereoRenderingHook::render_texture_render_thread is the first fallback for hooking
    // And we don't want to miss that unintentionally
    if (g_hook->m_attempted_hook_slate_thread && !g_hook->m_slate_thread_hook && !g_hook->m_attempted_hook_slate_thread_alternate && execution_count++ >= 50) {
        SPDLOG_INFO("DrawWindow_RenderThread was not hooked after {} render calls, trying alternative hook", execution_count);

        g_hook->attempt_hook_slate_thread(0, true);
    }

    if (vr->is_stereo_emulation_enabled()) {
        return;
    }

    diag_dump_engine_view_extensions(view_family);

    const auto frame_count = *(uint32_t*)((uintptr_t)&view_family + SceneViewExtensionAnalyzer::frame_count_offset);
    static uint32_t last_frame = 0;

    // We only want to run this logic on the first "frame" (left eye) passed through here
    // When using Native Stereo Fix
    if (vr->is_native_stereo_fix_enabled() && frame_count == last_frame) {
        return;
    }

    last_frame = frame_count;

    static bool is_ue5_rdg_builder = false;
    static uint32_t ue5_command_offset = 0;
    static bool analyzed_root_already = false;
    static bool is_old_command_base = false;

    if (is_ue5_rdg_builder) {
        cmd_list = *(sdk::FRHICommandListBase**)((uintptr_t)cmd_list + ue5_command_offset);
    }

    const auto compensation = g_hook->get_frame_delay_compensation();

    // Using slate's draw window hook is the safest way to do this without
    // false positives on the command list in this function
    // otherwise we can attempt to use the command list here and hook it
    // in the slate hook, a guaranteed proper command list is passed to the function
    // so we can use that to hook the command list
    // The main inspiration for this is UE5.0.3 because it passes an FRDGBuilder
    // which *does* contain the command list in it, but for whatever reason I can't
    // seem to hook it properly, so I'm using the slate hook instead
    // ADDENDUM: For now, I'm only using the slate hook for UE5.0.3.
    // But I'll use it as a fallback as well for when the command list appears to be empty
    // Reason being the slate hook doesn't appear to run every frame, so it's not a perfect solution
    auto enqueue_poses_on_slate_thread = [&]() {
        g_hook->get_slate_thread_worker()->enqueue([=](FRHICommandListImmediate* command_list) {
            static bool once_slate = true;

            if (once_slate) {
                SPDLOG_INFO("Called enqueued function on the Slate thread for the first time! Frame count: {}", frame_count);
                once_slate = false;
            }

            static size_t actual_offset = 0;
            auto l = (sdk::FRHICommandListBase*)((uintptr_t)command_list + actual_offset);
            const auto is_ue5 = g_hook->has_double_precision();

            if (l != nullptr && l->root != nullptr && ((uintptr_t)l->root & (sizeof(void*) - 1)) == 0) {
                auto new_root = (sdk::FRHICommandBase_New*)l->root;
                if (!analyzed_root_already) try {
                    // so all of this might seem really overkill but
                    // it's a good way to detect whether we have an FMemStack at the top of the command list
                    // which we need to skip on UE5.5+
                    if (utility::get_module_within(*(void**)l->root).value_or(nullptr) == nullptr || 
                        IsBadReadPtr(*(void**)l->root, sizeof(void*)) || 
                        utility::get_module_within(**(void***)l->root).value_or(nullptr) == nullptr ||
                        (!IsBadReadPtr(new_root->next, sizeof(void*)) && (utility::get_module_within(*(void**)new_root->next).value_or(nullptr) == nullptr || utility::get_module_within(**(void***)new_root->next).value_or(nullptr) == nullptr))
                    )
                {
                        if (is_ue5) {
                            // UE5 is NOT an old command list, we need to bruteforce the offset
                            // Start at 0x10 because that's usually where the pointers in FMemStack end.
                            for (size_t i = 0x10; i < 0x50; i += sizeof(void*)) try {
                                const auto cur_l = (sdk::FRHICommandListBase*)((uintptr_t)command_list + i);
                                if (utility::get_module_within(*(void**)cur_l->root).value_or(nullptr) != nullptr) {
                                    actual_offset = i;
                                    l = cur_l;
                                    SPDLOG_INFO("Found UE5.5+ command list at offset 0x{:x}", i);
                                    break;
                                }
                            } catch(...) {

                            }
                        } else {
                            SPDLOG_INFO("Old FRHICommandBase detected");
                            is_old_command_base = true;
                        }
                    } else {
                        SPDLOG_INFO("New FRHICommandBase detected");
                    }

                    analyzed_root_already = true;
                } catch(...) {
                    SPDLOG_ERROR("Failed to analyze FRHICommandBase");
                    analyzed_root_already = true;
                }

                if (!is_old_command_base) {
                    SceneViewExtensionAnalyzer::hook_new_rhi_command((sdk::FRHICommandBase_New*)l->root, frame_count + compensation);
                } else {
                    SceneViewExtensionAnalyzer::hook_old_rhi_command((sdk::FRHICommandBase_Old*)l->root, frame_count + compensation);
                }
            } else {
                // welp
                vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
            }
        });
    };

    // FIX: this previously read "(uintptr_t)cmd_list & 1 == 0", which due to operator precedence
    // (== binds tighter than &) actually parsed as "cmd_list & (1 == 0)" -> "cmd_list & 0" -> always 0,
    // making has_good_root always false and forcing every frame down the Slate-thread fallback path
    // below, regardless of whether cmd_list/root were actually valid. Parenthesize the comparisons
    // explicitly so the alignment check is evaluated correctly.
    const auto has_good_root = 
        cmd_list != nullptr &&
        (((uintptr_t)cmd_list & 1) == 0) &&
        cmd_list->root != nullptr &&
        (((uintptr_t)cmd_list->root & 1) == 0);

    if (!has_good_root) {
        static uint32_t diag_eval_count = 0;
        if (++diag_eval_count % 300 == 1) {
            const auto cmd_list_null = cmd_list == nullptr;
            const auto cmd_list_aligned = cmd_list != nullptr && (((uintptr_t)cmd_list & 1) == 0);
            const auto root_null = cmd_list != nullptr && cmd_list->root == nullptr;
            const auto root_aligned = cmd_list != nullptr && cmd_list->root != nullptr && (((uintptr_t)cmd_list->root & 1) == 0);
            SPDLOG_INFO("[DIAG] has_good_root=false cmd_list={:x} cmd_list_null={} cmd_list_aligned={} root={:x} root_null={} root_aligned={}",
                (uintptr_t)cmd_list, cmd_list_null, cmd_list_aligned,
                cmd_list != nullptr ? (uintptr_t)cmd_list->root : 0, root_null, root_aligned);
        }
    }

    // Hijack the top command in the command list so we can enqueue the render poses on the RHI thread
    if (has_good_root) {
        SPDLOG_INFO_ONCE("Command list root is good");

        if (!analyzed_root_already) try {
            auto root = cmd_list->root;

            auto analyze_for_ue5 = [&]() {
                // Find the real command list.
                is_ue5_rdg_builder = true;
                const auto rdg_builder = (uintptr_t)cmd_list;

                for (auto i = 0x10; i <= 0x100; i += sizeof(void*)) try {
                    const auto value = *(uintptr_t*)(rdg_builder + i);

                    if (value == 0 || IsBadReadPtr((void*)value, sizeof(void*))) {
                        continue;
                    }

                    if (utility::get_module_within((void*)value).has_value()) {
                        continue;
                    }

                    const auto value_deref = *(uintptr_t*)value;

                    if (value_deref == 0 || IsBadReadPtr((void*)value_deref, sizeof(void*))) {
                        continue;
                    }

                    if (utility::get_module_within((void*)value_deref).has_value()) {
                        continue;
                    }

                    const auto root_vtable = *(uintptr_t*)value_deref;

                    if (root_vtable == 0 || IsBadReadPtr((void*)root_vtable, sizeof(void*))) {
                        continue;
                    }

                    if (!utility::get_module_within((void*)root_vtable).has_value()) {
                        continue;
                    }

                    // Check that there is a valid function in the vtable
                    const auto first_function = *(uintptr_t*)root_vtable;

                    if (first_function == 0 || IsBadReadPtr((void*)first_function, sizeof(void*))) {
                        continue;
                    }

                    if (!utility::get_module_within((void*)first_function).has_value()) {
                        continue;
                    }

                    SPDLOG_INFO("Possible UE5 command list found at offset 0x{:x}", i);
                    ue5_command_offset = i;
                    cmd_list = (sdk::FRHICommandListBase*)value;
                    break;
                } catch(...) {
                    spdlog::error("Exception occurred while analyzing UE5 command list");
                }
            };

            // If we read the pointer at the start of the root and it's not a module, then it's the old FRHICommandBase
            // this is because all vtables reside within a module
            if (utility::get_module_within(*(void**)root).value_or(nullptr) == nullptr) {
                // UE5
                if (g_hook->has_double_precision()) {
                    analyze_for_ue5();

                    if (ue5_command_offset == 0) {
                        SPDLOG_ERROR("Failed to find UE5 command list, trying again next frame");
                        return;
                    }
                } else {
                    SPDLOG_INFO("Old FRHICommandBase detected");
                    is_old_command_base = true;
                }
            } else {
                SPDLOG_INFO("New FRHICommandBase detected");
            }

            analyzed_root_already = true;
        } catch(...) {
            SPDLOG_ERROR("Failed to analyze root command");
            analyzed_root_already = true;
        }

        if (g_hook->get_render_target_manager()->is_ue_5_0_3() && g_hook->has_slate_hook()) {
            enqueue_poses_on_slate_thread();
        } else try {
            if (!is_old_command_base) {
                SceneViewExtensionAnalyzer::hook_new_rhi_command((sdk::FRHICommandBase_New*)cmd_list->root, frame_count + compensation);
            } else {
                SceneViewExtensionAnalyzer::hook_old_rhi_command((sdk::FRHICommandBase_Old*)cmd_list->root, frame_count + compensation);
            }
        } catch(...) {
            SPDLOG_INFO_ONCE("Failed to hook command list, falling back to Slate thread hook");

            if (g_hook->has_slate_hook()) {
                enqueue_poses_on_slate_thread();
            } else {
                vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
            }
        }
    } else {
        SPDLOG_INFO_ONCE("Bad root or command list, falling back to Slate thread hook");

        static uint32_t diag_fallback_count = 0;
        if (++diag_fallback_count % 300 == 1) {
            SPDLOG_INFO("[DIAG] fallback branch taken (#{}): cmd_list={:x} root={:x} is_ue5_rdg_builder={} ue5_command_offset={:x} analyzed_root_already={} is_old_command_base={} frame_count={}",
                diag_fallback_count, (uintptr_t)cmd_list, cmd_list != nullptr ? (uintptr_t)cmd_list->root : 0,
                is_ue5_rdg_builder, ue5_command_offset, analyzed_root_already, is_old_command_base, frame_count);
        }

        // welp v2
        if (g_hook->has_slate_hook()) {
            enqueue_poses_on_slate_thread();
        } else {
            vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
        }
    }
}

bool FFakeStereoRenderingHook::setup_view_extensions() try {
    SPDLOG_INFO("Attempting to set up view extensions...");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot set up view extensions!");
        return false;
    }

    const auto active_stereo_device = locate_active_stereo_rendering_device();

    if (!active_stereo_device || !s_stereo_rendering_device_offset) {
        SPDLOG_ERROR("Failed to locate active stereo rendering device!");
        return false;
    }

    // This is a proof of concept at the moment for newer UE versions
    // older versions may not work or crash.
    constexpr auto weak_ptr_size = sizeof(TWeakPtr<void*>);
    static const auto potential_hmd_device_offset = s_stereo_rendering_device_offset + weak_ptr_size;
    static const uintptr_t potential_hmd_device = (uintptr_t)engine + potential_hmd_device_offset;
    static const uintptr_t potential_view_extensions =
        (uintptr_t)engine + s_stereo_rendering_device_offset + (weak_ptr_size * 2); // 2 to skip over the XRSystem

    // This can happen if the game left a VR plugin in it
    // Usually this isn't an issue, but some games can leave a valid HMDDevice or XRSystem laying around for whatever reason
    // If this isn't cleaned up, the game will crash because it tries to gather view extensions from the existing device
    // and the view extensions it gathered will cause a crash when calling them. also the HMD device itself can cause a crash, it's not
    // actually initialized.
    if (*(void**)potential_hmd_device != nullptr) {
        // Double check that we're actually replacing a pointer and not an integer or something
        if (!IsBadReadPtr(*(void**)potential_hmd_device, sizeof(void*))) {
            SPDLOG_INFO("Found an existing HMDDevice or XRSystem, nullifying it...");
            static std::vector<uintptr_t> replacement_vtable{};

            for (auto i = 0; i < 200; ++i) {
                replacement_vtable.push_back((uintptr_t)+[]() { return nullptr; });
            }

            *(void**)potential_hmd_device = nullptr;
            m_fixed_localplayer_view_count = true; // If this is already allocated, then there's already a second view for us to use
        }

        if (!IsBadReadPtr(*(void**)(potential_hmd_device + sizeof(void*)), sizeof(void*))) {
            *(void**)(potential_hmd_device + sizeof(void*)) = nullptr;
        }
    }

    m_tracking_system_hook = std::make_unique<IXRTrackingSystemHook>(this, potential_hmd_device_offset);
    m_components.push_back(m_tracking_system_hook.get());

    // Add a vectored exception handler that catches attempted dereferences of a null XRSystem or HMDDevice
    // The exception handler will then patch out the instructions causing the crash and continue execution
    AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS exception) -> LONG {
        static std::vector<Patch::Ptr> xrsystem_patches{};
        static std::unordered_set<uintptr_t> ignored_addresses{};

        if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            const auto exception_address = exception->ContextRecord->Rip;

            // GENERALIZED RECOVERY for a known family of use-after-free crashes inside the game's own
            // Client-Win64-Shipping.exe. We've observed multiple *different* faulting addresses/rvas
            // (0x23e1f889, 0x23e2aa86, ...) that all share the same signature: a base register used to
            // dereference memory holds a stale/freed value rather than a real pointer, in a `MOV`/`MOVZX`
            // style load into another register. This points at one corrupted/freed object being touched by
            // many different call sites in the game binary, so hardcoding each rva individually is a
            // whack-a-mole game (and a "does this register value look like poison" heuristic is unreliable -
            // we already missed 0x3f80000000000000 which isn't a repeating-byte pattern). Since we only get
            // here on an actual EXCEPTION_ACCESS_VIOLATION for this exact instruction, the CPU has ALREADY
            // proven the dereferenced address is invalid - there's nothing left to "detect", we can recover
            // unconditionally for this instruction shape, scoped ONLY to this game's exe (never 3rd-party/
            // system/engine DLLs) so we don't mask unrelated bugs elsewhere, and skip the faulting
            // instruction (treating the load as returning 0) so the game can continue instead of crashing.
            //
            // This must run before (and bypass) the ignored_addresses gate below, since these crashes can
            // recur at different addresses across the run, and even the same address can legitimately need
            // recovering more than once per session (each frame the code path executes).
            {
                const auto ex_mod_precheck = utility::get_module_within((void*)exception_address);

                if (ex_mod_precheck.has_value()) {
                    const auto mod_path_precheck = utility::get_module_path(*ex_mod_precheck).value_or("");
                    const auto is_game_exe = mod_path_precheck.find("-Win64-Shipping.exe") != std::string::npos;

                    if (is_game_exe) {
                        const auto decoded_precheck = utility::decode_one((uint8_t*)exception_address);

                        if (decoded_precheck && decoded_precheck->OperandsCount == 2 &&
                            decoded_precheck->Operands[1].Type == ND_OP_MEM && decoded_precheck->Operands[1].Info.Memory.HasBase &&
                            decoded_precheck->Operands[0].Type == ND_OP_REG) {

                            const auto ctx = exception->ContextRecord;
                            DWORD64* const gpr_table[16] = {
                                &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
                                &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
                                &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
                                &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15,
                            };

                            const auto base_reg = decoded_precheck->Operands[1].Info.Memory.Base;

                            if (base_reg < 16) {
                                static std::unordered_set<uintptr_t> warned_rvas{};
                                const auto ex_rva = exception_address - (uintptr_t)*ex_mod_precheck;

                                if (g_hook != nullptr) {
                                    g_hook->report_uaf_recovery(ex_rva);
                                }

                                if (warned_rvas.insert(ex_rva).second) {
                                    SPDLOG_WARN("[Exception Handler] Applying generalized UAF recovery in game exe at rva {:x} (base_reg={} value={:x}, load result forced to 0)",
                                        ex_rva, (int)base_reg, *gpr_table[base_reg]);
                                }

                                const auto dst_reg = decoded_precheck->Operands[0].Info.Register.Reg;

                                if (dst_reg < 16) {
                                    *gpr_table[dst_reg] = 0;

                                    auto resume_addr = exception_address + decoded_precheck->Length;

                                    // Guard against a second, unrecoverable crash: if the very next
                                    // instruction is an indirect CALL/JMP that uses the register we just
                                    // zeroed out (as either its base/index memory operand or a bare
                                    // register operand), executing it as-is would jump/call through a
                                    // near-null address (e.g. "Bad read pointer at 74") - a crash we've
                                    // observed happen right after this recovery path fires. Since we've
                                    // already established the underlying object is stale/freed, the only
                                    // safe option is to also skip that indirect branch (treat it as a
                                    // no-op) rather than let it execute. This only ever affects the game
                                    // exe's own UAF-crash instructions caught here - it does not touch any
                                    // rendering/stereo/eye-pose code paths.
                                    const auto next_decoded = utility::decode_one((uint8_t*)resume_addr);

                                    if (next_decoded) {
                                        const std::string_view next_mnemonic{next_decoded->Mnemonic};

                                        if (next_mnemonic.starts_with("CALL") || next_mnemonic.starts_with("JMP")) {
                                            const auto& next_op0 = next_decoded->Operands[0];
                                            const bool uses_zeroed_reg =
                                                (next_op0.Type == ND_OP_REG && next_op0.Info.Register.Reg == dst_reg) ||
                                                (next_op0.Type == ND_OP_MEM && next_op0.Info.Memory.HasBase && next_op0.Info.Memory.Base == dst_reg) ||
                                                (next_op0.Type == ND_OP_MEM && next_op0.Info.Memory.HasIndex && next_op0.Info.Memory.Index == dst_reg);

                                            if (uses_zeroed_reg) {
                                                static std::unordered_set<uintptr_t> warned_branch_skip_rvas{};
                                                const auto branch_rva = resume_addr - (uintptr_t)*ex_mod_precheck;

                                                if (warned_branch_skip_rvas.insert(branch_rva).second) {
                                                    SPDLOG_WARN("[Exception Handler] Skipping indirect {} at rva {:x} that would branch through zeroed register {}",
                                                        next_decoded->Mnemonic, branch_rva, (int)dst_reg);
                                                }

                                                resume_addr += next_decoded->Length;
                                            }
                                        }
                                    }

                                    ctx->Rip = resume_addr;
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                        }

                        // Same UAF family, but for STORES instead of loads: e.g. MOV [reg+disp], reg2/imm,
                        // where the destination memory operand (Operands[0]) holds a stale/freed pointer.
                        // This is a WRITE access violation (as opposed to the read case above), and it cannot
                        // be recovered by forcing a register to 0 - there's nothing to write TO. Since the CPU
                        // has already proven the destination address is invalid, the only safe recovery is to
                        // skip the write entirely (treat it as a no-op) and continue at the next instruction.
                        // Scoped identically to the read case: game exe only, one-time warning per rva.
                        if (decoded_precheck && decoded_precheck->OperandsCount >= 1 &&
                            decoded_precheck->Operands[0].Type == ND_OP_MEM && decoded_precheck->Operands[0].Info.Memory.HasBase) {

                            const auto ctx = exception->ContextRecord;
                            DWORD64* const gpr_table[16] = {
                                &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
                                &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
                                &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
                                &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15,
                            };

                            const auto base_reg = decoded_precheck->Operands[0].Info.Memory.Base;

                            if (base_reg < 16) {
                                static std::unordered_set<uintptr_t> warned_store_rvas{};
                                const auto ex_rva = exception_address - (uintptr_t)*ex_mod_precheck;

                                if (g_hook != nullptr) {
                                    g_hook->report_uaf_recovery(ex_rva);
                                }

                                if (warned_store_rvas.insert(ex_rva).second) {
                                    SPDLOG_WARN("[Exception Handler] Applying generalized UAF store recovery in game exe at rva {:x} (base_reg={} value={:x}, write skipped)",
                                        ex_rva, (int)base_reg, *gpr_table[base_reg]);
                                }

                                ctx->Rip = exception_address + decoded_precheck->Length;
                                return EXCEPTION_CONTINUE_EXECUTION;
                            }
                        }
                    }
                }
            }

            if (ignored_addresses.contains(exception_address)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            ignored_addresses.insert(exception_address);

            if (exception_address == 0) {
                SPDLOG_INFO("[Exception Handler] Exception address is null");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (IsBadReadPtr((void*)exception_address, sizeof(void*))) {
                SPDLOG_INFO("[Exception Handler] Bad read pointer at {:x}", exception_address);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto decoded = utility::decode_one((uint8_t*)exception_address);

            if (!decoded) {
                SPDLOG_ERROR("[Exception Handler] Failed to decode instruction at {:x}", exception_address);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto& op2 = decoded->Operands[1];

            if (decoded->OperandsCount != 2 || op2.Type != ND_OP_MEM || !op2.Info.Memory.HasBase) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SPDLOG_INFO("Encountered attempted dereference of null pointer at {:x}", exception_address);

            {
                const auto ctx = exception->ContextRecord;
                const auto base_reg = op2.Info.Memory.Base;
                const auto disp = op2.Info.Memory.HasDisp ? (int64_t)op2.Info.Memory.Disp : 0;
                const auto ex_mod = utility::get_module_within((void*)exception_address);
                SPDLOG_INFO("[Exception Handler]   rva={:x} base_reg={} disp={:#x} rax={:x} rcx={:x} rdx={:x} rbx={:x} rsi={:x} rdi={:x} r8={:x} r9={:x} r12={:x} r13={:x} r14={:x} r15={:x}",
                    ex_mod.has_value() ? exception_address - (uintptr_t)*ex_mod : exception_address, (int)base_reg, disp,
                    ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->Rbx, ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9, ctx->R12, ctx->R13, ctx->R14, ctx->R15);
            }

            // Get the start of the previous instruction
            const auto previous_instruction = utility::resolve_instruction(exception_address - 1);

            if (!previous_instruction) {
                SPDLOG_ERROR("Could not resolve previous instruction at {:x}", exception_address - 1);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (previous_instruction->instrux.Operands[0].Type != ND_OP_REG ||
                previous_instruction->instrux.Operands[0].Info.Register.Reg != op2.Info.Memory.Base) {
                // This unmatched-fault path is not the known stale XRSystem/HMDDevice case this handler was
                // originally written for, so nothing gets patched and the exception propagates into a fatal
                // crash. Log the owning module + a short disassembly window so the *next* occurrence of this
                // crash tells us exactly what code (module name/rva) and instruction is actually faulting,
                // instead of just "Fatal error!" with no other clue.
                const auto ex_mod = utility::get_module_within((void*)exception_address);
                const auto mod_path = ex_mod.has_value() ? utility::get_module_path(*ex_mod).value_or("<unknown>") : std::string{"<unknown>"};
                const auto ex_rva = ex_mod.has_value() ? exception_address - (uintptr_t)*ex_mod : 0;
                SPDLOG_ERROR("Previous instruction does not use the same register as the dereference");
                SPDLOG_ERROR("[Exception Handler] module={} exception_addr={:x} exception_mnemonic={}",
                    mod_path, exception_address, decoded->Mnemonic);
                SPDLOG_ERROR("[Exception Handler] previous_instr_addr={:x} previous_mnemonic={} previous_op0_type={} previous_op1_type={}",
                    previous_instruction->addr, previous_instruction->instrux.Mnemonic,
                    (int)previous_instruction->instrux.Operands[0].Type, (int)previous_instruction->instrux.Operands[1].Type);

                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto prev_op2 = previous_instruction->instrux.Operands[1];

            if (previous_instruction->instrux.OperandsCount < 2 || prev_op2.Type != ND_OP_MEM || !prev_op2.Info.Memory.HasBase) {
                SPDLOG_ERROR("Previous instruction is not a memory dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (!prev_op2.Info.Memory.HasDisp) {
                SPDLOG_ERROR("Previous instruction does not have a displacement");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (prev_op2.Info.Memory.Disp != potential_hmd_device_offset) {
                SPDLOG_ERROR("Previous instruction is not the XRSystem or HMDDevice dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SPDLOG_INFO("Found the dereference of the XRSystem or HMDDevice at {:x}", previous_instruction->addr);

            SPDLOG_INFO("Creating first patch...");

            std::vector<int16_t> first_patch{};

            for (auto i = 0; i < decoded->Length; ++i) {
                first_patch.push_back(0x90);
            }

            // --- AFTER ---
            xrsystem_patches.push_back(Patch::create(exception_address, first_patch));

            const auto next_instruction_addr = exception_address + decoded->Length;
            const auto next_instruction = utility::decode_one((uint8_t*)next_instruction_addr);

            if (!next_instruction) {
                SPDLOG_ERROR("Could not decode next instruction at {:x}", exception_address + decoded->Length);
                exception->ContextRecord->Rip = next_instruction_addr;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (!std::string_view{next_instruction->Mnemonic}.starts_with("CALL")) {
                SPDLOG_ERROR("Next instruction is not a call, continuing anyways since we patched the dereference");
                exception->ContextRecord->Rip = next_instruction_addr;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Patch the next instruction if it's a call
            SPDLOG_INFO("Creating second patch...");

            std::vector<int16_t> second_patch(next_instruction->Length, 0x90);
            xrsystem_patches.push_back(Patch::create(next_instruction_addr, second_patch));

            exception->ContextRecord->Rip = next_instruction_addr + next_instruction->Length;

            SPDLOG_INFO("Finished creating patches, continuing execution.");
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    });

    // The TWeakPtr version is for >= 4.11 UE versions
    TWeakPtr<FSceneViewExtensions>& view_extensions_tweakptr = *(TWeakPtr<FSceneViewExtensions>*)potential_view_extensions;

    if (!m_rendertarget_manager_embedded_in_stereo_device) {
        if (view_extensions_tweakptr.reference == nullptr) {
            view_extensions_tweakptr.allocate_naive(m_use_fmalloc_scene_view_extensions->value());
        }

        // Check if allocation failed or returned null
        if (view_extensions_tweakptr.reference == nullptr) {
            SPDLOG_ERROR("Failed to allocate or resolve view_extensions reference!");
            return false;
        }
    }

    // Pointer approach with standard null safety checks
    FSceneViewExtensions* view_extensions_ptr = m_rendertarget_manager_embedded_in_stereo_device
                                                    ? (FSceneViewExtensions*)potential_view_extensions
                                                    : view_extensions_tweakptr.reference;

    if (view_extensions_ptr == nullptr) {
        SPDLOG_ERROR("view_extensions pointer is null!");
        return false;
    }

    // Reference wrapper for clean compatibility with the rest of the existing code structure
    FSceneViewExtensions& view_extensions = *view_extensions_ptr;
    g_engine_view_extensions = view_extensions_ptr;
    diag_scan_lgui_render_anchors();

    SPDLOG_INFO("Current ext ptr: {:x}", (uintptr_t)view_extensions.extensions.data);
    SPDLOG_INFO("Current ext count: {}", view_extensions.extensions.count);
    SPDLOG_INFO("Current ext capacity: {}", view_extensions.extensions.capacity);

    // Verifications on the current memory of the FSceneViewExtensions, because pre-4.10 (?) the view extensions array did not actually
    // exist
    if (m_rendertarget_manager_embedded_in_stereo_device) {
        SPDLOG_INFO("Performing verifications on the current memory of the FSceneViewExtensions...");

        const auto& current_view_extensions_ptr_value = view_extensions.extensions;

        // Check if current value is non zero and points to invalid memory
        if (current_view_extensions_ptr_value.data != nullptr &&
            IsBadReadPtr((void*)current_view_extensions_ptr_value.data, sizeof(void*))) {
            SPDLOG_ERROR("Usual view extensions pointer is non-zero but points to invalid memory! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        if ((uint32_t)current_view_extensions_ptr_value.count > (uint32_t)current_view_extensions_ptr_value.capacity) {
            SPDLOG_ERROR("Usual view extensions count is greater than capacity! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        if ((int32_t)current_view_extensions_ptr_value.count < 0 || (int32_t)current_view_extensions_ptr_value.capacity < 0) {
            SPDLOG_ERROR("Usual view extensions count or capacity is negative! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        const auto count_as_ptr = *(void**)&current_view_extensions_ptr_value.count;
        if (count_as_ptr != nullptr && !IsBadReadPtr(count_as_ptr, sizeof(void*))) {
            SPDLOG_ERROR("Usual view extensions count is actually a pointer to valid memory! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        if (current_view_extensions_ptr_value.data == nullptr && current_view_extensions_ptr_value.capacity > 0) {
            SPDLOG_INFO("Usual view extensions data pointer is null but capacity is greater than 0! Cannot set up view extensions!");
            SPDLOG_INFO("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
        }

        if (current_view_extensions_ptr_value.data != nullptr && current_view_extensions_ptr_value.capacity == 0) {
            SPDLOG_ERROR("Usual view extensions data pointer is non-null but capacity is 0! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        if (current_view_extensions_ptr_value.data != nullptr) {
            for (auto i = 0; i < current_view_extensions_ptr_value.count; ++i) {
                const auto ext = current_view_extensions_ptr_value.data[i].reference;

                if (IsBadReadPtr((void*)ext, sizeof(void*))) {
                    SPDLOG_ERROR("Usual view extensions array contains an invalid entry! Cannot set up view extensions!");
                    SPDLOG_ERROR(
                        "This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
                    return false;
                }

                const auto ext_vtable = *(void**)ext;

                if (IsBadReadPtr((void*)ext_vtable, sizeof(void*))) {
                    SPDLOG_ERROR("Usual view extensions array contains an entry with an invalid vtable! Cannot set up view extensions!");
                    SPDLOG_ERROR(
                        "This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
                    return false;
                }
            }
        }
    }

    // Allocate a completely new array if the current one is null or empty
    if (view_extensions.extensions.data == nullptr || view_extensions.extensions.data[0].reference == nullptr ||
        view_extensions.extensions.count == 0) {
        SPDLOG_INFO("Allocating new view extensions array...");

        auto& exts = view_extensions.extensions;

        // Allocate a bunch more than necessary to prevent crashes when the engine tries to add new entries
        const auto new_capacity = 32;

        if (!m_use_fmalloc_scene_view_extensions->value()) {
            exts.data = new TWeakPtr<ISceneViewExtension>[new_capacity] {};
        } else {
            if (auto fmalloc = sdk::FMalloc::get(); fmalloc != nullptr) {
                exts.data = (TWeakPtr<ISceneViewExtension>*)fmalloc->malloc(new_capacity * sizeof(TWeakPtr<ISceneViewExtension>));
                for (auto i = 0; i < new_capacity; ++i) {
                    new (&exts.data[i]) TWeakPtr<ISceneViewExtension>();
                }
            } else {
                SPDLOG_ERROR(
                    "Failed to get FMalloc! Cannot allocate new view extensions array! Falling back to default allocation method...");
                exts.data = new TWeakPtr<ISceneViewExtension>[new_capacity] {};
            }
        }

        exts.count = 0;
        exts.capacity = new_capacity;

        ZeroMemory(exts.data, sizeof(TWeakPtr<ISceneViewExtension>) * new_capacity);
        exts.data[exts.count++].allocate_naive(m_use_fmalloc_scene_view_extensions->value());
    } else if (view_extensions.extensions.data != nullptr && view_extensions.extensions.count <= view_extensions.extensions.capacity) {
        auto& exts = view_extensions.extensions;

        // TODO: Use FMemory::Realloc (or whatever its called) instead of new/delete cuz game crashes when reallocating/closing the game
        if (exts.count == exts.capacity) {
            SPDLOG_INFO("Extending view extensions array...");

            const auto new_capacity = exts.capacity * 4;
            const auto old_capacity = exts.capacity;

            TWeakPtr<ISceneViewExtension>* new_exts = nullptr;

            if (!m_use_fmalloc_scene_view_extensions->value()) {
                new_exts = new TWeakPtr<ISceneViewExtension>[new_capacity];
            } else {
                if (auto fmalloc = sdk::FMalloc::get(); fmalloc != nullptr) {
                    new_exts = (TWeakPtr<ISceneViewExtension>*)fmalloc->malloc(new_capacity * sizeof(TWeakPtr<ISceneViewExtension>));
                    for (auto i = 0; i < new_capacity; ++i) {
                        new (&new_exts[i]) TWeakPtr<ISceneViewExtension>();
                    }
                } else {
                    SPDLOG_ERROR(
                        "Failed to get FMalloc! Cannot allocate new view extensions array! Falling back to default allocation method...");
                    new_exts = new TWeakPtr<ISceneViewExtension>[new_capacity];
                }
            }

            ZeroMemory(new_exts, sizeof(TWeakPtr<ISceneViewExtension>) * new_capacity);
            memcpy(new_exts, exts.data, sizeof(TWeakPtr<ISceneViewExtension>) * old_capacity);

            exts.data = new_exts;
            exts.capacity = new_capacity;
        } else {
            SPDLOG_INFO("Allocating new view extension entry onto existing array...");
        }

        exts.data[exts.count++].allocate_naive(m_use_fmalloc_scene_view_extensions->value());
    } else {
        SPDLOG_INFO("None of the previous conditions were met, so we're not allocating a new view extensions array");
    }

    if (view_extensions.extensions.count > 0 && view_extensions.extensions.data != nullptr) {
        // Replace the vtable of the first entry
        auto& entry = view_extensions.extensions.data[view_extensions.extensions.count - 1];

        if (entry.reference == nullptr) {
            SPDLOG_ERROR("Failed to get first view extension entry!");
            return false;
        }

        auto& vtable = *(uintptr_t**)entry.reference;

        g_hook->m_analyze_view_extensions_start_time = std::chrono::high_resolution_clock::now();
        g_hook->m_analyzing_view_extensions = true;

        if (!m_rendertarget_manager_embedded_in_stereo_device) {
            SceneViewExtensionAnalyzer::FillVtable<g_view_extension_vtable.size() - 1>::fill(g_view_extension_vtable);
        } else {
            // Skip straight to stage 2.
            SPDLOG_INFO("Skipping view extension stage 1...");
            SceneViewExtensionAnalyzer::FillVtable<g_view_extension_vtable.size() - 1>::fill2(g_view_extension_vtable);
        }

        // Will get called when the view extensions are finally hooked.
        RenderThreadWorker::get().enqueue([this]() {
            this->m_analyzing_view_extensions = false;
            this->m_has_view_extensions_installed = true;
        });

        // overwrite the vtable
        vtable = g_view_extension_vtable.data();
        m_has_view_extension_hook = true;
    } else {
        // TODO: Allocate a new one.
        m_has_view_extension_hook = false;

        SPDLOG_INFO("Failed to set up view extensions! (not yet implemented to allocate a new one)");
    }

    return true;
} catch (...) {
    SPDLOG_ERROR("Unknown exception while setting up view extensions!");
    return false;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_fake_stereo_rendering_constructor() {
    static std::optional<uintptr_t> cached_result{};

    if (cached_result) {
        return cached_result;
    }

    const auto engine_dll = sdk::get_ue_module(L"Engine");

    auto fake_stereo_rendering_constructor = utility::find_function_from_string_ref(engine_dll, L"r.StereoEmulationHeight");

    if (!fake_stereo_rendering_constructor) {
        fake_stereo_rendering_constructor = utility::find_function_from_string_ref(engine_dll, L"r.StereoEmulationFOV");

        if (!fake_stereo_rendering_constructor) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering constructor");
            return std::nullopt;
        }
    }

    if (!fake_stereo_rendering_constructor) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering constructor");
        return std::nullopt;
    }

    SPDLOG_INFO("FFakeStereoRendering constructor: {:x}", (uintptr_t)*fake_stereo_rendering_constructor);
    cached_result = *fake_stereo_rendering_constructor;

    return *fake_stereo_rendering_constructor;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_fake_stereo_rendering_vtable() {
    static std::optional<uintptr_t> cached_result{};

    if (cached_result) {
        return cached_result;
    }

    if (g_hook->m_manually_constructed) {
        cached_result = *(uintptr_t*)((uintptr_t)sdk::UGameEngine::get() + s_stereo_rendering_device_offset);
        return cached_result;
    }

    const auto fake_stereo_rendering_constructor = locate_fake_stereo_rendering_constructor();

    if (!fake_stereo_rendering_constructor) {
        // If this happened, then that's bad news, the UE version is probably extremely old
        // so we have to use this fallback method.
        SPDLOG_INFO("Failed to locate FFakeStereoRendering constructor, using fallback method");
        const auto initialize_hmd_device = sdk::UEngine::get_initialize_hmd_device_address();

        if (!initialize_hmd_device) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method");
            return std::nullopt;
        }

        // To be seen if this needs to be adjusted. At first glance it doesn't look very reliable.
        // maybe perform emulation or something in the future?
        const auto instruction = utility::scan_disasm(*initialize_hmd_device, 100, "48 8D 05 ? ? ? ?");

        if (!instruction) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method (2)");
            return std::nullopt;
        }

        const auto result = utility::calculate_absolute(*instruction + 3);

        if (!result) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method (3)");
            return std::nullopt;
        }

        SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", (uintptr_t)result);
        cached_result = result;

        return result;
    }

    const auto vtable_ref = utility::scan(*fake_stereo_rendering_constructor, 100, "48 8D 05 ? ? ? ?");

    if (!vtable_ref) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable Reference");
        return std::nullopt;
    }

    const auto vtable = utility::calculate_absolute(*vtable_ref + 3);

    if (!vtable) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable");
        return std::nullopt;
    }

    SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", (uintptr_t)vtable);
    cached_result = vtable;

    return vtable;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_active_stereo_rendering_device() {
    auto engine = (uintptr_t)sdk::UEngine::get();

    if (engine == 0) {
        SPDLOG_ERROR("GEngine does not appear to be instantiated, cannot verify stereo rendering device is setup.");
        return std::nullopt;
    }

    SPDLOG_INFO("Checking engine pointers for StereoRenderingDevice...");
    auto fake_stereo_device_vtable = locate_fake_stereo_rendering_vtable();

    if (!fake_stereo_device_vtable) {
        SPDLOG_ERROR("Failed to locate fake stereo rendering device vtable, cannot verify stereo rendering device is setup.");
        return std::nullopt;
    }

    if (s_stereo_rendering_device_offset != 0) {
        const auto result = *(uintptr_t*)(engine + s_stereo_rendering_device_offset);

        if (result == 0) {
            return std::nullopt;
        }

        return result;
    }

    for (auto i = 0; i < 0x2000; i += sizeof(void*)) {
        const auto addr_of_ptr = engine + i;

        if (IsBadReadPtr((void*)addr_of_ptr, sizeof(void*))) {
            SPDLOG_INFO("Reached end of engine pointers at offset {:x}", i);
            break;
        }

        const auto ptr = *(uintptr_t*)addr_of_ptr;

        if (ptr == 0 || IsBadReadPtr((void*)ptr, sizeof(void*))) {
            continue;
        }

        auto potential_vtable = *(uintptr_t*)ptr;

        if (potential_vtable == *fake_stereo_device_vtable) {
            SPDLOG_INFO("Found fake stereo rendering device at offset {:x} -> {:x}", i, ptr);
            s_stereo_rendering_device_offset = i;
            return ptr;
        }
    }

    SPDLOG_ERROR("Failed to find stereo rendering device");
    return std::nullopt;
}

std::optional<uint32_t> FFakeStereoRenderingHook::get_stereo_view_offset_index(uintptr_t vtable) {
    for (auto i = 0; i < 30; ++i) {
        auto func = ((uintptr_t*)vtable)[i];

        if (func == 0 || IsBadReadPtr((void*)func, sizeof(void*))) {
            continue;
        }

        // Resolve jmps if needed.
        while (*(uint8_t*)func == 0xE9) {
            SPDLOG_INFO("VFunc at index {} contains a jmp, resolving...", i);
            func = utility::calculate_absolute(func + 1);
        }

        bool found = false;
        uint32_t xmm_register_usage_count = 0;

        // We do an exhaustive decode (disassemble all possible code paths) that correctly follows the control flow
        // because some games are obfuscated and do huge jumps across gaps of junk code.
        // so we can't just linearly scan forward as the disassembler will fail at some point.
        utility::exhaustive_decode((uint8_t*)func, 50, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (found) {
                return utility::ExhaustionResult::BREAK;
            }

            if (ix.BranchInfo.IsBranch && !ix.BranchInfo.IsConditional && std::string_view{ix.Mnemonic}.starts_with("CALL")) {
                return utility::ExhaustionResult::STEP_OVER;
            }

            char txt[ND_MIN_BUF_SIZE]{};
            NdToText(&ix, 0, sizeof(txt), txt);

            if (std::string_view{txt}.find("xmm") != std::string_view::npos && ++xmm_register_usage_count >= 10) {
                found = true;
                return utility::ExhaustionResult::BREAK;
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        if (found) {
            SPDLOG_INFO("Found Stereo View Offset Index: {}", i);
            return i;
        }
    }

    return std::nullopt;
}

// DISCLAIMER: I've only seen this in one game so far...
// So, there's some kind of compiler optimization for inlined virtuals
// that checks whether the vtable pointer matches the base FFakeStereoRendering class.
// if it matches, it just calls an inlined version of the function.
// otherwise it actually calls the function within the vtable.
bool FFakeStereoRenderingHook::patch_vtable_checks() {
    SPDLOG_INFO("Attempting to patch inlined vtable checks...");

    const auto fake_stereo_rendering_constructor = locate_fake_stereo_rendering_constructor();
    const auto fake_stereo_rendering_vtable = locate_fake_stereo_rendering_vtable();

    if (!fake_stereo_rendering_constructor || !fake_stereo_rendering_vtable) {
        SPDLOG_ERROR("Cannot patch vtables, constructor or vtable not found!");
        return false;
    }

    const auto vtable_module_within = utility::get_module_within(*fake_stereo_rendering_vtable);
    const auto module_size = utility::get_module_size(*vtable_module_within);
    const auto module_end = (uintptr_t)*vtable_module_within + *module_size;

    SPDLOG_INFO("{:x} {:x} {:x}", *fake_stereo_rendering_vtable, (uintptr_t)*vtable_module_within, *module_size);

    for (auto ref = utility::scan_displacement_reference(*vtable_module_within, *fake_stereo_rendering_vtable); 
        ref.has_value();
        ref = utility::scan_displacement_reference((uintptr_t)*ref + 4, (module_end - *ref) - sizeof(void*), *fake_stereo_rendering_vtable)) 
    {
        const auto distance_from_constructor = *ref - *fake_stereo_rendering_constructor;

        // We don't want to mess with the one within the constructor.
        if (distance_from_constructor < 0x100) {
            SPDLOG_INFO("Skipping vtable reference within constructor");
            continue;
        }

        // Change the bytes to be some random number
        // this causes the vtable check to fail and will call the function within the vtable.
        DWORD old{};
        VirtualProtect((void*)*ref, 4, PAGE_EXECUTE_READWRITE, &old);
        *(uint32_t*)*ref = 0x12345678;
        VirtualProtect((void*)*ref, 4, old, &old);
        SPDLOG_INFO("Patched vtable check at {:x}", (uintptr_t)*ref);
    }

    SPDLOG_INFO("Finished patching inlined vtable checks.");
    return true;
}

bool FFakeStereoRenderingHook::attempt_runtime_inject_stereo() {
    // This attempts to create a new StereoRenderingDevice in the GEngine
    // if it doesn't already exist via using -emulatestereo.
    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to locate GEngine, cannot inject stereo rendering device at runtime.");
        return false;
    }

    static auto enable_stereo_emulation_cvar = sdk::vr::get_enable_stereo_emulation_cvar();

    if (!locate_active_stereo_rendering_device()) {
        SPDLOG_INFO("Calling InitializeHMDDevice...");

        //utility::ThreadSuspender _{};

        engine->initialize_hmd_device();

        SPDLOG_INFO("Called InitializeHMDDevice.");

        if (!locate_active_stereo_rendering_device()) {
            SPDLOG_INFO("Previous call to InitializeHMDDevice did not setup the stereo rendering device, attempting to call again...");

            auto patch_emulate_stereo_flag = []() {
                //SPDLOG_ERROR("Failed to locate r.EnableStereoEmulation cvar, next call may fail.");
                SPDLOG_INFO("r.EnableStereoEmulation cvar not found, using fallback method of forcing -emulatestereo flag.");
                
                const auto emulate_stereo_string_ref = sdk::UGameEngine::get_emulatestereo_string_ref_address();

                if (emulate_stereo_string_ref) {
                    const auto resolved = utility::resolve_instruction(*emulate_stereo_string_ref);

                    if (resolved) {
                        // Scan forward for a call instruction, this call checks the command line for "emulatestereo".
                        const auto call = utility::scan_disasm(resolved->addr, 20, "E8 ? ? ? ?");

                        if (call) {
                            // Patch the instruction to mov al, 1
                            SPDLOG_INFO("Patching instruction at {:x} to mov al, 1", (uintptr_t)*call);
                            static auto patch = Patch::create(*call, { 0xB0, 0x01, 0x90, 0x90, 0x90 });
                        }
                    }
                }
            };

            // We don't call this before because the cvar will not be set up
            // until it's referenced once. after we set this we need to call the function again.
            if (enable_stereo_emulation_cvar) {
                try {
                    enable_stereo_emulation_cvar->set<int>(1);
                } catch(...) {
                    SPDLOG_ERROR("Access violation occurred when writing to r.EnableStereoEmulation, the address may be incorrect!");
                    patch_emulate_stereo_flag();
                }
            } else {
                //SPDLOG_ERROR("Failed to locate r.EnableStereoEmulation cvar, next call may fail.");
                patch_emulate_stereo_flag();
            }

            SPDLOG_INFO("Calling InitializeHMDDevice... AGAIN");

            engine->initialize_hmd_device();

            SPDLOG_INFO("Called InitializeHMDDevice again.");
        }

        if (locate_active_stereo_rendering_device()) {
            SPDLOG_INFO("Stereo rendering device setup successfully.");
        } else {
            SPDLOG_ERROR("Failed to setup stereo rendering device.");
            return false;
        }
    } else {
        SPDLOG_INFO("Not necessary to call InitializeHMDDevice, stereo rendering device is already setup.");
        m_fixed_localplayer_view_count = true; // Everything was set up beforehand, we don't need to do anything, so just set it to true.
    }

    return true;
}

bool FFakeStereoRenderingHook::is_stereo_enabled(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("is stereo enabled called!");
#else
    SPDLOG_INFO_ONCE("is stereo enabled called!");
#endif

    // wait!!!
    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (g_hook->m_sceneview_data.inside_post_init_properties) {
        g_hook->set_should_recreate_textures(true);
        return true;
    }
    
    /*if (g_hook->m_analyzing_view_extensions) {
        const auto now = std::chrono::high_resolution_clock::now();

        if (now - g_hook->m_analyze_view_extensions_start_time > std::chrono::seconds(15)) {
            SPDLOG_INFO("Timed out waiting for view extensions to be analyzed.");
            g_hook->m_analyzing_view_extensions = false;
        }

        return false;
    }*/

    static std::atomic<bool> last_state = false;
    auto hook = g_hook;

    // The best way to enable stereo rendering without causing crashes
    // while also allowing the desktop view to initially display
    // if the HMD is not on at the start. It only allows
    // stereo to be enabled if it starts from the first call to IsStereoEnabled inside UGameViewportClient::Draw.
    if (hook->m_has_game_viewport_client_draw_hook) {
        if (GameThreadWorker::get().is_same_thread()) {
            if (hook->m_in_viewport_client_draw && !hook->m_was_in_viewport_client_draw) {
                const auto is_hmd_active = VR::get()->is_hmd_active();

                if (!last_state && is_hmd_active) {
                    VR::get()->wait_for_present();
                    hook->set_should_recreate_textures(true);
                }

                last_state = is_hmd_active;
            }

            hook->m_was_in_viewport_client_draw = hook->m_in_viewport_client_draw;
        }

        return last_state;
    }

    static uint32_t count = 0;

    // Forcefully return true the first few times to let stuff initialize.
    if (count < 50) {
        if (count == 0) {
            hook->set_should_recreate_textures(true);
        }

        ++count;
        last_state = true;
        return true;
    }

    const auto result = !VR::get()->get_runtime()->got_first_sync || VR::get()->is_hmd_active();

    if (result && !last_state) {
        hook->set_should_recreate_textures(true);
    }

    last_state = result;

    return result;
}

void FFakeStereoRenderingHook::adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("adjust view rect called! {}", index);
    SPDLOG_INFO(" x: {}, y: {}, w: {}, h: {}", *x, *y, *w, *h);
#else
    SPDLOG_INFO_ONCE("adjust view rect called! {}", index);
    SPDLOG_INFO_ONCE(" x: {}, y: {}, w: {}, h: {}", *x, *y, *w, *h);
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    static bool index_starts_from_one = true;

    if (index == 2) {
        index_starts_from_one = true;
    } else if (index == 0) {
        index_starts_from_one = false;
    }

    // The purpose of this is to prevent the game from crashing in IDirect3D12CommandList::Close
    // Because the game will try to copy a texture region that is out of bounds.
    if (g_hook->m_skip_next_adjust_view_rect) {
        *x = 0;
        *y = 0;
        *w = std::min<uint32_t>(VR::get()->get_hmd_width(), *w);
        *h = std::min<uint32_t>(VR::get()->get_hmd_height(), *h);
        g_hook->m_skip_next_adjust_view_rect = false;
        g_hook->m_skip_next_adjust_view_rect_count = 1;
        return;
    }

    if (g_hook->m_skip_next_adjust_view_rect_count > 0) {
        *x = 0;
        *y = 0;
        *w = std::min<uint32_t>(VR::get()->get_hmd_width(), *w);
        *h = std::min<uint32_t>(VR::get()->get_hmd_height(), *h);
        --g_hook->m_skip_next_adjust_view_rect_count;
        return;
    }

    if (VR::get()->is_stereo_emulation_enabled()) {
        *w *= 2;
    } else {
        *w = VR::get()->get_hmd_width() * 2;
        *h = VR::get()->get_hmd_height();
    }


    *w = *w / 2;

    auto true_index = index_starts_from_one ? ((index + 1) % 2) : (index % 2);

    // NOTE: In AFR mode this game calls AdjustViewRect with the SAME raw `index` value (e.g.
    // always 2) for both eye passes, since AFR reuses the same view slot across frames instead of
    // alternating the index itself. Without this override, true_index would resolve to the same
    // eye every single call, placing BOTH eyes' geometry into the same half of the backbuffer and
    // leaving the other half permanently black. Mirror the same call-scoped alternator used in
    // calculate_stereo_view_offset so this function's eye classification stays in sync with it.
    if (VR::get()->is_using_afr()) {
        if (VR::get()->is_unified_frame_parity_enabled()) {
            // DIAG: derive true_index from the same frame-parity source the compositor uses
            // (m_render_frame_count % 2 == m_left_eye_interval) instead of this function's own
            // independent call-scoped avr_call_index/g_frame_count alternator, to test whether the
            // two trackers falling out of phase with each other is contributing to the input/UI
            // desync seen with Native Stereo Fix disabled under AFR.
            true_index = VR::get()->get_unified_true_index();
        } else {
            static uint32_t last_avr_frame_count = 0;
            static uint32_t avr_call_index = 0;

            if (last_avr_frame_count != g_frame_count || avr_call_index > 1) {
                avr_call_index = 0;
            }

            last_avr_frame_count = g_frame_count;

            true_index = (g_frame_count + avr_call_index) % 2;
            ++avr_call_index;
        }
    }

    if (!VR::get()->is_native_stereo_fix_enabled()) {
        *x += *w * true_index;
    }

    // Record the actual x-offset the engine assigned to this eye (true_index 0 == left, 1 == right),
    // so the compositor (D3D12Component::composite_afr_eye / D3D11Component equivalent) can crop from
    // the real backbuffer half instead of assuming a fixed left=0/right=half layout. See
    // get_last_left_eye_x_offset()/get_last_right_eye_x_offset() for why this is necessary: the
    // compositor's own left/right classification is derived from a completely separate frame-parity
    // counter (vr->m_render_frame_count) that is not guaranteed to stay in phase with true_index here.
    if (true_index == 0) {
        g_hook->m_last_left_eye_x_offset = (uint32_t)*x;
        g_hook->m_left_eye_x_offset_update_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_hook->m_last_right_eye_x_offset = (uint32_t)*x;
        g_hook->m_right_eye_x_offset_update_count.fetch_add(1, std::memory_order_relaxed);
    }
    g_hook->m_has_seen_eye_x_offsets = true;

    // DIAG: throttled visibility into the final per-eye viewport rect produced by AdjustViewRect.
    // If one eye's rect is ever zero-sized (w/h == 0) or has an out-of-range x/y offset, the engine
    // simply never renders any scene geometry into that eye's portion of the backbuffer, which would
    // explain a permanently-black eye even though our AFR copy path is otherwise working correctly.
    //
    // IMPORTANT: this diagnostic's true_index is derived purely from AdjustViewRect's own local
    // index/index_starts_from_one state, which is INDEPENDENT of D3D12Component.cpp's is_left_eye_frame
    // classification (which is instead based on vr->m_render_frame_count % 2 == m_left_eye_interval).
    // If these two independently-computed "which eye is this" trackers ever fall out of phase with
    // each other, AdjustViewRect could be placing real content at an x-offset that D3D12Component then
    // samples/copies as the WRONG eye, producing a permanently-black eye despite the engine rendering
    // both eyes correctly. Log vr's frame-count/interval state here too so the two can be correlated
    // directly in the log. Also fixed the previous "% 300 == 1" throttle: since this function and
    // D3D12Component::on_frame's diag_afr_count both increment roughly once per real per-eye call, a
    // fixed EVEN stride can alias onto a single parity and make it look like only one eye is ever
    // logged after warm-up, when in fact both are still occurring - use an ODD stride instead so both
    // parities are sampled during steady-state logging.
    {
        static uint32_t diag_adjust_view_rect_count = 0;
        ++diag_adjust_view_rect_count;

        auto vr = VR::get();

        if (LGUI_DIAG_STEADY_STATE && (diag_adjust_view_rect_count <= 20 || diag_adjust_view_rect_count % 301 == 1)) {
            SPDLOG_INFO("[DIAG] AdjustViewRect (#{}): index={} true_index={} index_starts_from_one={} x={} y={} w={} h={} native_stereo_fix={} vr_frame_count={} vr_left_interval={} vr_right_interval={} is_using_afr={}",
                diag_adjust_view_rect_count, index, true_index, index_starts_from_one, *x, *y, *w, *h,
                vr->is_native_stereo_fix_enabled(), vr->m_render_frame_count, vr->m_left_eye_interval, vr->m_right_eye_interval, vr->is_using_afr());
        }
    }
}

__forceinline void FFakeStereoRenderingHook::calculate_stereo_view_offset(
    FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, 
    const float world_to_meters, Vector3f* view_location)
{
    // DIAG: record the raw view_index for this frame/thread so sceneview_constructor() (called by
    // the engine immediately afterwards for the SAME view) can attribute its ground-truth FSceneView
    // metadata back to this exact index. See t_last_calc_stereo_view_index declaration for rationale.
    t_last_calc_stereo_view_index = view_index;
    t_last_calc_stereo_view_index_frame = g_frame_count;

    // DIAG: NSF-CALLER-SITE. Captured at entry, before any trampoline/inline logic runs, so this is
    // the exact return address of whatever engine code called into this (hooked) function - i.e. the
    // real UE caller for THIS specific view_index. Logged (throttled per index) as module name + RVA
    // offset from that module's base so it can be resolved (dumpbin /disasm, IDA/Ghidra, or symbols if
    // available) to identify exactly which UE subsystem issues each raw view_index - e.g. distinguish
    // a genuine second eye render from a shadow-depth/HZB-occlusion/Nanite-visibility/scene-capture
    // pass, rather than inferring it indirectly from timing/alias behavior alone.
    if (VR::get() != nullptr && VR::get()->is_diag_log_raw_view_index_enabled()) {
        static uintptr_t s_last_logged_caller[9] = {};
        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto clamped_index_for_caller = std::clamp(view_index, 0, 8);

        if (s_last_logged_caller[clamped_index_for_caller] != return_address) {
            s_last_logged_caller[clamped_index_for_caller] = return_address;

            const auto module_within = utility::get_module_within(return_address);

            if (module_within) {
                const auto module_path = utility::get_module_path(*module_within);
                const auto rva = return_address - (uintptr_t)*module_within;
                const auto module_name_narrow = module_path ? *module_path : std::string{"<unknown>"};

                SPDLOG_WARN("[VR][NSF-CALLER-SITE] view_index={} caller_module={} caller_rva=0x{:x}",
                    view_index, module_name_narrow, rva);
            } else {
                SPDLOG_WARN("[VR][NSF-CALLER-SITE] view_index={} caller_addr=0x{:x} (module lookup failed)",
                    view_index, return_address);
            }
        }
    }

    // DIAG: NSF-STACKWALK-INDEX1. view_index has been shown (via NSF-VIEWINDEX-IDENTITY) to be
    // POLYMORPHIC: the exact same caller RVA has, at different points in the same session, produced
    // both a tiled 64x64 non-stereo capture (UI/icon-like) AND a burst of 900+ calls in a single
    // frame with no matching FSceneView construction at all (consistent with a shadow-cascade/light-
    // view transform query). NSF-CALLER-SITE alone can't distinguish these, since it only captures
    // the immediate return address, which both behaviors apparently share. Walk further up the stack
    // (skipping this function and its immediate caller) to capture a short chain of return addresses,
    // and bucket the sample by how many times view_index==1 has already fired THIS frame, so we get
    // one sample from a low-count ("UI capture"-like) frame and one from a high-count ("shadow-view"-
    // like, 900+/frame) frame per session, without spamming every single call.
    if (view_index == 1 && VR::get() != nullptr && VR::get()->is_diag_log_raw_view_index_enabled()) {
        static uint32_t s_index1_calls_this_frame = 0;
        static uint32_t s_index1_last_frame = 0xFFFFFFFFu;
        static bool s_logged_low_count_sample = false;
        static bool s_logged_high_count_sample = false;

        if (g_frame_count != s_index1_last_frame) {
            s_index1_last_frame = g_frame_count;
            s_index1_calls_this_frame = 0;
        }

        ++s_index1_calls_this_frame;

        // "Low count" sample: first time we see view_index==1 fire only a handful of times in a
        // frame (matches the tiled-64x64-capture behavior observed earlier in sessions).
        const bool want_low_sample = !s_logged_low_count_sample && s_index1_calls_this_frame >= 2 && s_index1_calls_this_frame <= 5;
        // "High count" sample: fires far more than a real per-eye call ever should in one frame
        // (matches the 900+/frame no-FSceneView behavior observed later in the same session).
        const bool want_high_sample = !s_logged_high_count_sample && s_index1_calls_this_frame >= 100;

        if (want_low_sample || want_high_sample) {
            constexpr auto max_stack_depth = 16;
            uintptr_t stack[max_stack_depth]{};
            const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

            std::string chain;
            for (auto i = 0; i < depth; ++i) {
                const auto frame_addr = stack[i];
                const auto module_within = utility::get_module_within(frame_addr);

                if (module_within) {
                    const auto rva = frame_addr - (uintptr_t)*module_within;
                    chain += fmt::format("0x{:x} ", rva);
                } else {
                    chain += fmt::format("?0x{:x} ", frame_addr);
                }
            }

            SPDLOG_WARN("[VR][NSF-STACKWALK-INDEX1] sample_kind={} frame={} calls_this_frame={} stack_rvas=[ {}]",
                want_low_sample ? "LOW_COUNT" : "HIGH_COUNT", g_frame_count, s_index1_calls_this_frame, chain);

            if (want_low_sample) {
                s_logged_low_count_sample = true;
            }

            if (want_high_sample) {
                s_logged_high_count_sample = true;
            }
        }
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate stereo view offset called! {}", view_index);
#else
    SPDLOG_INFO_ONCE("calculate stereo view offset called! {}", view_index);
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto vr = VR::get();

    if (vr == nullptr) {
        SPDLOG_INFO_ONCE("calculate stereo view offset called but VR is not initialized, ignoring.");
        return;
    }

    //std::scoped_lock _{vr->get_vr_mutex()};

    static bool index_starts_from_one = true;
    static bool index_was_ever_two = false;
    static bool index_was_ever_negative = false;

    if (view_index == -1) {
        index_was_ever_negative = true;
        SPDLOG_INFO_ONCE("calculate stereo view offset called with view index -1 (INDEX_NONE), ignoring.");
        return;
    }

    // DIAG: see m_diag_suppress_extra_view declaration for full rationale. Narrow, view_index-exact
    // early-return so only the specific stray extra call (proven via NSF-FINAL-EYE-POSE/GLITCH-EYE-DIAG
    // logs to sit at a different, slowly-converging position under the same true_index as a real eye)
    // is suppressed, before any pose caching/state below can be polluted by it. Off by default.
    if (vr->is_diag_suppress_extra_view_enabled() && view_index == vr->get_diag_suppress_view_index()) {
        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR][DIAG-SUPPRESS-VIEW] Suppressing calculate_stereo_view_offset call for view_index={} g_frame_count={}",
            view_index, g_frame_count);
        return;
    }

    // This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.
    if (index_was_ever_two && view_index == 0) {
        SPDLOG_INFO_ONCE("calculate stereo view offset called with view index 0 after 2, ignoring.");
        return;
    }

    vr->set_world_to_meters(world_to_meters);

    if (view_index == 2) {
        index_starts_from_one = true;
        index_was_ever_two = true;
    } else if (view_index == 0 && !index_was_ever_two) {
        index_starts_from_one = false;
    }

    const auto is_full_pass = view_index == 0 && !index_was_ever_two && !index_was_ever_negative;

    auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);
    const auto has_double_precision = g_hook->m_has_double_precision;
    const auto rot_d = (Rotator<double>*)view_rotation;

    // DIAG: world_to_meters is supplied per-call (effectively per-eye) but immediately clobbers a single
    // shared vr->m_world_to_meters value that is then read back later (as world_scale) for whichever eye
    // renders next. If the engine ever supplies a DIFFERENT world_to_meters for the left-eye call vs the
    // right-eye call within the same render frame - which can transiently happen while a camera/animation
    // blend is driving WorldToMeters on the active camera component - then one eye's head/eye-offset math
    // ends up scaled differently than the other eye's for that frame, which would look exactly like a
    // brief per-eye scale mismatch (image "splits" then re-converges) without being an NSF/AFR issue at all,
    // since this code path runs unconditionally for every eye in every stereo mode.
    //
    // FIXED: this previously bucketed calls via `(view_index == 2) ? 1 : (view_index % 2)`, which does
    // NOT match true_index's real aliasing behavior (view_index=1 and view_index=3 both alias to
    // true_index=0 once index_starts_from_one flips true - see NSF-TRUE-INDEX-ALIAS). That meant
    // view_index 1/2/3 were all silently bucketed into slot "1" and slot "0" was never populated, so
    // this check could never actually detect a real per-eye mismatch for this game. Now keyed off the
    // same true_index used everywhere else, and logs every value (not just mismatches) so we can also
    // directly compare aliased calls' world_to_meters against each other, not just eye0 vs eye1.
    if (vr->is_diag_log_world_to_meters_enabled()) {
        static thread_local uint32_t s_wtm_diag_frame_count = 0;
        static thread_local float s_wtm_diag_last_value[2] = {0.0f, 0.0f};
        static thread_local bool s_wtm_diag_has_value[2] = {false, false};

        if ((uint32_t)g_frame_count != s_wtm_diag_frame_count) {
            s_wtm_diag_frame_count = (uint32_t)g_frame_count;
            s_wtm_diag_has_value[0] = false;
            s_wtm_diag_has_value[1] = false;
        }

        const auto other_index = true_index == 0 ? 1 : 0;

        if (s_wtm_diag_has_value[other_index] && s_wtm_diag_last_value[other_index] != world_to_meters) {
            SPDLOG_WARN("[VR][NSF-WTM-MISMATCH] frame={} true_index={} view_index={} world_to_meters={:.4f} DIFFERS from true_index={} world_to_meters={:.4f}",
                g_frame_count, true_index, view_index, world_to_meters, other_index, s_wtm_diag_last_value[other_index]);
        }

        SPDLOG_WARN("[VR][NSF-WTM-VALUE] frame={} true_index={} view_index={} world_to_meters={:.4f}",
            g_frame_count, true_index, view_index, world_to_meters);

        s_wtm_diag_last_value[true_index] = world_to_meters;
        s_wtm_diag_has_value[true_index] = true;
    }


    // DIAG: NSF-TRUE-INDEX-ALIAS. true_index is derived purely from view_index's PARITY (see above),
    // so two DIFFERENT view_index values landing on the same parity (e.g. view_index=1 and
    // view_index=3 both -> true_index=0 once index_starts_from_one is true) silently alias onto the
    // SAME true_index. Since true_index is what the sync-pose cache, eye-offset math, and every other
    // per-eye branch in this function key off of, an aliased "extra" call doesn't just add a harmless
    // third render - it can overwrite/compete with the real eye's cached pose and offset state for
    // that frame, which is a much better explanation for the reported doubled-image/eye desync than a
    // simple ignorable stray view. Track the most recent view_index seen for each true_index and warn
    // loudly (not throttled - this is meant to be enabled briefly during a repro) the moment a
    // DIFFERENT view_index reuses a true_index within the same g_frame_count, which pinpoints exactly
    // when/how often the aliasing itself occurs, independent of whatever raw index numbers this
    // session happens to be using.
    if (vr->is_diag_log_true_index_alias_enabled()) {
        static int32_t s_last_view_index_for_true_index[2] = { -999, -999 };
        static uint64_t s_last_frame_for_true_index[2] = { (uint64_t)-1, (uint64_t)-1 };

        if (s_last_frame_for_true_index[true_index] == (uint64_t)g_frame_count &&
            s_last_view_index_for_true_index[true_index] != -999 &&
            s_last_view_index_for_true_index[true_index] != view_index) {
            SPDLOG_WARN("[VR][NSF-TRUE-INDEX-ALIAS] frame={} true_index={} view_index={} ALIASES with prior view_index={} in the SAME frame (index_starts_from_one={} index_was_ever_two={})",
                g_frame_count, true_index, view_index, s_last_view_index_for_true_index[true_index],
                index_starts_from_one, index_was_ever_two);
        }

        s_last_view_index_for_true_index[true_index] = view_index;
        s_last_frame_for_true_index[true_index] = (uint64_t)g_frame_count;
    }

    // DIAG: NSF-RAW-VIEW-INDEX. Unconditional record of EVERY raw view_index this hook is ever
    // called with (not just 0/1, and not just aliasing collisions), so we can answer directly which
    // raw indices actually occur in a session, how many times each occurs, and in what order relative
    // to true_index/is_full_pass - instead of inferring it indirectly from other diagnostics. Indexed
    // by view_index directly (bounded/clamped) so this scales to whatever range this build's engine
    // build actually uses (observed 0-4 previously).
    if (vr->is_diag_log_raw_view_index_enabled()) {
        constexpr int32_t kMaxTrackedViewIndex = 8;
        static uint64_t s_raw_view_index_call_count[kMaxTrackedViewIndex + 1] = {};
        static uint64_t s_raw_view_index_last_frame[kMaxTrackedViewIndex + 1] = {};

        const auto clamped_index = std::clamp(view_index, 0, kMaxTrackedViewIndex);
        s_raw_view_index_call_count[clamped_index]++;

        SPDLOG_WARN("[VR][NSF-RAW-VIEW-INDEX] frame={} view_index={} true_index={} is_full_pass={} is_using_afr={} index_starts_from_one={} index_was_ever_two={} call_count_for_this_index={} frames_since_last_seen={}",
            g_frame_count, view_index, true_index, is_full_pass, vr->is_using_afr(), index_starts_from_one, index_was_ever_two,
            s_raw_view_index_call_count[clamped_index],
            s_raw_view_index_last_frame[clamped_index] == 0 ? 0 : (uint64_t)g_frame_count - s_raw_view_index_last_frame[clamped_index]);

        s_raw_view_index_last_frame[clamped_index] = (uint64_t)g_frame_count;
    }


    if (vr->is_using_afr() && !is_full_pass) {
        // NOTE: We used to just do `true_index = g_frame_count % 2;` here, but g_frame_count
        // reflects the runtime's internal frame counter, which can stall (e.g. during a
        // dropped/duplicated present) for multiple consecutive calls into this function. When
        // that happens, every call during the stall resolves to the SAME eye regardless of which
        // eye the engine actually asked for via view_index, which starves the other eye of any
        // rendered content (permanently black eye). Instead, fold in a call-scoped alternator
        // (mirroring the same pattern used in sceneview_constructor) that increments on every
        // call within an unchanged g_frame_count, so consecutive calls still alternate eyes even
        // if the underlying frame counter hasn't advanced.
        if (vr->is_unified_frame_parity_enabled()) {
            // DIAG: derive true_index from the same frame-parity source the compositor uses
            // (m_render_frame_count % 2 == m_left_eye_interval), see AdjustViewRect for rationale.
            true_index = vr->get_unified_true_index();
        } else {
            static uint32_t last_offset_frame_count = 0;
            static uint32_t offset_call_index = 0;

            if (last_offset_frame_count != g_frame_count || offset_call_index > 1) {
                offset_call_index = 0;
            }

            last_offset_frame_count = g_frame_count;

            true_index = (g_frame_count + offset_call_index) % 2;
            ++offset_call_index;
        }

        if (!vr->is_using_synchronized_afr()) {
            if (g_hook->m_has_double_precision) {
                if (true_index == 1) {
                    *rot_d = g_hook->m_last_afr_rotation_double;
                } else {
                    g_hook->m_last_afr_rotation_double = *rot_d;
                }
            } else {
                if (true_index == 1) {
                    *view_rotation = g_hook->m_last_afr_rotation;
                } else {
                    g_hook->m_last_afr_rotation = *view_rotation;
                }
            }
        }
    }

    // NSF (non-AFR Native Stereo Fix) eye-desync mitigation: unlike AFR above, NSF renders both eyes
    // within the SAME engine frame (two calls into this function per frame, Pass1=left then Pass2=
    // right, see begin_render_viewfamily_real's wants_swap path), and had no cross-eye pose caching at
    // all. During a camera-transition animation (cutscene blend, dash, ability camera shift) the live
    // animated camera pose can change between these two calls, so the two eyes get built from two
    // different poses for what should be one synchronized stereo frame - producing a momentary
    // left/right desync that resolves once the camera holds still. Mirror the AFR rotation-cache
    // pattern here: cache Pass1 (left)'s rotation/location for this frame, then force Pass2 (right) to
    // reuse it. Gated to NSF (native_stereo_fix_enabled && !is_using_afr) and behind its own toggle so
    // it never affects AFR (which already has its own mechanism above) or normal head tracking on
    // frames where NSF isn't swapping passes.
    //
    // REVERTED: an earlier attempt restricted this cache to a "main stereo pass" view_index range
    // derived from index_starts_from_one, based on a hypothesis that an extra secondary view (e.g. a
    // VFX/portal capture) was polluting the cache. In-headset testing DISPROVED this: blocking that
    // "extra" view instead caused it to stick rigidly to the HMD with no eye offset applied at all -
    // proving it is actually a REAL eye call, not a secondary view, and the view_index classification
    // above was wrong for this game. That restriction also caused a permanent low-level doubled-image
    // regression by extension in the projection-matrix path. Both restrictions are reverted here; the
    // cache is unconditional again (gated only on NSF/enabled/toggle) matching original behavior.
    //
    // FIX: user confirmed via the DiagSuppressExtraView test (fully early-returning view_index=1,
    // BEFORE any pose caching/rendering) that view_index=1 does NOT correspond to a visible rendered
    // eye this session - no stuck/frozen/blank eye resulted, unlike an earlier session where blocking
    // an assumed "extra view" DID stick to the HMD (that was almost certainly a different, since-
    // invalidated raw index guess, from before 2=left/3=right was established). view_index=1 fires at
    // a CONSTANT ~4x rate relative to 2/3 regardless of glitch timing (see NSF-RAW-VIEW-INDEX), and it
    // aliases onto true_index=0 alongside the real view_index=3 eye call (NSF-TRUE-INDEX-ALIAS) - a
    // race where whichever of the two writes last wins, which plausibly explains why the reported
    // glitch has switched eyes (L/R) across sessions. Exclude view_index=1 specifically from the
    // sync-pose cache (NOT a full render skip) so it can never again overwrite/race with the real
    // eye's cached pose, while leaving actual rendering of it untouched in case some other subsystem
    // depends on it. Toggle-gated so it can be instantly reverted if further testing disagrees.
    const bool diag_exclude_from_sync_cache = vr->is_diag_exclude_view_index_from_sync_cache_enabled() &&
        view_index == vr->get_diag_exclude_view_index_from_sync_cache();

    if (!diag_exclude_from_sync_cache && vr->is_native_stereo_fix_enabled() && !vr->is_using_afr() && vr->is_native_stereo_fix_sync_pose_enabled() && !is_full_pass) {
        auto& hook_data = *g_hook;
        const auto local_view_d = (Vector3d*)view_location;

        if (true_index == 0) {
            // Pass1 (left eye): cache this frame's pose for Pass2 to reuse below.
            if (has_double_precision) {
                hook_data.m_nsf_sync_pose_rotation_double = *rot_d;
                hook_data.m_nsf_sync_pose_location_double = *local_view_d;
            } else {
                hook_data.m_nsf_sync_pose_rotation = *view_rotation;
                hook_data.m_nsf_sync_pose_location = *view_location;
            }

            hook_data.m_nsf_sync_pose_frame_count = g_frame_count;
            hook_data.m_nsf_sync_pose_have_left = true;

            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] NSF sync-pose: cached Pass1 (left) pose for frame {}", g_frame_count);
        } else if (true_index == 1 && !hook_data.m_nsf_sync_pose_have_left) {
            // DIAG: Pass2 (right eye) reached, but no Pass1 pose was cached this pass (have_left is
            // false) - meaning sync is NOT applied for this Pass2 call. Previously this branch also
            // required g_frame_count to match the cached Pass1 frame, but g_frame_count is sourced from
            // the HMD runtime's internal_frame_count (see line ~2598), which increments once PER EYE
            // SUBMISSION - so it always advances by ~2 between the Pass1 and Pass2 calls within the SAME
            // engine render frame. That made the old frame_count equality check permanently false, so
            // sync-pose silently never applied. Fixed below to rely on have_left alone.
            SPDLOG_WARNING_EVERY_N_SEC(1, "[VR][NSF-POSE-DIVERGE-SKIP] Pass2 frame={} has no cached Pass1 pose (have_left=false), sync NOT applied",
                g_frame_count);
        } else if (hook_data.m_nsf_sync_pose_have_left) {
            // Pass2 (right eye): Pass1 and Pass2 run back-to-back synchronously within the same
            // begin_render_viewfamily_real call (see the immediate std::swap(views[0], views[1]) and
            // re-invocation of the render module there), so no frame-count match is needed - have_left
            // alone is sufficient. Consume and clear it immediately so a stale Pass1 pose can never be
            // reused by some later, unrelated Pass2-only call.
            hook_data.m_nsf_sync_pose_have_left = false;

            // Force ROTATION to match to prevent orientation-based eye desync during animation/camera-
            // transition frames. Do NOT touch view_location/local_view_d here - that holds the per-eye
            // stereo position offset (IPD), which legitimately differs between the left and right eye.
            // Overwriting it with Pass1's location collapses the right eye onto the left eye's position,
            // eliminating positional parallax/depth between the two eyes for that frame. This was
            // previously done unconditionally and with no delta logging, which produced a near-zero-
            // disparity "flat"/double-vision-like frame that was misattributed to view_rect/composite
            // scaling instead.
            if (has_double_precision) {
                const auto rot_delta = glm::length(glm::dvec3{
                    rot_d->yaw - hook_data.m_nsf_sync_pose_rotation_double.yaw,
                    rot_d->pitch - hook_data.m_nsf_sync_pose_rotation_double.pitch,
                    rot_d->roll - hook_data.m_nsf_sync_pose_rotation_double.roll});

                // DIAG: NSF-POSE-DIVERGE. We only force ROTATION here; POSITION is intentionally left
                // alone (see comment above) because it legitimately carries the per-eye IPD offset.
                // But if the underlying camera/actor location itself moves a large amount between the
                // Pass1 (left) call and the Pass2 (right) call within the SAME engine frame - which can
                // happen during a fast skill/ability/cutscene camera translation, as opposed to a pure
                // rotation - then Pass2 still renders from a different WORLD position than Pass1, which
                // would look exactly like a residual left/right positional desync ("double vision")
                // that the rotation-only sync cannot fix. Log the raw position delta (not applied) so a
                // captured log can prove/disprove whether large POSITION deltas (not just rotation
                // deltas) are occurring during the reported glitch window.
                const auto pos_delta = glm::length(glm::dvec3{*local_view_d} - glm::dvec3{hook_data.m_nsf_sync_pose_location_double});

                // DIAG: unconditional (throttled) trace to prove this branch is actually reached and
                // to see the real delta magnitudes even when they stay under the warn threshold below.
                // Gated behind is_diag_sync_pose_verbose_logging_enabled() - fires every Pass2 call and
                // is only meant to be enabled briefly while reproducing/tuning, not left on constantly.
                if (vr->is_diag_sync_pose_verbose_logging_enabled()) {
                    SPDLOG_INFO_EVERY_N_SEC(1, "[VR][NSF-POSE-DIVERGE-TRACE] Pass2 reached, frame={} rot_delta_deg={:.4f} pos_delta={:.4f}",
                        g_frame_count, rot_delta, pos_delta);

                    if (rot_delta > 0.001 || pos_delta > 0.01) {
                        SPDLOG_WARN("[VR][NSF-POSE-DIVERGE] frame={} rot_delta_deg={:.4f} pos_delta={:.4f} pass1_pos=({:.2f},{:.2f},{:.2f}) pass2_pos=({:.2f},{:.2f},{:.2f})",
                            g_frame_count, rot_delta, pos_delta,
                            hook_data.m_nsf_sync_pose_location_double.x, hook_data.m_nsf_sync_pose_location_double.y, hook_data.m_nsf_sync_pose_location_double.z,
                            local_view_d->x, local_view_d->y, local_view_d->z);
                    }
                }

                // BLEND (not a hard snap): alpha==1.0 fully overwrites Pass2's rotation with Pass1's
                // (old behavior - eliminates desync but makes the right eye feel completely frozen/
                // lagging relative to the live animated camera for that frame, which reads as
                // disorienting one-sided lag). A lower alpha lets Pass2 keep some of its own live
                // rotation, splitting the residual error across both eyes instead of concentrating the
                // full correction (and the perceptual "lag") onto the right eye alone.
                //
                // HARD-CUT OVERRIDE: mirrors the approach used by the existing Lua UI-fix script for
                // cutscene camera-actor changes (ResetCutSceneCamOffset), which forces an INSTANT/full
                // reset when the cutscene camera actor's view changes abruptly - treating a genuine
                // camera CUT differently from continuous small drift. Blending across a real cut would
                // render a visibly wrong intermediate pose in one eye for that frame (a brief
                // double-image at the cut itself), which is worse than a one-frame freeze; only small,
                // continuous per-frame divergence (VFX/skill camera motion) benefits from a partial
                // blend. Unlike the Lua script (which only evaluates this while the view target is an
                // actual CineCameraActor, i.e. never during normal player input), this runs every frame
                // regardless of what's driving the camera, so a flat magnitude threshold alone was too
                // sensitive to fast thumbstick turns. is_nsf_sync_pose_hard_cut() instead requires an
                // abrupt SPIKE relative to the recent rolling baseline AND a raised absolute floor.
                // DIAG: post-hard-cut sensitivity boost. See is_within_post_hard_cut_window() and
                // is_diag_post_hard_cut_sensitivity_boost_enabled() for full rationale - temporarily
                // lowers the spike-detector's multiplier for a short window after a hard cut fires, so
                // smaller residual jitter later in the SAME skill/dash animation also gets fully
                // corrected, not just the single frame that originally crossed the threshold.
                const bool use_sensitivity_boost = vr->is_diag_post_hard_cut_sensitivity_boost_enabled() &&
                    hook_data.is_within_post_hard_cut_window(vr->get_diag_post_hard_cut_sensitivity_boost_window_ms());
                const bool is_hard_cut = hook_data.is_nsf_sync_pose_hard_cut((float)rot_delta, (float)pos_delta,
                    use_sensitivity_boost ? vr->get_diag_post_hard_cut_sensitivity_boost_multiplier() : 0.0f);

                // TRIGGER: only report a pose-divergence event (which feeds the auto-mirror-on-motion
                // fallback) on an actual hard-cut-grade spike, NOT on the near-zero logging threshold
                // above. That threshold (0.001deg/0.01 units) is essentially noise-floor and fires on
                // ordinary thumbstick turning/camera smoothing every single frame while moving, which
                // kept re-extending the mirror's 250ms activity window for as long as - and for a
                // while after - the player was actively turning. Because the mirror path shows an
                // identical (non-stereo) image in both eyes, that read as a long-lived "static/frozen"
                // right eye rather than a brief one-frame correction. Gating on the same spike detector
                // used for the hard-cut snap ensures the mirror only engages for genuine abrupt
                // animation/VFX camera discontinuities, not continuous player-driven motion.
                if (is_hard_cut) {
                    g_hook->report_pose_divergence_event();
                }

                // DIAG: is_native_stereo_fix_sync_pose_force_full_enabled() forces the full 1.0 snap
                // unconditionally, bypassing the hard-cut spike detector entirely, so we can confirm
                // with certainty that the sync path engages on every single Pass2 call during a
                // reported multi-frame glitch window (e.g. UI menu open), not just on detected spikes.
                const bool force_full = vr->is_native_stereo_fix_sync_pose_force_full_enabled();

                // DIAG: gradual hard-cut convergence test - see get_nsf_gradual_convergence_alpha() for
                // rationale. Replaces the instant blend_alpha=1.0 snap with a ramp from 0->1 over
                // get_diag_gradual_hard_cut_convergence_duration_ms() so both eyes ease into agreement
                // instead of one eye teleporting into place.
                const bool use_gradual_convergence = vr->is_diag_gradual_hard_cut_convergence_enabled();

                const auto blend_alpha = use_gradual_convergence
                    ? (double)hook_data.get_nsf_gradual_convergence_alpha(is_hard_cut || force_full, vr->get_diag_gradual_hard_cut_convergence_duration_ms())
                    : (is_hard_cut || force_full)
                        ? 1.0
                        : (double)vr->get_native_stereo_fix_sync_pose_blend_alpha();

                if (force_full && vr->is_diag_sync_pose_verbose_logging_enabled()) {
                    SPDLOG_WARN("[VR][NSF-SYNC-FORCE-FULL] frame={} rot_delta_deg={:.4f} pos_delta={:.4f} applying FULL snap (is_hard_cut={})",
                        g_frame_count, rot_delta, pos_delta, is_hard_cut);
                }

                // DIAG: dash-blur numeric capture. See request_dash_capture()/request_dash_capture_mark()
                // in VR.hpp - logs every relevant value for this Pass2 evaluation, unthrottled, while a
                // capture window is active (armed via NumPad1), and tags the exact frame NumPad2 was
                // pressed on so the perceived-blur moment can be correlated against the surrounding data.
                if (bool marked = false; auto seq = vr->consume_dash_capture_frame(marked)) {
                    SPDLOG_WARN("[VR][DASH-CAPTURE]{} seq={} frame={} rot_delta_deg={:.4f} pos_delta={:.4f} "
                                "rot_ema={:.4f} pos_ema={:.4f} is_hard_cut={} force_full={} blend_alpha={:.3f} "
                                "pass1_rot=({:.3f},{:.3f},{:.3f}) pass2_rot_before=({:.3f},{:.3f},{:.3f})",
                        marked ? " [MARK]" : "", *seq, g_frame_count, rot_delta, pos_delta,
                        hook_data.m_nsf_pose_delta_rot_ema, hook_data.m_nsf_pose_delta_pos_ema,
                        is_hard_cut, force_full, blend_alpha,
                        hook_data.m_nsf_sync_pose_rotation_double.pitch, hook_data.m_nsf_sync_pose_rotation_double.yaw, hook_data.m_nsf_sync_pose_rotation_double.roll,
                        rot_d->pitch, rot_d->yaw, rot_d->roll);
                }

                if (blend_alpha >= 1.0) {
                    *rot_d = hook_data.m_nsf_sync_pose_rotation_double;
                } else if (blend_alpha > 0.0) {
                    rot_d->yaw = std::lerp(rot_d->yaw, hook_data.m_nsf_sync_pose_rotation_double.yaw, blend_alpha);
                    rot_d->pitch = std::lerp(rot_d->pitch, hook_data.m_nsf_sync_pose_rotation_double.pitch, blend_alpha);
                    rot_d->roll = std::lerp(rot_d->roll, hook_data.m_nsf_sync_pose_rotation_double.roll, blend_alpha);
                }

                // Sync the raw (pre-IPD) camera POSITION too, if enabled. This runs BEFORE the
                // eye_separation/IPD offset math further down in this function (which reads *view_d /
                // *view_location and subtracts a per-eye offset derived from true_index), so overwriting
                // the raw position here does NOT collapse stereo parallax - each eye still gets its own
                // IPD offset applied afterward on top of this now-synced base position. Same blend-
                // alpha applies here as for rotation above.
                //
                // DIAG: FIX ATTEMPT #1/#2 for dash/fast-turn blur (see DiagRotationGatedPositionSync/
                // DiagSustainedMotionPositionSyncSuppression in VR.hpp). Real capture data proved the
                // dash blur frames have rot_delta_deg==0.0000 (no genuine rotational mismatch) while
                // pos_delta stays moderately elevated every frame purely from continuous motion, and
                // the unconditional position snap below fights that normal per-eye parallax. Gate the
                // POSITION portion only (rotation sync above is unaffected) behind these two optional,
                // independently toggleable tests. BOTH also require pos_delta to be under the shared
                // ceiling - some hard cuts are purely positional (huge pos_delta, near-zero rotation)
                // and must never be suppressed just because rotation looks dash-like.
                const bool is_low_rotation = (float)rot_delta < vr->get_diag_rotation_gated_position_sync_threshold_deg();
                const bool is_low_position = (float)pos_delta < vr->get_diag_position_sync_suppression_pos_delta_ceiling();
                const bool is_dash_like = is_low_rotation && is_low_position;
                const bool suppress_by_sustained_motion = hook_data.update_and_check_sustained_motion_suppression(
                    is_dash_like, vr->get_diag_sustained_motion_position_sync_suppression_frames())
                    && vr->is_diag_sustained_motion_position_sync_suppression_enabled();
                const bool suppress_by_rotation_gate = vr->is_diag_rotation_gated_position_sync_enabled() && is_dash_like;
                const bool suppress_position_sync = suppress_by_rotation_gate || suppress_by_sustained_motion;

                if (vr->is_native_stereo_fix_sync_pose_position_enabled() && !suppress_position_sync) {
                    if (blend_alpha >= 1.0) {
                        *local_view_d = hook_data.m_nsf_sync_pose_location_double;
                    } else if (blend_alpha > 0.0) {
                        local_view_d->x = std::lerp(local_view_d->x, hook_data.m_nsf_sync_pose_location_double.x, blend_alpha);
                        local_view_d->y = std::lerp(local_view_d->y, hook_data.m_nsf_sync_pose_location_double.y, blend_alpha);
                        local_view_d->z = std::lerp(local_view_d->z, hook_data.m_nsf_sync_pose_location_double.z, blend_alpha);
                    }
                }
            } else {
                const auto rot_delta = glm::length(glm::vec3{
                    view_rotation->yaw - hook_data.m_nsf_sync_pose_rotation.yaw,
                    view_rotation->pitch - hook_data.m_nsf_sync_pose_rotation.pitch,
                    view_rotation->roll - hook_data.m_nsf_sync_pose_rotation.roll});

                // DIAG: NSF-POSE-DIVERGE (single-precision path). See double-precision branch above for
                // full rationale - logs the un-applied Pass1 vs Pass2 position delta for the same frame.
                const auto pos_delta = glm::length(*view_location - hook_data.m_nsf_sync_pose_location);

                // DIAG: unconditional (throttled) trace to prove this branch is actually reached and
                // to see the real delta magnitudes even when they stay under the warn threshold below.
                // Gated behind is_diag_sync_pose_verbose_logging_enabled() - fires every Pass2 call and
                // is only meant to be enabled briefly while reproducing/tuning, not left on constantly.
                if (vr->is_diag_sync_pose_verbose_logging_enabled()) {
                    SPDLOG_INFO_EVERY_N_SEC(1, "[VR][NSF-POSE-DIVERGE-TRACE] Pass2 reached, frame={} rot_delta_deg={:.4f} pos_delta={:.4f}",
                        g_frame_count, rot_delta, pos_delta);

                    if (rot_delta > 0.001f || pos_delta > 0.01f) {
                        SPDLOG_WARN("[VR][NSF-POSE-DIVERGE] frame={} rot_delta_deg={:.4f} pos_delta={:.4f} pass1_pos=({:.2f},{:.2f},{:.2f}) pass2_pos=({:.2f},{:.2f},{:.2f})",
                            g_frame_count, rot_delta, pos_delta,
                            hook_data.m_nsf_sync_pose_location.x, hook_data.m_nsf_sync_pose_location.y, hook_data.m_nsf_sync_pose_location.z,
                            view_location->x, view_location->y, view_location->z);
                    }
                }

                // HARD-CUT OVERRIDE: see double-precision branch above for full rationale. Uses spike-
                // relative-to-baseline + absolute-floor detection instead of a flat threshold, since a
                // flat threshold alone misfires on fast thumbstick turns (this runs every frame
                // regardless of what's driving the camera, unlike the Lua script's CineCameraActor gate).
                const bool use_sensitivity_boost = vr->is_diag_post_hard_cut_sensitivity_boost_enabled() &&
                    hook_data.is_within_post_hard_cut_window(vr->get_diag_post_hard_cut_sensitivity_boost_window_ms());
                const bool is_hard_cut = hook_data.is_nsf_sync_pose_hard_cut(rot_delta, pos_delta,
                    use_sensitivity_boost ? vr->get_diag_post_hard_cut_sensitivity_boost_multiplier() : 0.0f);

                // TRIGGER: see double-precision branch above for full rationale - only report on an
                // actual hard-cut-grade spike, not the near-zero logging threshold, so the auto-mirror
                // fallback doesn't stay engaged (showing a flat non-stereo image) for the entire
                // duration of ordinary thumbstick-driven camera turning.
                if (is_hard_cut) {
                    g_hook->report_pose_divergence_event();
                }

                // DIAG: see the matching double-precision branch above for full rationale.
                const bool force_full = vr->is_native_stereo_fix_sync_pose_force_full_enabled();

                const bool use_gradual_convergence = vr->is_diag_gradual_hard_cut_convergence_enabled();

                const auto blend_alpha_f = use_gradual_convergence
                    ? hook_data.get_nsf_gradual_convergence_alpha(is_hard_cut || force_full, vr->get_diag_gradual_hard_cut_convergence_duration_ms())
                    : (is_hard_cut || force_full)
                        ? 1.0f
                        : vr->get_native_stereo_fix_sync_pose_blend_alpha();

                if (force_full && vr->is_diag_sync_pose_verbose_logging_enabled()) {
                    SPDLOG_WARN("[VR][NSF-SYNC-FORCE-FULL] frame={} rot_delta_deg={:.4f} pos_delta={:.4f} applying FULL snap (is_hard_cut={})",
                        g_frame_count, rot_delta, pos_delta, is_hard_cut);
                }

                // DIAG: see the matching double-precision branch above for full rationale.
                if (bool marked = false; auto seq = vr->consume_dash_capture_frame(marked)) {
                    SPDLOG_WARN("[VR][DASH-CAPTURE]{} seq={} frame={} rot_delta_deg={:.4f} pos_delta={:.4f} "
                                "rot_ema={:.4f} pos_ema={:.4f} is_hard_cut={} force_full={} blend_alpha={:.3f} "
                                "pass1_rot=({:.3f},{:.3f},{:.3f}) pass2_rot_before=({:.3f},{:.3f},{:.3f})",
                        marked ? " [MARK]" : "", *seq, g_frame_count, rot_delta, pos_delta,
                        hook_data.m_nsf_pose_delta_rot_ema, hook_data.m_nsf_pose_delta_pos_ema,
                        is_hard_cut, force_full, blend_alpha_f,
                        hook_data.m_nsf_sync_pose_rotation.pitch, hook_data.m_nsf_sync_pose_rotation.yaw, hook_data.m_nsf_sync_pose_rotation.roll,
                        view_rotation->pitch, view_rotation->yaw, view_rotation->roll);
                }

                if (blend_alpha_f >= 1.0f) {
                    *view_rotation = hook_data.m_nsf_sync_pose_rotation;
                } else if (blend_alpha_f > 0.0f) {
                    view_rotation->yaw = std::lerp(view_rotation->yaw, hook_data.m_nsf_sync_pose_rotation.yaw, blend_alpha_f);
                    view_rotation->pitch = std::lerp(view_rotation->pitch, hook_data.m_nsf_sync_pose_rotation.pitch, blend_alpha_f);
                    view_rotation->roll = std::lerp(view_rotation->roll, hook_data.m_nsf_sync_pose_rotation.roll, blend_alpha_f);
                }

                // See double-precision branch above for rationale: this runs before the later per-eye
                // eye_separation/IPD offset is applied to *view_location, so parallax is preserved.
                //
                // DIAG: see the matching double-precision branch above for full rationale on the
                // rotation-gate/sustained-motion position-sync suppression tests. Both also require
                // pos_delta to be under the shared ceiling so purely-positional hard cuts (huge
                // pos_delta, near-zero rotation) are never suppressed.
                const bool is_low_rotation = rot_delta < vr->get_diag_rotation_gated_position_sync_threshold_deg();
                const bool is_low_position = pos_delta < vr->get_diag_position_sync_suppression_pos_delta_ceiling();
                const bool is_dash_like = is_low_rotation && is_low_position;
                const bool suppress_by_sustained_motion = hook_data.update_and_check_sustained_motion_suppression(
                    is_dash_like, vr->get_diag_sustained_motion_position_sync_suppression_frames())
                    && vr->is_diag_sustained_motion_position_sync_suppression_enabled();
                const bool suppress_by_rotation_gate = vr->is_diag_rotation_gated_position_sync_enabled() && is_dash_like;
                const bool suppress_position_sync = suppress_by_rotation_gate || suppress_by_sustained_motion;

                if (vr->is_native_stereo_fix_sync_pose_position_enabled() && !suppress_position_sync) {
                    if (blend_alpha_f >= 1.0f) {
                        *view_location = hook_data.m_nsf_sync_pose_location;
                    } else if (blend_alpha_f > 0.0f) {
                        view_location->x = std::lerp(view_location->x, hook_data.m_nsf_sync_pose_location.x, blend_alpha_f);
                        view_location->y = std::lerp(view_location->y, hook_data.m_nsf_sync_pose_location.y, blend_alpha_f);
                        view_location->z = std::lerp(view_location->z, hook_data.m_nsf_sync_pose_location.z, blend_alpha_f);
                    }
                }
            }
        }
    }

    // DIAG: DiagApplySyncedPoseToExcludedViewIndex. view_index=1 was excluded above from ever
    // writing to/consuming the NSF sync-pose cache (it races with the real eye and isn't a
    // renderable eye - see NSF-VIEWINDEX-IDENTITY: it never gets its own FSceneView, and fires a
    // variable number of times per frame, consistent with a shadow-cascade/occlusion sub-view pass
    // rather than a second eye). That exclusion fixed the eye blur/desync, but left this index's own
    // pose completely untouched, so whatever shadow/culling subsystem consumes it keeps seeing
    // whatever raw (possibly stale/mid-transition) pose the engine handed it - plausibly explaining
    // inconsistent foliage-sway culling/shadow behavior independent of the eye-blur fix.
    //
    // This applies the SAME fully-converged pose the real eyes settle on (read-only: does not set
    // have_left, does not write m_nsf_sync_pose_*) to the excluded index, so its frustum/pose stays
    // coherent with the actual HMD pose without re-introducing the cache race. Gated so it can be
    // disabled independently if it turns out to break/flicker culling instead of fixing it.
    if (diag_exclude_from_sync_cache && vr->is_diag_apply_synced_pose_to_excluded_view_index_enabled() &&
        vr->is_native_stereo_fix_enabled() && !vr->is_using_afr() && vr->is_native_stereo_fix_sync_pose_enabled() && !is_full_pass) {
        auto& hook_data = *g_hook;

        if (has_double_precision) {
            *rot_d = hook_data.m_nsf_sync_pose_rotation_double;

            if (vr->is_native_stereo_fix_sync_pose_position_enabled()) {
                auto* local_view_d = (Vector3d*)view_location;
                *local_view_d = hook_data.m_nsf_sync_pose_location_double;
            }
        } else {
            *view_rotation = hook_data.m_nsf_sync_pose_rotation;

            if (vr->is_native_stereo_fix_sync_pose_position_enabled()) {
                *view_location = hook_data.m_nsf_sync_pose_location;
            }
        }

        if (vr->is_diag_sync_pose_verbose_logging_enabled()) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR][NSF-EXCLUDED-INDEX-SYNCED] view_index={} frame={} applied cached synced pose (read-only)",
                view_index, g_frame_count);
        }
    }

    if (true_index == 0 && !is_full_pass) {
        if (has_double_precision) {
            g_hook->m_last_pre_rotation_double = *rot_d;
        } else {
            g_hook->m_last_pre_rotation = *view_rotation;
        }

        //vr->wait_for_present();

        if (!g_hook->m_has_view_extension_hook && !g_hook->m_has_game_viewport_client_draw_hook) {
            vr->update_hmd_state();
        }
    }

    // DIAG: throttled visibility into the resolved eye index for this stereo view-offset call.
    // If true_index never (or rarely) resolves to 0 while is_using_afr() is true, the engine is
    // never being asked to render the left-eye camera pass at all - which would fully explain a
    // permanently-black left eye independent of the AFR copy path (which only copies whatever the
    // engine actually rendered).
    //
    // IMPORTANT: this diagnostic's true_index is derived from this function's own local g_frame_count/
    // index_starts_from_one state, which is INDEPENDENT of D3D12Component.cpp's is_left_eye_frame
    // classification (based on vr->m_render_frame_count % 2 == m_left_eye_interval). If these two
    // separately-computed "which eye is this" trackers ever fall out of phase with each other, the
    // engine could be rendering the left-eye camera into a viewport that D3D12Component then samples/
    // copies as the wrong eye, producing a permanently-black eye despite both eyes rendering correctly.
    // Log vr's frame-count/interval state here too so the two can be correlated directly in the log.
    // Also fixed the previous "% 300 == 1" throttle: since this function and D3D12Component::on_frame's
    // diag_afr_count both increment roughly once per real per-eye call, a fixed EVEN stride can alias
    // onto a single parity and make it look like only one eye is ever logged after warm-up, when in
    // fact both are still occurring - use an ODD stride instead so both parities are sampled during
    // steady-state logging.
    {
        static uint32_t diag_stereo_offset_count = 0;
        ++diag_stereo_offset_count;

        // DIAG: running per-eye call counters. This directly answers "does the engine ever stop
        // asking for a left-eye camera pass at all", independent of whatever D3D12Component later
        // does with that pass's output. If diag_left_calls stops incrementing entirely (while
        // diag_right_calls keeps climbing) at some point in the log, the black left eye is proven
        // to originate upstream of our hooks entirely (the engine itself stopped requesting a
        // left-eye view), not in our copy/compositor code.
        static uint64_t diag_left_calls = 0;
        static uint64_t diag_right_calls = 0;
        static uint32_t diag_last_left_call_index = 0;
        static uint32_t diag_last_right_call_index = 0;

        if (true_index == 0) {
            ++diag_left_calls;
            diag_last_left_call_index = diag_stereo_offset_count;
        } else {
            ++diag_right_calls;
            diag_last_right_call_index = diag_stereo_offset_count;
        }

        if (vr->is_diag_verbose_logging_enabled() && (diag_stereo_offset_count <= 20 || diag_stereo_offset_count % 301 == 1)) {
            SPDLOG_INFO("[DIAG] calculate_stereo_view_offset (#{}): view_index={} true_index={} is_full_pass={} is_using_afr={} g_frame_count={} index_starts_from_one={} vr_frame_count={} vr_left_interval={} vr_right_interval={} left_calls={} right_calls={} last_left_call=#{} last_right_call=#{}",
                diag_stereo_offset_count, view_index, true_index, is_full_pass, vr->is_using_afr(), g_frame_count, index_starts_from_one,
                vr->m_render_frame_count, vr->m_left_eye_interval, vr->m_right_eye_interval,
                diag_left_calls, diag_right_calls, diag_last_left_call_index, diag_last_right_call_index);
        }

        // DIAG: unconditional low-frequency heartbeat (time-based, not call-count-based) so a
        // left-eye stall is visible even if it happens to occur between the count-based throttle
        // windows above. If "left_calls" is ever seen to stop advancing across two consecutive
        // heartbeats while "right_calls" keeps advancing, the engine has stopped requesting a
        // left-eye camera pass entirely - proving the root cause is upstream of any of our hooks.
        if (vr->is_diag_verbose_logging_enabled()) {
            SPDLOG_INFO_EVERY_N_SEC(3, "[DIAG] calculate_stereo_view_offset heartbeat: left_calls={} right_calls={} last_left_call=#{} last_right_call=#{} is_using_afr={}",
                diag_left_calls, diag_right_calls, diag_last_left_call_index, diag_last_right_call_index, vr->is_using_afr());
        }
    }

    /*if (view_index % 2 == 1 && VR::get()->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
        std::scoped_lock _{ vr->get_runtime()->render_mtx };
        SPDLOG_INFO("SYNCING!!!");
        //vr->get_runtime()->synchronize_frame();
        vr->update_hmd_state();
    }*/

    // if we were unable to hook UGameEngine::Tick, we can run our game thread jobs here instead.
    if (!is_full_pass && !g_hook->m_has_view_extension_hook && g_hook->m_attempted_hook_game_engine_tick && !g_hook->m_hooked_game_engine_tick) {
        GameThreadWorker::get().execute();
    }

    if (vr->is_sceneview_compatibility_enabled() && !g_hook->m_inside_manual_view_offset) {
        return;
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    if (!is_full_pass) {
        for (auto& mod : mods) {
            mod->on_early_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }

        for (auto& mod : mods) {
            mod->on_pre_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }
    }

    const auto view_d = (Vector3d*)view_location;

    const auto view_mat = !has_double_precision ? 
        glm::yawPitchRoll(
            glm::radians(view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians((float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians((float)rot_d->roll));

    const auto view_mat_inverse = !has_double_precision ? 
        glm::yawPitchRoll(
            glm::radians(-view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(-view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians(-(float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians(-(float)rot_d->roll));

    const auto view_quat_inverse = glm::quat {
        view_mat_inverse
    };

    const auto view_quat = glm::quat {
        view_mat
    };

    const auto quat_converter = glm::quat{Matrix4x4f {
        0, 0, -1, 0,
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 0, 1
    }};

    auto vqi_norm = glm::normalize(view_quat_inverse);

    // Decoupled Pitch
    if (vr->is_decoupled_pitch_enabled()) {
        vr->set_pre_flattened_rotation(vqi_norm);
        vqi_norm = utility::math::flatten(vqi_norm);
    }

    const auto camera_forward_offset = vr->get_camera_forward_offset();
    const auto camera_right_offset = vr->get_camera_right_offset();
    const auto camera_up_offset = vr->get_camera_up_offset();
    const auto camera_forward = quat_converter * (vqi_norm * glm::vec3{0, 0, camera_forward_offset});
    const auto camera_right = quat_converter * (vqi_norm * glm::vec3{-camera_right_offset, 0, 0});
    const auto camera_up = quat_converter * (vqi_norm * glm::vec3{0, -camera_up_offset, 0});

    const auto world_scale = world_to_meters * vr->get_world_scale();

    if (has_double_precision) {
        *view_d += camera_forward;
        *view_d += camera_right;
        *view_d += camera_up;
    } else {
        *view_location += camera_forward;
        *view_location += camera_right;
        *view_location += camera_up;
    }

    // Don't apply any headset transformations
    // if we have stereo emulation mode enabled
    // it is only for debugging purposes
    if (!vr->is_stereo_emulation_enabled()) {
        const auto is_2d_screen = vr->is_using_2d_screen();

        const auto rotation_offset = vr->get_rotation_offset();
        const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{vr->get_rotation(0)});
        const auto current_eye_rotation_offset = glm::normalize(glm::quat{vr->get_eye_transform(true_index)});

        const auto new_rotation = glm::normalize(vqi_norm * current_hmd_rotation * current_eye_rotation_offset);
        const auto eye_offset = glm::vec3{vr->get_eye_offset((VRRuntime::Eye)(true_index))};


        const auto standing_delta = vr->get_position(0) - vr->get_standing_origin();
        const auto standing_delta_flat = glm::vec3{standing_delta.x, 0, standing_delta.z};

        const auto pos = glm::vec3{rotation_offset * standing_delta};
        const auto pos_flat = glm::vec3{rotation_offset * standing_delta_flat};

        const auto head_offset = quat_converter * (vqi_norm * (pos * world_scale));
        const auto head_offset_flat = quat_converter * (vqi_norm * (pos_flat * world_scale));
        const auto eye_separation = quat_converter * (glm::normalize(new_rotation) * (eye_offset * world_scale));

        // DIAGNOSTIC: confirm whether this hook (and the is_2d_screen guard around
        // head_offset/eye_separation) is actually reached, and with what values, when
        // testing 2D-screen-mode UI interaction. Rate-limited to avoid log spam.
        if (LGUI_DIAG_STEADY_STATE) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR][diag] calculate_stereo_view_offset: is_2d_screen={} is_using_afr={} true_index={} eye_separation=({:.3f},{:.3f},{:.3f}) head_offset=({:.3f},{:.3f},{:.3f})",
                is_2d_screen, vr->is_using_afr(), true_index,
                eye_separation.x, eye_separation.y, eye_separation.z,
                head_offset.x, head_offset.y, head_offset.z);
        }

        // DIAG: GLITCH-EYE-DIAG window. Logs unconditionally (no throttle) for a few seconds after
        // a glitch marker press, for BOTH eyes, so we can see the actual per-eye stereo separation
        // vector frame-by-frame across the glitch moment. If eye_separation's magnitude collapses
        // toward 0 (or becomes near-identical between true_index=0 and true_index=1 calls) while this
        // window is active, the camera-level stereo math itself is failing for that VFX-heavy moment.
        // If eye_separation stays normal/nonzero and mismatched between eyes as expected here, the
        // camera-level math is fine and the flattened-looking VFX element must be a problem isolated
        // to that effect's own material/shader (e.g. a camera-facing billboard computed once and
        // shared across both eyes) rather than anything in this per-eye offset hook.
        if (vr->is_glitch_eye_diag_window_active()) {
            // DIAG: is_full_pass/is_using_afr/view_index are included so we can tell a real
            // NSF gameplay Pass1/Pass2 call apart from a secondary scene-capture (portrait,
            // minimap, reflection, VFX billboard, etc.) that Unreal also routes through this
            // same hook and which typically presents as view_index=0/is_full_pass=true and
            // always resolves to true_index=0 - previously this was indistinguishable from a
            // real left-eye call and made the left/right call counts look imbalanced (e.g. 3
            // true_index=0 logs per 1 true_index=1 log) even though NSF itself was still
            // alternating 1:1 for the real stereo pass.
            SPDLOG_WARN("[VR][GLITCH-EYE-DIAG] true_index={} view_index={} is_full_pass={} is_using_afr={} eye_separation=({:.4f},{:.4f},{:.4f}) len={:.4f} head_offset=({:.4f},{:.4f},{:.4f}) g_frame_count={}",
                true_index, view_index, is_full_pass, vr->is_using_afr(), eye_separation.x, eye_separation.y, eye_separation.z, glm::length(eye_separation),
                head_offset.x, head_offset.y, head_offset.z, g_frame_count);
        }

        if (!has_double_precision) {
            if (!is_2d_screen) {
                *view_location -= head_offset;
                *view_location -= eye_separation;
            }
        } else {
            if (!is_2d_screen) {
                *view_d -= head_offset;
                *view_d -= eye_separation;
            }
        }

        if (!is_2d_screen) {
            const auto euler = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation));

            if (!has_double_precision) {
                view_rotation->pitch = euler.x;
                view_rotation->yaw = euler.y;
                view_rotation->roll = euler.z;
            } else {
                rot_d->pitch = euler.x;
                rot_d->yaw = euler.y;
                rot_d->roll = euler.z;
            }
        }

        // DIAG: NSF-FINAL-EYE-POSE. Logs the FULLY RESOLVED per-eye camera position/rotation (post
        // head_offset/eye_separation/sync-pose - i.e. exactly what each eye renders from this frame),
        // unthrottled for both eyes every frame. See m_diag_log_final_eye_pose declaration for
        // rationale: this lets the two eyes' pose trajectories be diffed directly across an entire
        // multi-frame glitch window instead of a short glitch-marker capture, without needing to
        // reproduce the issue near a button press. Deliberately verbose/unthrottled - only meant to be
        // enabled briefly while reproducing the issue, then disabled again.
        if (vr->is_diag_log_final_eye_pose_enabled()) {
            if (!has_double_precision) {
                SPDLOG_WARN("[VR][NSF-FINAL-EYE-POSE] eye={} view_index={} frame={} pos=({:.3f},{:.3f},{:.3f}) rot=({:.3f},{:.3f},{:.3f})",
                    true_index, view_index, g_frame_count, view_location->x, view_location->y, view_location->z,
                    view_rotation->pitch, view_rotation->yaw, view_rotation->roll);
            } else {
                SPDLOG_WARN("[VR][NSF-FINAL-EYE-POSE] eye={} view_index={} frame={} pos=({:.3f},{:.3f},{:.3f}) rot=({:.3f},{:.3f},{:.3f})",
                    true_index, view_index, g_frame_count, view_d->x, view_d->y, view_d->z,
                    rot_d->pitch, rot_d->yaw, rot_d->roll);
            }
        }

        // Roomscale movement
        // only do it on the right eye pass
        // if we did it on the left, there would be eye desyncs when the right eye is rendered
        if (true_index == 1 && (vr->is_roomscale_enabled() || vr->is_aim_pawn_control_rotation_enabled())) {
            const auto world = sdk::UEngine::get()->get_world();

            if (const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); controller != nullptr) {
                const auto pawn = controller->get_acknowledged_pawn();

                static bool was_pawn_rotation_enabled = false;

                if (pawn != nullptr && vr->is_aim_pawn_control_rotation_enabled()) {
                    auto camera_component = (sdk::UObject*)pawn->get_camera_component();

                    if (camera_component != nullptr && camera_component->get_class() != nullptr) {
                        static const auto boolprop = (sdk::FBoolProperty*)camera_component->get_class()->find_property(L"bUsePawnControlRotation");

                        if (boolprop != nullptr) {
                            boolprop->set_value_in_object(camera_component, true);
                            was_pawn_rotation_enabled = true;
                        }
                    }
                } else if (pawn != nullptr && was_pawn_rotation_enabled) {
                    auto camera_component = (sdk::UObject*)pawn->get_camera_component();

                    if (camera_component != nullptr && camera_component->get_class() != nullptr) {
                        static const auto boolprop = (sdk::FBoolProperty*)camera_component->get_class()->find_property(L"bUsePawnControlRotation");

                        if (boolprop != nullptr) {
                            boolprop->set_value_in_object(camera_component, false);
                            was_pawn_rotation_enabled = false;
                        }
                    }
                }

                if (pawn != nullptr && vr->is_roomscale_enabled()) {
                    const auto pawn_pos = pawn->get_actor_location();
                    const auto new_pos = pawn_pos - head_offset_flat;

                    // Roomscale sweep option allows the actor to affect the world
                    // like push doors open, and prevent them from clipping through walls
                    pawn->set_actor_location(new_pos, vr->is_roomscale_sweep_enabled(), false);

                    // Recenter the standing origin
                    auto current_standing_origin = vr->get_standing_origin();
                    const auto hmd_pos = vr->get_position(0);
                    // dont touch the Y axis
                    current_standing_origin.x = hmd_pos.x;
                    current_standing_origin.z = hmd_pos.z;
                    vr->set_standing_origin(current_standing_origin);
                }
            }
        }

        // Process snapturn    
        vr->process_snapturn();
    }

    if (!is_full_pass) {
        for (auto& mod : mods) {
            mod->on_post_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }

        if (true_index == 0) {
            if (has_double_precision) {
                g_hook->m_last_rotation_double = *rot_d;
            } else {
                g_hook->m_last_rotation = *view_rotation;
            }
        }

        // Modify Player Control Rotation
        if (true_index == 1 && vr->is_aim_modify_player_control_rotation_enabled() && vr->is_any_aim_method_active()) {
            if (g_hook->m_tracking_system_hook != nullptr) {
                g_hook->m_tracking_system_hook->manual_update_control_rotation();
            }
        }
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("Finished calculating stereo view offset!");
#else
    SPDLOG_INFO_ONCE("Finished calculating stereo view offset!");
#endif
}

__forceinline Matrix4x4f* FFakeStereoRenderingHook::calculate_stereo_projection_matrix(FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate stereo projection matrix called! {} from {:x}", view_index, (uintptr_t)_ReturnAddress() - (uintptr_t)utility::get_module_within((uintptr_t)_ReturnAddress()).value_or(nullptr));
#else
    SPDLOG_INFO_ONCE("calculate stereo projection matrix called! {} from {:x}", view_index, (uintptr_t)_ReturnAddress() - (uintptr_t)utility::get_module_within((uintptr_t)_ReturnAddress()).value_or(nullptr));
#endif

    auto& vr = VR::get();

    // Only call PostInitProperties if ghosting fix enabled or native stereo is being used.
    // Also, if we don't have a hook on GetDesiredNumberOfViews, we need to call PostInitProperties
    //if (!vr->is_using_afr() || vr->is_ghosting_fix_enabled() || !g_hook->m_get_desired_number_of_views_hook) {
    if (!vr->should_skip_post_init_properties()) {
        if (!g_hook->m_fixed_localplayer_view_count) {
            if (!g_hook->m_calculate_stereo_projection_matrix_post_hook) {
                const auto return_address = (uintptr_t)_ReturnAddress();
                SPDLOG_INFO("Inserting midhook after CalculateStereoProjectionMatrix... @ {:x}", return_address);

                constexpr auto max_stack_depth = 100;
                uintptr_t stack[max_stack_depth]{};

                const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

                for (int i = 0; i < depth; i++) {
                    g_hook->m_projection_matrix_stack.push_back(stack[i]);
                    SPDLOG_INFO(" {:x}", (uintptr_t)stack[i]);
                }

                g_hook->m_calculate_stereo_projection_matrix_post_hook = safetyhook::create_mid((void*)return_address, &FFakeStereoRenderingHook::post_calculate_stereo_projection_matrix);

                if (!g_hook->m_calculate_stereo_projection_matrix_post_hook) {
                    SPDLOG_ERROR("Failed to insert midhook after CalculateStereoProjectionMatrix!");
                }
            }
        } else if (g_hook->m_calculate_stereo_projection_matrix_post_hook) {
            SPDLOG_INFO("Removing midhook after CalculateStereoProjectionMatrix, job is done...");
            g_hook->m_calculate_stereo_projection_matrix_post_hook = {};
            g_hook->m_get_projection_data_pre_hook = {};
        }   
    }

    if (!g_framework->is_game_data_intialized()) {
        if (vr->is_glitch_eye_diag_window_active()) {
            SPDLOG_WARN("[VR][NSF-PROJ-BYPASS] game data not initialized, projection override SKIPPED view_index={} g_frame_count={}", view_index, g_frame_count);
        }

        if (g_hook->m_calculate_stereo_projection_matrix_hook) {
            return g_hook->m_calculate_stereo_projection_matrix_hook.call<Matrix4x4f*>(stereo, out, view_index);
        }

        return out;
    }

    static bool index_starts_from_one = true;
    static bool index_was_ever_two = false;

    // This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.
    // or maybe we should, this could be used for WorldToScreen.
    /*if (index_was_ever_two && view_index == 0) {
        SPDLOG_INFO_ONCE("Index was ever two, and now it's zero. This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.");
        return out;
    }*/

    if (view_index == 2) {
        index_starts_from_one = true;
        index_was_ever_two = true;
    } else if (view_index == 0) {
        index_starts_from_one = false;
    }

    // Can happen if we hooked this differently.
    if (g_hook->m_calculate_stereo_projection_matrix_hook) {
        g_hook->m_calculate_stereo_projection_matrix_hook.call<Matrix4x4f*>(stereo, out, view_index);
    } else {
        if (g_hook->m_has_double_precision) {
            (*out)[3][2] = sdk::globals::get_near_clipping_plane();
        } else {
            (*(Matrix4x4d*)out)[3][2] = (double)sdk::globals::get_near_clipping_plane();
        }
    }

    if (VR::get()->is_using_2d_screen()) {
        if (vr->is_glitch_eye_diag_window_active()) {
            SPDLOG_WARN("[VR][NSF-PROJ-BYPASS] is_using_2d_screen==true, VR projection override SKIPPED view_index={} true_index_guess={} g_frame_count={}",
                view_index, index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2), g_frame_count);
        }

        float fov = 90.0f; // todo, get from FMinimalViewInfo

        const float width = VR::get()->get_hmd_width();
        const float height = VR::get()->get_hmd_height();
        const float half_fov = glm::radians(fov) / 2.0f;
        const float xs = 1.0f / glm::tan(half_fov);
        const float ys = width / glm::tan(half_fov) / height;
        const float near_z = sdk::globals::get_near_clipping_plane();

        if (g_hook->m_has_double_precision) {
            (*(Matrix4x4d*)out) = Matrix4x4d {
                xs, 0.0, 0.0, 0.0,
                0.0, ys, 0.0, 0.0,
                0.0, 0.0, 0.0, 1.0,
                0.0, 0.0, near_z, 0.0
            };
        } else {
            *out = Matrix4x4f {
                xs, 0.0f, 0.0f, 0.0f,
                0.0f, ys, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, near_z, 0.0f
            };
        }

        return out;
    }

    // SPDLOG_INFO("NearZ: {}", old_znear);

    if (out != nullptr) {
        auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);

        // REVERTED: an earlier attempt restricted the update_matrices()+projection-override block
        // below to a "main stereo pass" view_index range, based on a hypothesis that an extra
        // secondary view (VFX/portal capture) was corrupting the shared projection matrices.
        // In-headset testing DISPROVED this - it caused a permanent doubled-image regression on
        // every frame - so this is fully unconditional again, matching original behavior.
        if (vr->is_using_afr()) {
            // See the matching comment in calculate_stereo_view_offset: fold in a call-scoped
            // alternator so a stalled g_frame_count doesn't force multiple consecutive calls to
            // resolve to the same eye.
            static uint32_t last_proj_frame_count = 0;
            static uint32_t proj_call_index = 0;

            if (last_proj_frame_count != g_frame_count || proj_call_index > 1) {
                proj_call_index = 0;
            }

            last_proj_frame_count = g_frame_count;

            true_index = (g_frame_count + proj_call_index) % 2;
            ++proj_call_index;
        }

        // DIAG: NSF (native_stereo_fix_enabled && !is_using_afr) projection-matrix divergence trace.
        // calculate_stereo_view_offset's NSF-POSE-DIVERGE-TRACE proved rot/pos delta is consistently
        // 0.0000 during the reported "zoom" artifact (menu open, conversation camera switch), which
        // rules out position/rotation as the cause of that specific symptom. A "zoom" is by
        // definition a projection/FOV change, and unlike position/rotation this function replaces
        // the game's projection matrix outright with VR::get_projection_matrix(true_index) - a
        // per-eye HMD-derived matrix that should be STABLE per eye and not tied to the live camera at
        // all. If the two back-to-back Pass1/Pass2 calls within one NSF frame ever resolve to the
        // SAME true_index (both 0 or both 1), or resolve to the WRONG eye relative to
        // calculate_stereo_view_offset's true_index for the same frame, one eye would momentarily
        // reuse the other eye's (or a stale) projection matrix - which would look exactly like a
        // sudden zoom/snap in that eye. Track have-seen-Pass1 the same way sync-pose does and log any
        // same-eye-twice or missing-Pass1 condition immediately.
        if (vr->is_native_stereo_fix_enabled() && !vr->is_using_afr()) {
            static thread_local uint32_t s_proj_diag_frame_count = 0;
            static thread_local bool s_proj_diag_have_pass1 = false;
            static thread_local int32_t s_proj_diag_pass1_true_index = -1;

            if (g_frame_count != s_proj_diag_frame_count) {
                s_proj_diag_frame_count = g_frame_count;
                s_proj_diag_have_pass1 = false;
                s_proj_diag_pass1_true_index = -1;
            }

            if (!s_proj_diag_have_pass1) {
                s_proj_diag_have_pass1 = true;
                s_proj_diag_pass1_true_index = true_index;

                SPDLOG_INFO_EVERY_N_SEC(1, "[VR][NSF-PROJ-DIVERGE-TRACE] Pass1 frame={} true_index={} view_index={}",
                    g_frame_count, true_index, view_index);
            } else {
                const bool same_eye_twice = true_index == s_proj_diag_pass1_true_index;

                SPDLOG_INFO_EVERY_N_SEC(1, "[VR][NSF-PROJ-DIVERGE-TRACE] Pass2 frame={} true_index={} view_index={} pass1_true_index={} same_eye_twice={}",
                    g_frame_count, true_index, view_index, s_proj_diag_pass1_true_index, same_eye_twice);

                if (same_eye_twice) {
                    SPDLOG_WARN("[VR][NSF-PROJ-DIVERGE] frame={} BOTH projection matrix calls resolved to true_index={} - one eye is reusing the wrong eye's projection/FOV matrix this frame",
                        g_frame_count, true_index);
                }
            }
        }

        auto& double_matrix = *(Matrix4x4d*)out;

        // DIAG: NSF-PROJ-VALUE. Capture the RAW engine-provided projection matrix's FOV-relevant
        // terms (the [0][0]/[1][1] scale terms directly encode horizontal/vertical FOV; a "zoom"
        // is by definition a change in these) BEFORE we overwrite it with the fixed VR projection
        // below. If one eye's raw matrix is mid-animation-blend (changing frame to frame) while the
        // other is already stable, that proves the engine itself is feeding us desynced per-eye FOV
        // data for this frame, upstream of anything we do here.
        if (vr->is_glitch_eye_diag_window_active()) {
            const double raw_m00 = g_hook->m_has_double_precision ? double_matrix[0][0] : (double)(*out)[0][0];
            const double raw_m11 = g_hook->m_has_double_precision ? double_matrix[1][1] : (double)(*out)[1][1];

            SPDLOG_WARN("[VR][NSF-PROJ-VALUE-RAW] frame={} true_index={} view_index={} raw_m00={:.6f} raw_m11={:.6f}",
                g_frame_count, true_index, view_index, raw_m00, raw_m11);
        }

        if (!g_hook->m_has_double_precision) {
            float old_znear = (*out)[3][2];
            VR::get()->m_nearz = old_znear;            
            VR::get()->get_runtime()->update_matrices(old_znear, 10000.0f);
        } else {
            double old_znear = (double_matrix)[3][2];
            VR::get()->m_nearz = (float)old_znear;
            VR::get()->get_runtime()->update_matrices((float)old_znear, 10000.0f);
        }

        if (!g_hook->m_has_double_precision) {
            *out = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));
        } else {
            const auto fmat = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));
            double_matrix = fmat;
        }

        // DIAG: NSF-PROJ-VALUE. Capture the FINAL (post-override) per-eye projection FOV terms so
        // we can directly compare, frame-by-frame across the reported "one eye zooms back" window,
        // whether the two eyes' FINAL matrices ever differ in a way that isn't explained by normal
        // IPD/eye offset (i.e. m00/m11 should track HMD FOV only and be near-IDENTICAL between the
        // two eyes on any given frame, and STABLE frame-to-frame while the HMD FOV itself is fixed).
        // If these drift/mismatch here even though the RAW values above looked fine, the bug is in
        // our own override/get_projection_matrix() or update_matrices() path, not the engine's data.
        if (vr->is_glitch_eye_diag_window_active()) {
            const double final_m00 = g_hook->m_has_double_precision ? double_matrix[0][0] : (double)(*out)[0][0];
            const double final_m11 = g_hook->m_has_double_precision ? double_matrix[1][1] : (double)(*out)[1][1];

            SPDLOG_WARN("[VR][NSF-PROJ-VALUE-FINAL] frame={} true_index={} view_index={} final_m00={:.6f} final_m11={:.6f} nearz={:.4f}",
                g_frame_count, true_index, view_index, final_m00, final_m11, VR::get()->m_nearz);
        }
    } else {
        SPDLOG_ERROR("CalculateStereoProjectionMatrix returned nullptr!");
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("Finished calculating stereo projection matrix!");
#else
    SPDLOG_INFO_ONCE("Finished calculating stereo projection matrix!");
#endif
    
    return out;
}

__forceinline void FFakeStereoRenderingHook::render_texture_render_thread(FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list,
    FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) 
{
    if (!g_framework->is_game_data_intialized()) {
        return;
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("render texture render thread called!");
#else
    SPDLOG_INFO_ONCE("render texture render thread called!");
#endif


    if (!g_hook->is_slate_hooked() && g_hook->has_attempted_to_hook_slate()) {
        SPDLOG_INFO("Attempting to hook SlateRHIRenderer::DrawWindow_RenderThread using RenderTexture_RenderThread return address...");
        const auto return_address = (uintptr_t)_ReturnAddress();
        SPDLOG_INFO(" Return address: {:x}", return_address);
        g_hook->attempt_hook_slate_thread(return_address);
    }

    g_hook->get_slate_thread_worker()->execute(rhi_command_list);

    /*const auto return_address = (uintptr_t)_ReturnAddress();
    const auto slate_cvar_usage_location = sdk::vr::get_slate_draw_to_vr_render_target_usage_location();

    if (slate_cvar_usage_location) {
        const auto distance_from_usage = (intptr_t)(return_address - *slate_cvar_usage_location);

        if (distance_from_usage <= 0x200) {
            //SPDLOG_INFO("Ret: {:x} Distance: {:x}", return_address, distance_from_usage);

            auto& d3d11_vr = VR::get()->m_d3d11;
            auto& hook = g_framework->get_d3d11_hook();
            auto device = hook->get_device();
            ComPtr<ID3D11DeviceContext> context{};

            device->GetImmediateContext(&context);
            context->CopyResource(d3d11_vr.get_test_tex().Get(), (ID3D11Resource*)src_texture->get_native_resource());
            context->Flush();
        }
    }*/

    //g_hook->m_rtm.set_render_target(src_texture);

    /*if (g_hook->m_rtm.get_scene_target() != src_texture) {
        g_hook->m_rtm.set_render_target(src_texture);
    }*/

    // SPDLOG_INFO("{:x}", (uintptr_t)src_texture->GetNativeResource());

    // maybe the window size is actually a pointer we will find out later.
    /*if (g_hook->m_render_texture_render_thread_hook) {
        g_hook->m_render_texture_render_thread_hook->call<void*>(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    }*/
}

void FFakeStereoRenderingHook::init_canvas(FFakeStereoRendering* stereo, sdk::FSceneView* view, UCanvas* canvas) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("init canvas called!");
#else
    SPDLOG_INFO_ONCE("init canvas called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    // Since the FSceneView and UCanvas structures will probably vary wildly
    // in terms of field offsets and size, we will need to dynamically scan
    // from the return address of this function to find the ViewProjectionMatrix offset.
    // in the FSceneView and also the UCanvas.
    // it happens in the else block of the conditional statement that calls this function
    static uint32_t fsceneview_viewproj_offset = 0;
    static uint32_t ucanvas_viewproj_offset = 0;

    if (fsceneview_viewproj_offset == 0 || ucanvas_viewproj_offset == 0) {
        SPDLOG_INFO("Searching for FSceneView and UCanvas offsets...");
        SPDLOG_INFO("Canvas: {:x}", (uintptr_t)canvas);

        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto containing_function = utility::find_function_start(return_address);

        SPDLOG_INFO("Found containing function at {:x}", *containing_function);

        auto find_offsets = [](uintptr_t start, uintptr_t end) -> bool {
            for (auto ip = (uintptr_t)start; ip < end + 0x100;) {
                const auto ix = utility::decode_one((uint8_t*)ip);

                if (!ix) {
                    SPDLOG_ERROR("Failed to decode instruction at {:x}", ip);
                    break;
                }

                // The initial instructions look something like this
                /*
                0F 28 86 C0 03 00 00                          movaps  xmm0, xmmword ptr [rsi+3C0h]
                41 0F 11 87 80 02 00 00                       movups  xmmword ptr [r15+280h], xmm0
                */
                if (std::string_view{ix->Mnemonic} == "MOVAPS" && ix->Operands[1].Type == ND_OP_MEM) {
                    const auto next = utility::decode_one((uint8_t*)(ip + ix->Length));

                    if (next) {
                        if (std::string_view{next->Mnemonic} == "MOVUPS" && next->Operands[0].Type == ND_OP_MEM) {
                            fsceneview_viewproj_offset = ix->Operands[1].Info.Memory.Disp;
                            ucanvas_viewproj_offset = next->Operands[0].Info.Memory.Disp;
                            
                            SPDLOG_INFO("Found at {:x}", ip);
                            SPDLOG_INFO("Found FSceneView ViewProjectionMatrix offset: {:x}", fsceneview_viewproj_offset);
                            SPDLOG_INFO("Found UCanvas ViewProjectionMatrix offset: {:x}", ucanvas_viewproj_offset);
                            return true;
                            break;
                        }
                    }
                }

                ip += ix->Length;
            }

            return false;
        };

        if (!find_offsets(*containing_function, return_address)) {
            // If we still didn't find it at this stage, re-scan from the previous function from the previous function call instead.
            const auto potential_func = utility::calculate_absolute(return_address - 4);
            if (!find_offsets(potential_func, potential_func + 0x100)) {
                SPDLOG_ERROR("Failed to find offsets!");
                return;
            }
        }
    }

    //*(Matrix4x4f*)((uintptr_t)view + fsceneview_viewproj_offset) = VR::get()->get_projection_matrix(VRRuntime::Eye::LEFT);
    *(Matrix4x4f*)((uintptr_t)canvas + ucanvas_viewproj_offset) = *(Matrix4x4f*)((uintptr_t)view + fsceneview_viewproj_offset);
}

uint32_t FFakeStereoRenderingHook::get_desired_number_of_views_hook(FFakeStereoRendering* stereo, bool is_stereo_enabled) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get desired number of views hook called!");
#else
    SPDLOG_INFO_ONCE("get desired number of views hook called!");
#endif

    auto& vr = VR::get();

    if (g_hook->m_sceneview_data.inside_post_init_properties) {
        return 2;
    }

    if (!is_stereo_enabled || (vr->is_using_afr() && !vr->is_splitscreen_compatibility_enabled())) {
        // We need to know about the second scene state to fix ghosting, so set the view count to 2
        // after we know about it, we can continue returning 1.
        if (is_stereo_enabled && vr->is_ghosting_fix_enabled() && vr->is_using_afr() &&
            g_hook->m_sceneview_data.known_scene_states.size() < 2 && g_hook->m_fixed_localplayer_view_count &&
            !!g_hook->m_sceneview_data.constructor_hook && g_hook->m_has_view_extensions_installed)
        {
            // Only works correctly if view extensions are installed, so we can reset the view count to 1 without crashing
            return 2;
        }

        return 1;
    }

    if (vr->is_native_stereo_fix_enabled() && !vr->should_mirror_right_eye_this_frame()) {
        auto rtm = g_hook->get_render_target_manager();

        // This vfunc runs every frame even when begin_render_viewfamily_real is not reached (e.g. the
        // scene has no FScene yet or the view family is empty during a level transition), so it is the
        // most reliable place to notice that the world our scene-capture actor/render target lives in
        // has changed or is tearing down, and to release them before the engine's LoadMap GC pass.
        if (rtm->is_scene_capture_world_stale()) {
            SPDLOG_INFO("[VR] get_desired_number_of_views_hook: scene capture's world is stale (changed or tearing down), destroying proactively");
            rtm->destroy_scene_capture();
        }

        if ((rtm->get_scene_capture_render_target() == nullptr || !g_hook->m_sceneview_data.constructor_hook || !g_hook->m_render_module_begin_render_viewfamily_hook)) {
            if (rtm->get_scene_capture_utexture() == nullptr) {
                // This vfunc is called every frame, independent of begin_render_viewfamily_real/sceneview_constructor.
                // Without a loading guard here, this path will spam create_scene_capture() (actor spawn + full
                // render target/swapchain reallocation) on every single frame while the scene capture is invalid
                // during a level transition, which is heavy enough to stall the loading screen indefinitely.
                const bool diag_engine_tick_stalled = vr->is_engine_tick_stalled();
                const bool diag_local_player_missing = is_local_player_controller_missing();
                const bool diag_boot_phase_active = is_boot_phase_active();
                bool is_loading = diag_engine_tick_stalled || diag_local_player_missing || diag_boot_phase_active;
                const char* diag_loading_reason = "none";

                if (diag_engine_tick_stalled) {
                    diag_loading_reason = "engine_tick_stalled";
                } else if (diag_local_player_missing) {
                    diag_loading_reason = "local_player_controller_missing";
                } else if (diag_boot_phase_active) {
                    diag_loading_reason = "boot_phase_active";
                }

                if (are_loading_guards_disabled()) {
                    is_loading = false;
                    diag_loading_reason = "guards_disabled_override";
                }

                if (!is_loading) {
                    auto engine = sdk::UEngine::get();
                    auto world = engine != nullptr ? engine->get_world() : nullptr;

                    if (world == nullptr) {
                        is_loading = true;
                        diag_loading_reason = "world_is_null";
                    } else {
                        static auto world_class = world->get_class();
                        if (world_class != nullptr) {
                            static auto is_tearing_down_prop = world_class->find_property(L"bIsTearingDown");
                            if (is_tearing_down_prop != nullptr) {
                                auto prop_addr = (uint8_t*)((uintptr_t)world + is_tearing_down_prop->get_offset());
                                if (prop_addr != nullptr && (*prop_addr & 1) != 0) {
                                    is_loading = true;
                                    diag_loading_reason = "world_is_tearing_down";
                                }
                            }
                        }
                    }
                }

                SPDLOG_INFO_EVERY_N_SEC(2, "[DIAG] get_desired_number_of_views_hook: scene_capture_render_target=null scene_capture_utexture=null "
                    "is_loading={} reason={} sceneview_constructor_hook={} begin_render_viewfamily_hook={}",
                    is_loading, diag_loading_reason, (bool)g_hook->m_sceneview_data.constructor_hook,
                    (bool)g_hook->m_render_module_begin_render_viewfamily_hook);

                if (!is_loading) {
                    static auto last_create_time = std::chrono::steady_clock::time_point{};
                    const auto now = std::chrono::steady_clock::now();
                    SPDLOG_INFO("[VR] create_scene_capture() invoked from get_desired_number_of_views_hook ({}ms since last invocation)",
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_create_time).count());
                    last_create_time = now;
                    rtm->create_scene_capture();
                } else {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] get_desired_number_of_views_hook: skipping create_scene_capture(), level transition/load in progress");
                }
            }

            return 1; // wait for the scene capture render target to be set and FSceneView constructor to be hooked
        }
    }

    return 2;
}

// Only really necessary for 5.0.3 because for some reason negative view index gets passed into it
// but 5.0.3 doesn't account for this and thinks it's a secondary pass
// so the purpose of the hook (mostly) is to make those return eSSP_FULL to fix a crash
EStereoscopicPass FFakeStereoRenderingHook::get_view_pass_for_index_hook(FFakeStereoRendering* stereo, bool stereo_requested, int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get view pass for index hook called! {} {}", stereo_requested, view_index);
#else
    SPDLOG_INFO_ONCE("get view pass for index hook called! {} {}", stereo_requested, view_index);
#endif

    // On 5.0.3 this check is not here, it was only added in 5.1
    // So we need to imitate it here to prevent a crash
    if (!stereo_requested || view_index < 0) {
        return EStereoscopicPass::eSSP_FULL;
    }

    return view_index % 2 == 0 ? EStereoscopicPass::eSSP_PRIMARY : EStereoscopicPass::eSSP_SECONDARY;
}

IStereoRenderTargetManager* FFakeStereoRenderingHook::get_render_target_manager_hook(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get render target manager hook called!");
#else
    SPDLOG_INFO_ONCE("get render target manager hook called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return nullptr;
    }

    auto vr = VR::get();

    if (vr->is_stereo_emulation_enabled() || vr->is_extreme_compatibility_mode_enabled()) {
        return nullptr;
    }

    if (!vr->get_runtime()->got_first_poses || vr->is_hmd_active()) {
        if (g_hook->m_uses_old_rendertarget_manager) {
            return (IStereoRenderTargetManager*)&g_hook->m_rtm_418;
        }

        if (g_hook->m_special_detected) {
            return (IStereoRenderTargetManager*)&g_hook->m_rtm_special;
        }

        return &g_hook->m_rtm;
    }

    return nullptr;
}

IStereoLayers* FFakeStereoRenderingHook::get_stereo_layers_hook(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get stereo layers hook called!");
#else
    SPDLOG_INFO_ONCE("get stereo layers hook called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return nullptr;
    }

    if (!VR::get()->get_runtime()->got_first_poses || VR::get()->is_hmd_active()) {
        /*static uint8_t fake_data[0x100]{};

        if (*(uintptr_t*)&fake_data == 0) {
            *(uintptr_t*)&fake_data = (uintptr_t)utility::get_executable() + 0x3D13420; // test
        }

        //return &g_hook->m_sl;
        return (IStereoLayers*)&fake_data;*/
    }

    return nullptr;
}

void FFakeStereoRenderingHook::post_calculate_stereo_projection_matrix(safetyhook::Context& ctx) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("post calculate stereo projection matrix called!");
#else
    SPDLOG_INFO_ONCE("post calculate stereo projection matrix called!");
#endif

    if (g_hook->m_fixed_localplayer_view_count || g_hook->m_hooked_alternative_localplayer_scan) {
        return;
    }

    auto vfunc = utility::find_virtual_function_start(g_hook->m_calculate_stereo_projection_matrix_post_hook.target_address());

    if (!vfunc) {
        // attempt to hook GetProjectionData instead to get the localplayer
        SPDLOG_INFO("Failed to find virtual function start for CalculateStereoProjectionMatrix, attempting to hook GetProjectionData instead...");

        if (!g_hook->m_projection_matrix_stack.empty() && g_hook->m_projection_matrix_stack.size() >= 3) {
            const auto post_get_projection_data = g_hook->m_projection_matrix_stack[2];

            const auto get_projection_data_candidate_1 = utility::find_function_start_with_call(post_get_projection_data);
            const auto get_projection_data_candidate_2 = utility::find_virtual_function_start(post_get_projection_data);

            // Select whichever one is closest to post_get_projection_data
            std::optional<uintptr_t> get_projection_data{};

            if (get_projection_data_candidate_1 && get_projection_data_candidate_2) {
                const auto candidate_1_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_1);
                const auto candidate_2_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_2);

                if (candidate_1_distance < candidate_2_distance) {
                    get_projection_data = get_projection_data_candidate_1;
                } else {
                    get_projection_data = get_projection_data_candidate_2;
                }
            } else if (get_projection_data_candidate_1) {
                get_projection_data = get_projection_data_candidate_1;
            } else if (get_projection_data_candidate_2) {
                get_projection_data = get_projection_data_candidate_2;
            } else {
                // emergency fallback
                SPDLOG_INFO("Failed to find GetProjectionData, falling back to emergency fallback (this may not work)");
                get_projection_data = utility::find_function_start(post_get_projection_data);
            }

            if (get_projection_data) {
                SPDLOG_INFO("Successfully found GetProjectionData at {:x}", *get_projection_data);

                g_hook->m_hooked_alternative_localplayer_scan = true;

                g_hook->m_get_projection_data_pre_hook = safetyhook::create_mid((void*)*get_projection_data, &FFakeStereoRenderingHook::pre_get_projection_data);
                g_hook->m_projection_matrix_stack.clear();

                if (g_hook->m_get_projection_data_pre_hook) {
                    SPDLOG_INFO("Successfully hooked GetProjectionData");
                    return;
                } else {
                    SPDLOG_ERROR("Failed to hook GetProjectionData");
                }
            } else {
                SPDLOG_ERROR("Failed to find GetProjectionData!");
            }
        }
    }

    if (!vfunc) {
        SPDLOG_INFO("Could not find function via normal means, scanning for int3s...");

        const auto ref = utility::scan_reverse(g_hook->m_calculate_stereo_projection_matrix_post_hook.target_address(), 0x2000, "CC CC CC");

        if (ref) {
            vfunc = *ref + 3;
        }

        if (!vfunc) {
            g_hook->m_fixed_localplayer_view_count = true;
            SPDLOG_ERROR("Failed to find virtual function start for post calculate_stereo_projection_matrix!");
            return;
        }
    }

    // Scan forward until we find an assignment of the RCX register into a storage register.
    std::unordered_map<uint32_t, uintptr_t*> register_to_context {
        { NDR_RBX, &ctx.rbx },
        { NDR_RCX, &ctx.rcx },
        { NDR_RDX, &ctx.rdx },
        { NDR_RSI, &ctx.rsi },
        { NDR_RDI, &ctx.rdi },
        { NDR_RBP, &ctx.rbp },
        { NDR_RSP, &ctx.rsp },
        { NDR_R8, &ctx.r8 },
        { NDR_R9, &ctx.r9 },
        { NDR_R10, &ctx.r10 },
        { NDR_R11, &ctx.r11 },
        { NDR_R12, &ctx.r12 },
        { NDR_R13, &ctx.r13 },
        { NDR_R14, &ctx.r14 },
        { NDR_R15, &ctx.r15 },
    };

    INSTRUX ix{};
    std::optional<uint32_t> found_register{};
    auto ip = (uint8_t*)vfunc.value_or(0);

    while (true) {
        const auto status = NdDecodeEx(&ix, (ND_UINT8*)ip, 1000, ND_CODE_64, ND_DATA_64);

        if (!ND_SUCCESS(status)) {
            SPDLOG_INFO("Decoding failed with error {:x}!", (uint32_t)status);
            break;
        }

        if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_REG && ix.Operands[1].Type == ND_OP_REG && ix.Operands[1].Info.Register.Reg == NDR_RCX) {
            SPDLOG_INFO("Found assignment of RCX to storage register at {:x} ({})!", (uintptr_t)ip, ix.Operands[0].Info.Register.Reg);
            found_register = ix.Operands[0].Info.Register.Reg;
            break;
        }

        ip += ix.Length;
    }

    if (!found_register) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find assignment of RCX to storage register!");
        return;
    }

    const auto localplayer = *register_to_context[found_register.value_or(0)];
    SPDLOG_INFO("Local player: {:x}", localplayer);

    if (localplayer == 0) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find local player, cannot call PostInitProperties!");
        return;
    }

    g_hook->post_init_properties(localplayer);
}

void FFakeStereoRenderingHook::pre_get_projection_data(safetyhook::Context& ctx) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("pre get projection data called!");
#else
    SPDLOG_INFO_ONCE("pre get projection data called!");
#endif

    if (g_hook->m_fixed_localplayer_view_count) {
        return;
    }

    const auto localplayer = ctx.rcx;
    SPDLOG_INFO("Local player: {:x}", localplayer);

    if (localplayer == 0) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find local player, cannot call PostInitProperties!");
        return;
    }

    g_hook->post_init_properties(localplayer);
}

void FFakeStereoRenderingHook::post_init_properties(uintptr_t localplayer) {
    SPDLOG_INFO("Searching for PostInitProperties virtual function...");

    std::optional<uint32_t> idx{};
    const auto engine = sdk::UEngine::get_lvalue();

    if (engine == nullptr) {
        SPDLOG_ERROR("Cannot proceed without engine!");
        return;
    }

    const auto vtable = *(uintptr_t**)localplayer;

    if (vtable == nullptr || IsBadReadPtr((void*)vtable, sizeof(void*))) {
        SPDLOG_ERROR("Cannot proceed, vtable for so-called \"local player\" is invalid!");
        return;
    }

    INSTRUX ix{};

    for (auto i = 1; i < 25; ++i) {
        if (idx) {
            break;
        }

        SPDLOG_INFO("Analyzing index {}...", i);

        const auto vfunc = vtable[i];

        if (vfunc == 0 || IsBadReadPtr((void*)vfunc, 1)) {
            SPDLOG_ERROR("Encountered invalid vfunc at index {}!", i);
            break;
        }

        SPDLOG_INFO("Scanning vfunc at index {} ({:x})...", i, vfunc);

        utility::exhaustive_decode((uint8_t*)vfunc, 25, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (idx) {
                return utility::ExhaustionResult::BREAK;
            }

            if (const auto disp = utility::resolve_displacement(ip); disp) {
                // the second expression catches UE dynamic/debug builds
                if (*disp == (uintptr_t)engine || 
                    (!IsBadReadPtr((void*)*disp, sizeof(void*)) && *(uintptr_t*)*disp == (uintptr_t)*engine)) 
                {
                    SPDLOG_INFO("Found PostInitProperties at {} {:x}!", i, (uintptr_t)vfunc);
                    idx = i;
                    return utility::ExhaustionResult::BREAK;
                }
            }

            return utility::ExhaustionResult::CONTINUE;
        });
    }

    if (!idx) {
        SPDLOG_ERROR("Failed to find PostInitProperties virtual function! A crash may occur!");
    }

    // Now call PostInitProperties.
    // The purpose of this is setting up the view for the other eye.
    // Just creating the StereoRenderingDevice does not automatically do it, so we have to do it manually.
    // Usually the game just calls this function near startup after calling InitializeHMDDevice.
    if (idx) {
        SPDLOG_INFO("Calling PostInitProperties on local player!");

        // Get PEB and set debugger present
        auto peb = (PEB*)__readgsqword(0x60);

        const auto old = peb->BeingDebugged;
        peb->BeingDebugged = true;

        // If the exception count exceeds a certain amount, we need to un-nop the function call because it was supposed to return a pointer.
        static auto exception_count = 0;
        static std::vector<Patch::Ptr> patches{};
        static std::vector<uintptr_t> patch_locations{};

        static std::vector<Patch::Ptr> assert_patches{};
        const void (*post_init_properties)(uintptr_t) = (*(decltype(post_init_properties)**)localplayer)[*idx];

        // Scan through all of the branches of PostInitProperties to find any assertions
        // The assertion we're looking for is easily identified by a string that it loads in RCX, named "!Reference"
        // If we dont do this, there's a possibility that the game will crash at some point or cause some sort of corruption
        utility::exhaustive_decode((uint8_t*)post_init_properties, 100, [](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (ix.Operands[1].Type == ND_OP_MEM) {
                const auto referenced_addr = utility::resolve_displacement(ip);

                if (referenced_addr) try {
                    if (std::string_view{(const char*)*referenced_addr}.starts_with("!Reference")) {
                        // Scan forward and patch out the first call or jmp we run into
                        utility::exhaustive_decode((uint8_t*)ip, 10, [](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
                            if (*(uint8_t*)ip == 0xE8) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                assert_patches.push_back(Patch::create(ip, { 0x90, 0x90, 0x90, 0x90, 0x90 }));
                                return utility::ExhaustionResult::BREAK;
                            }

                            if (*(uint8_t*)ip == 0xE9) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                assert_patches.push_back(Patch::create(ip, { 0xC3 }));
                                return utility::ExhaustionResult::BREAK;
                            }

                            if (std::string_view{ix.Mnemonic}.starts_with("CALL")) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                std::vector<int16_t> nop{};
                                for (auto i = 0; i < ix.Length; ++i) {
                                    nop.push_back(0x90);
                                }

                                assert_patches.push_back(Patch::create(ip, nop));
                                return utility::ExhaustionResult::BREAK;
                            }

                            return utility::ExhaustionResult::CONTINUE;
                        });
                    }
                } catch(...) {

                }
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        // set up a handler to skip int3 assertions
        // we do this because debug builds assert when the views are already setup.
        const auto seh_handler = [](PEXCEPTION_POINTERS info) -> LONG {
            ++exception_count;

            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
                SPDLOG_INFO("Skipping int3 breakpoint at {:x}!", info->ContextRecord->Rip);
                const auto insn = utility::decode_one((uint8_t*)info->ContextRecord->Rip);

                if (insn) {
                    spdlog::info("Skipping {} bytes!", insn->Length);
                    info->ContextRecord->Rip += insn->Length;

                    // Nop out the next function call.
                    // It logs and does some other stuff and causes a crash later on.
                    // To be seen if this will cause any issues, does not appear to (on 4.9 debug builds)
                    const auto call = utility::scan_disasm((uintptr_t)info->ContextRecord->Rip, 20, "E8 ? ? ? ?");

                    if (call) {
                        patch_locations.push_back(*call);
                        patches.emplace_back(Patch::create(*call, {0x90, 0x90, 0x90, 0x90, 0x90}));
                    }

                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            SPDLOG_INFO("Encountered exception {:x} at {:x}!", info->ExceptionRecord->ExceptionCode, info->ContextRecord->Rip);

            // This happens if we removed a call that shouldn't have been removed.
            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !patches.empty()) {
                SPDLOG_WARN("Access violation at {:x}! Removing patch at {:x}!", info->ContextRecord->Rip, patch_locations.back());

                exception_count = 0;
                info->ContextRecord->Rip = patch_locations.back();
                patches.pop_back();
                patch_locations.pop_back();
            } else {
                const auto insn = utility::decode_one((uint8_t*)info->ContextRecord->Rip);

                if (insn) {
                    info->ContextRecord->Rip += insn->Length;
                } else {
                    info->ContextRecord->Rip += 1;
                }
            }

            // yolo? idk xd
            return EXCEPTION_CONTINUE_EXECUTION;
        };

        const auto exception_handler = AddVectoredExceptionHandler(1, seh_handler);

        m_sceneview_data.inside_post_init_properties = true;
        post_init_properties(localplayer);
        m_sceneview_data.inside_post_init_properties = false;

        SPDLOG_INFO("PostInitProperties called!");

        // remove the handler
        RemoveVectoredExceptionHandler(exception_handler);
        peb->BeingDebugged = old;
    }

    g_hook->m_sceneview_data.known_scene_states.clear();
    g_hook->m_fixed_localplayer_view_count = true;
}

void* FFakeStereoRenderingHook::slate_draw_window_render_thread(void* renderer, void* a2, void* a3, 
                                                                void* a4, void* params, void* unk1, void* unk2) 
{
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("SlateRHIRenderer::DrawWindow_RenderThread called!");
#else
    SPDLOG_INFO_ONCE("SlateRHIRenderer::DrawWindow_RenderThread called!");
#endif

    if (!g_framework->is_game_data_intialized() || a2 == nullptr) {
        return g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);
    }

    auto viewport_info = (sdk::FViewportInfo*)a3;
    sdk::ISlateViewport* slate_viewport = nullptr; // UE5.5+
    void** a4_ptr = (void**)a4;

    static bool a4_is_ue_5_5_variant = [&]() -> bool {
        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Checking if a4 is UE 5.5 variant...");

        __try {
            if (a4_ptr[0] == renderer) {
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] a4 is UE 5.5 variant!");
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            SPDLOG_WARN("Exception occurred while checking if a4 is UE 5.5 variant!");
        }

        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] a4 is not UE 5.5 variant!");

        return false;
    }();

    if (!a4_is_ue_5_5_variant) {
        // How are we going to fix this on UE5.5?
        g_hook->get_slate_thread_worker()->execute((FRHICommandListImmediate*)a2);
    } else {
        const auto window = (uintptr_t)a4_ptr[2];

        static std::optional<size_t> viewport_offset = [&]() -> std::optional<size_t> {
            std::optional<size_t> result{};
            const auto module_within = utility::get_module_within(g_hook->m_slate_thread_hook.target_address());

            // Temporarily unhook the DrawWindow_RenderThread hook because we need to emulate the function
            // We could use the trampoline but bdshemu is picky about whether RIP is
            // within the "shellcode" or not (e.g. within the module bounds)
            // and so, the hook must be temporarily unhooked
            if (!module_within) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to get module within for target address!");
                return result;
            }

            SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Module within: {:x}", (uintptr_t)*module_within);

            if (!g_hook->m_slate_thread_hook.disable().has_value()) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to disable slate thread hook!");
                return result;
            }

            utility::ScopeGuard guard{[&]() {
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Re-enabling slate thread hook!");
                if (!g_hook->m_slate_thread_hook.enable().has_value()) {
                    SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to re-enable slate thread hook!");
                }
            }};

            utility::ShemuContext ctx{*module_within};
            ctx.ctx->Registers.RegRip = (ND_UINT64)g_hook->m_slate_thread_hook.target_address();
            ctx.ctx->Registers.RegRcx = (ND_UINT64)renderer;
            ctx.ctx->Registers.RegRdx = (ND_UINT64)a2;
            ctx.ctx->Registers.RegR8 = (ND_UINT64)a3;
            ctx.ctx->Registers.RegR9 = (ND_UINT64)a4;
            ctx.ctx->MemThreshold = 1000;

            uint32_t window_getter_callstack_level = 0;
            std::span<uint8_t> window_bounds{(uint8_t*)window, (uint8_t*)window + 0x1000};

            utility::emulate(*module_within, ctx.ctx->Registers.RegRip, 1000, ctx, [&](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Emulating instruction: {:x} ({:X})", ctx.ctx->ctx->Registers.RegRip, ctx.ctx->ctx->Registers.RegRip - (uintptr_t)*module_within);

                // Allow writes to go through if we are inside the window getter.
                // The downside is this might unintentionally increase the reference count of the window
                // but it's necessary for the window getter to not give us a nullptr.
                if (ctx.next.writes_to_memory && window_getter_callstack_level == 0) {
                    return utility::ExhaustionResult::STEP_OVER;
                }

                if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                    if (window_getter_callstack_level > 0) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing call inside window getter function, continuing!");
                        ++window_getter_callstack_level;
                        return utility::ExhaustionResult::CONTINUE;
                    }

                    // Check if RCX != window first. We don't want to skip over the call if it is set to it.
                    // There are inlined and non-inlined versions of this function which is why we need to check this.
                    if ((uint8_t*)ctx.ctx->ctx->Registers.RegRcx < window_bounds.data() || (uint8_t*)ctx.ctx->ctx->Registers.RegRcx > window_bounds.data() + window_bounds.size()) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping call!");
                        return utility::ExhaustionResult::STEP_OVER;
                    }

                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing call, RCX matches window {:x}!", ctx.next.ix.Operands[0].Info.Register.Reg, window);
                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RCX: {:x}, RDX: {:x}", ctx.ctx->ctx->Registers.RegRcx, ctx.ctx->ctx->Registers.RegRdx);
                    ++window_getter_callstack_level;
                    return utility::ExhaustionResult::CONTINUE;
                }

                // Check if we hit a ret and are inside the window getter function.
                if (ctx.next.ix.Instruction == ND_INS_RETN) {
                    if (window_getter_callstack_level > 0) { 
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Hit ret inside window getter function, continuing!");
                        --window_getter_callstack_level;
                        return utility::ExhaustionResult::CONTINUE;
                    }
                }

                // We're looking for a mov reg, [reg+offset] instruction
                // where reg contains the pointer to the window
                // and offset is the offset to the viewport.
                const auto& cctx = ctx.ctx->ctx;
                const auto& ix = cctx->Instruction;

                if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_REG && ix.Operands[1].Type == ND_OP_MEM &&
                    ix.Operands[1].Info.Memory.HasBase && ix.Operands[1].Info.Memory.HasDisp)
                {
                    uintptr_t* reg = (uintptr_t*)&((uint64_t*)&cctx->Registers.RegRax)[ix.Operands[1].Info.Memory.Base];

                    // Instead of checking the window, we check if the register is within the bounds of the window's memory.
                    // This should allow us to catch all sorts of compiler optimizations.
                    if ((uint8_t*)*reg >= window_bounds.data() && (uint8_t*)*reg < window_bounds.data() + window_bounds.size()) try {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found window pointer at {:x}!", (uintptr_t)reg);
                        auto offset = ix.Operands[1].Info.Memory.Disp;
                        const auto value = *(uintptr_t***)((uintptr_t)*reg + offset);

                        if (value == nullptr || IsBadReadPtr((void*)value, sizeof(void*))) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid offset at {:x}!", (uintptr_t)value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        if (*value == nullptr || IsBadReadPtr((void*)*value, sizeof(void*))) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid vtable at {:x}!", (uintptr_t)*value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        if (!utility::get_module_within(*value).has_value() || !utility::get_module_within((*value)[0]).has_value()) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid module at {:x}!", (uintptr_t)*value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        const auto behind_value = *(uintptr_t***)((uintptr_t)*reg + offset - sizeof(void*));

                        if (behind_value != nullptr && !IsBadReadPtr((void*)behind_value, sizeof(void*)) &&
                            *behind_value != nullptr && !IsBadReadPtr((void*)*behind_value, sizeof(void*)) &&
                            utility::get_module_within(*behind_value).has_value() && utility::get_module_within((*behind_value)[0]).has_value())
                        {
                            SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Adjusting offset by sizeof(void*)!");
                            offset -= sizeof(void*);
                        }

                        result = (*reg + offset) - (uintptr_t)window;

                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found viewport offset at {:x}!", *result);
                        return utility::ExhaustionResult::BREAK;
                    } catch (...) {
                        SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Exception while checking offset!");
                    }
                }

                return utility::ExhaustionResult::CONTINUE;
            });

            if (!result) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to find viewport offset!");
            }

            return result;
        }();

        if (viewport_offset) {
            slate_viewport = *(sdk::ISlateViewport**)((uintptr_t)window + *viewport_offset);
        }
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_pre_slate_draw_window(renderer, a2, viewport_info);
    }

    g_hook->m_inside_slate_draw_window = true;
    g_hook->m_slate_draw_window_thread_id = GetCurrentThreadId();

    auto call_orig = [&]() {
        auto ret = g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);

        for (auto& mod : mods) {
            mod->on_post_slate_draw_window(renderer, a2, viewport_info);
        }

        g_hook->m_inside_slate_draw_window = false;

        return ret;
    };


    auto vr = VR::get();

    if (!vr->is_hmd_active() || vr->is_stereo_emulation_enabled()) {
        return call_orig();
    }

    const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();

    // ui_target can be non-null but dangling if a texture recreation (e.g. submenu open/close,
    // resolution change) raced with this call. We're about to hand it straight to the engine's
    // Slate renderer (slate_resource->get_mutable_resource() = ui_target), which dereferences it
    // during DrawWindow_RenderThread, so validate readability first, matching the guard pattern
    // used elsewhere in this file for ui_target accesses.
    if (ui_target == nullptr || IsBadReadPtr(ui_target, 0x60)) {
        SPDLOG_INFO_EVERY_N_SEC(1, "No UI target (or ui_target invalid), skipping!");
        return call_orig();
    }

    sdk::FSlateResource* slate_resource = nullptr;

    if (slate_viewport != nullptr) {
        slate_resource = slate_viewport->GetViewportRenderTargetTexture();
    } else {
        const auto viewport_rt_provider = viewport_info->get_rt_provider(g_hook->get_render_target_manager()->get_render_target());

        if (viewport_rt_provider == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1, "No viewport RT provider, skipping!");
            return call_orig();
        }
    
        slate_resource = viewport_rt_provider->get_viewport_render_target_texture();
    }

    if (slate_resource == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1, "No slate resource, skipping!");
        return call_orig();
    }
    
    // Replace the texture with one we have control over.
    // This isolates the UI to render on our own texture separate from the scene.
    const auto old_texture = slate_resource->get_mutable_resource();
    slate_resource->get_mutable_resource() = ui_target;

    // To be seen if we need to resort to a MidHook on this function if the parameters
    // are wildly different between UE versions.
    const auto ret = g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);

    // Restore the old texture.
    slate_resource->get_mutable_resource() = old_texture;

    for (auto& mod : mods) {
        mod->on_post_slate_draw_window(renderer, a2, viewport_info);
    }
    
    // After this we copy over the texture and clear it in the present hook. doing it here just seems to crash sometimes.
    SPDLOG_INFO_ONCE("SlateRHIRenderer::DrawWindow_RenderThread finished!");

    return ret;
}

// INTERNAL USE ONLY!!!!
__declspec(noinline) void VRRenderTargetManager::CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::CalculateRenderTargetSize called!");

    m_last_calculate_render_size_return_address = (uintptr_t)_ReturnAddress();

    VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
}

__declspec(noinline) bool VRRenderTargetManager::NeedReAllocateDepthTexture(const void* DepthTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::NeedReAllocateDepthTexture called!");

    m_last_needs_reallocate_depth_texture_return_address = (uintptr_t)_ReturnAddress();

    if (this->depth_analysis_passed) {
        return VRRenderTargetManager_Base::need_reallocate_depth_texture(DepthTarget);
    }

    return false;
}

__declspec(noinline) bool VRRenderTargetManager::NeedReAllocateShadingRateTexture(const void* ShadingRateTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::NeedReAllocateShadingRateTexture called!");

    const auto return_address = (uintptr_t)_ReturnAddress();
    const auto diff = return_address - m_last_calculate_render_size_return_address;

    if (diff <= 0x50) {
        // We need to switch the FFakeStereoRenderingHook's render target manager
        // to the old one NOW or we will crash. Reason being what was actually called
        // is the GetNumberOfBufferedFrames function, not NeedReAllocateShadingRateTexture.
        SPDLOG_INFO("Switching to old render target manager! Incorrect function called!");
        //g_hook->switch_to_old_rendertarget_manager();

        // Do a switcharoo on the vtable of this object to the old one because we will crash if we don't.
        // I've decided against actually switching the entire object over in favor of just vtable
        // swapping for now even though it's kind of a hack.
        const auto fake_object = std::make_unique<VRRenderTargetManager_418>();
        *(void**)this = *(void**)fake_object.get();

        return true; // The return value should actually be 1, so just return true.
    }

    return false;
}

void VRRenderTargetManager_Base::update_viewport(bool use_separate_rt, const sdk::FViewport& vp, class SViewport* vp_widget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::update_viewport called! {} {:x} {:x}", use_separate_rt, (uintptr_t)&vp, (uintptr_t)vp_widget);

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    //SPDLOG_INFO("Widget: {:x}", (uintptr_t)ViewportWidget);
}

void VRRenderTargetManager_Base::calculate_render_target_size(const sdk::FViewport& viewport, uint32_t& x, uint32_t& y) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::calculate_render_target_size called!");

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate render target size called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    SPDLOG_INFO("RenderTargetSize Before: {}x{}", x, y);

    x = VR::get()->get_hmd_width() * 2;
    y = VR::get()->get_hmd_height();

    SPDLOG_INFO("RenderTargetSize After: {}x{}", x, y);
}

bool VRRenderTargetManager_Base::need_reallocate_view_target(const sdk::FViewport& Viewport) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::need_reallocate_view_target called!");

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (!m_attempted_find_force_separate_rt) try {
            m_attempted_find_force_separate_rt = true;

            // Go up the stack until we find something that isn't in our module.
            const auto our_module = g_framework->get_framework_module();
            constexpr auto max_stack_depth = 100;
            uintptr_t stack[max_stack_depth]{};

            const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

            std::optional<uintptr_t> ret_addr{};
            std::optional<HMODULE> module_within{};

            for (auto i = 0; i < depth; ++i) {
                SPDLOG_INFO("Stack[{}]: {:x}", i, stack[i]);

                module_within = utility::get_module_within(stack[i]);

                if (!module_within) {
                    continue;
                }

                if (*module_within != our_module) {
                    ret_addr = stack[i];
                    break;
                }
            }

            // Emulate from the return address and find a memory write
            // this should contain the offset to the force separate rt bool.
            if (ret_addr) {
                SPDLOG_INFO("Found return address: {:x}", *ret_addr);

                utility::ShemuContext ctx{*module_within};
                ctx.ctx->Registers.RegRip = *ret_addr;
                ctx.ctx->Registers.RegRax = 1; // As if we're returning true from this function.

            utility::emulate(*module_within, *ret_addr, 100, ctx, [this](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                        SPDLOG_INFO("Emulating instruction: {:x}", ctx.ctx->ctx->Registers.RegRip);

                        if (ctx.next.writes_to_memory) {
                            const auto& ix = ctx.next.ix;
                            if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_MEM && ix.Operands[1].Type == ND_OP_REG) {
                                // We're looking for a mov [reg1+N], reg2
                                const auto& op0 = ix.Operands[0];

                                // Needs a register
                                if (!op0.Info.Memory.HasBase || op0.Info.Memory.IsRipRel) {
                                    return utility::ExhaustionResult::STEP_OVER;
                                }

                                // Needs a displacement
                                if (!op0.Info.Memory.HasDisp) {
                                    return utility::ExhaustionResult::STEP_OVER;
                                }

                                // We don't want a stack based register
                                if (op0.Info.Memory.Base == NDR_RSP || op0.Info.Memory.Base == NDR_RBP) {
                                    return utility::ExhaustionResult::STEP_OVER;
                                }

                                if (op0.Info.Memory.Disp > 0 && op0.Info.Memory.Disp < 0x2000) {
                                    m_viewport_force_separate_rt_offset = op0.Info.Memory.Disp;
                                    SPDLOG_INFO("Found force separate rt offset: {:x}", *m_viewport_force_separate_rt_offset);
                                    return utility::ExhaustionResult::BREAK;
                                }
                            }

                            SPDLOG_INFO("Stepping over...");

                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                            // We need to break out of this, we should've found the offset before the call.
                    SPDLOG_ERROR("Failed to find force separate rt offset! Encountered call at {:x}", ctx.ctx->ctx->Registers.RegRip);
                            return utility::ExhaustionResult::BREAK;
                        }

                        return utility::ExhaustionResult::CONTINUE;
                    });
            }
    } catch(...) { // if we dont find it, it's fine, not very many games require it.
            SPDLOG_ERROR("Failed to find force separate rt offset! (Exception)");
        }

    const auto w = VR::get()->get_hmd_width();
    const auto h = VR::get()->get_hmd_height();

    // In 2D screen mode, get_hmd_width()/height() derive from the game's own render target size
    // (Framework::get_rt_size()), which can fluctuate frame-to-frame (e.g. dynamic resolution
    // scaling during a loading screen, or the backbuffer briefly resizing). Reacting to every
    // transient change here triggers a full, expensive backbuffer + OpenXR swapchain teardown/
    // recreate cascade (observed costing several hundred ms to ~1-2s each time), which can repeat
    // continuously and starve the loading process of the cycles it needs to finish, manifesting
    // as the game appearing stuck on its loading screen even though rendering itself is healthy.
    // Debounce by requiring the new size to be stable across several consecutive checks before
    // committing to a reallocation, unless a reallocation is forced via should_recreate_textures().
    const auto forced = g_hook->should_recreate_textures();

    if (!forced && (w != this->last_width || h != this->last_height)) {
        if (w != m_pending_width || h != m_pending_height) {
            m_pending_width = w;
            m_pending_height = h;
            m_pending_size_stable_count = 1;
        } else {
            ++m_pending_size_stable_count;
        }

        static constexpr uint32_t required_stable_checks = 5;

        if (m_pending_size_stable_count < required_stable_checks) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Deferring view target reallocation ({} {} -> {} {}), waiting for size to stabilize ({}/{})",
                this->last_width, this->last_height, w, h, m_pending_size_stable_count, required_stable_checks);
            return false;
        }
    }

    if (forced || w != this->last_width || h != this->last_height) {
        const auto new_generation = g_hook->bump_view_target_generation();
        SPDLOG_INFO("Reallocating view target! {} {} -> {} {} (generation -> {})", this->last_width, this->last_height, w, h, new_generation);

        this->last_width = w;
        this->last_height = h;
        m_pending_size_stable_count = 0;
        this->wants_depth_reallocate = true;
        this->destroy_scene_capture();
        g_hook->set_should_recreate_textures(false);
        return true;
    }

    return false;
}

bool VRRenderTargetManager_Base::need_reallocate_depth_texture(const void* DepthTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::need_reallocate_depth_texture called!");

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (this->wants_depth_reallocate) {
        SPDLOG_INFO("Reallocating depth texture!");

        this->wants_depth_reallocate = false;
        return true;
    }

    return false;
}

FFakeStereoRenderingHook::UIDrawExtent FFakeStereoRenderingHook::get_ui_target_size() {
    // Default to the shared scene RT size so behavior is unchanged if VR is unavailable.
    const auto rt = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
    int32_t w = (int32_t)rt.x;
    int32_t h = (int32_t)rt.y;

    auto vr = VR::get();
    const bool tall_ui_enabled = vr != nullptr && vr->is_native_stereo_fix_tall_ui_enabled();
    bool grew_for_tall_ui = false;

    if (tall_ui_enabled) {
        // LGUI lays its canvas out at the per-eye view rect (hmd_width x hmd_height). When NSF is on this per-eye
        // height (e.g. 3377) exceeds the scene RT height (2160), so the bottom of the UI is clipped. Grow the UI
        // target to hold the full canvas; it is cropped/stretched back to 16:9 at presentation. Gated to NSF ON
        // via the toggle - in AFR / NSF OFF the canvas is not tall, so growing the target would distort the UI.
        const int32_t canvas_w = (int32_t)vr->get_hmd_width();
        const int32_t canvas_h = (int32_t)vr->get_hmd_height();

        if (canvas_w > 0 && canvas_h > 0) {
            w = std::max(w, canvas_w);
            h = std::max(h, canvas_h);
            grew_for_tall_ui = w != (int32_t)rt.x || h != (int32_t)rt.y;
        }
    }

    // DIAG: log the resolved UI swapchain size any time the inputs to it change (mode/toggle/resolution),
    // so we can confirm exactly which VR mode(s) are getting an oversized (tall-UI) UI swapchain without
    // needing verbose per-frame logging. Cheap: only compares a handful of cached scalars per call.
    {
        static int32_t s_last_w = -1, s_last_h = -1;
        static bool s_last_tall_ui_enabled = false;
        static bool s_last_grew = false;
        static bool s_last_nsf = false;
        static bool s_last_afr = false;
        static bool s_have_last = false;

        const bool nsf_now = vr != nullptr && vr->is_native_stereo_fix_enabled();
        const bool afr_now = vr != nullptr && vr->is_using_afr();

        const bool changed = !s_have_last || w != s_last_w || h != s_last_h ||
            tall_ui_enabled != s_last_tall_ui_enabled || grew_for_tall_ui != s_last_grew ||
            nsf_now != s_last_nsf || afr_now != s_last_afr;

        if (changed) {
            s_have_last = true;
            s_last_w = w;
            s_last_h = h;
            s_last_tall_ui_enabled = tall_ui_enabled;
            s_last_grew = grew_for_tall_ui;
            s_last_nsf = nsf_now;
            s_last_afr = afr_now;

            SPDLOG_INFO("[UI_TARGET_SIZE] resolved={}x{} (base_rt={}x{}) tall_ui_enabled={} grew={} nsf={} afr={}",
                w, h, (int32_t)rt.x, (int32_t)rt.y, tall_ui_enabled, grew_for_tall_ui, nsf_now, afr_now);
        }
    }

    return UIDrawExtent{w, h};
}

void VRRenderTargetManager_Base::pre_texture_hook_callback(safetyhook::Context& ctx, bool from_second) {
    SPDLOG_INFO("PreTextureHook called! {}", ctx.r8);

    // maybe do some work later to bruteforce the registers/offsets for these
    // a la emulation or something more rudimentary
    // since it always seems to access a global right before, which
    // refers to the current pixel format, which we can overwrite (which may not be safe)
    // so we could just follow how the global is being written to registers or the stack
    // and then just overwrite the registers/stack with our own values
    auto rtm = g_hook->get_render_target_manager();

    if (!rtm->allocate_texture_called) {
        SPDLOG_ERROR("AllocateTexture not called yet! (PreTextureHook)");
        return;
    }

    if (!g_hook->has_pixel_format_cvar()) {
        if (g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
            //ctx.r8 = 2; // PF_B8G8R8A8 // decided not to actually set it here, we need to double check when it's actually called
        } else if (!rtm->is_using_texture_desc) {
            *((uint8_t*)ctx.rsp + 0x28) = 2; // PF_B8G8R8A8
        }
    }

    // Now we are going to attempt to JIT a function that will call the original function
    // using the context we have. This will call it twice, but allow us to
    // have control over one of the textures it generates. We need
    // the other generated texture as a UI render target to be used in FFakeStereoRenderingHook::slate_draw_window_render_thread.
    // This will allow the original game UI to be rendered in world space without resorting to WidgetComponent.
    // One can argue that this may be an overengineered alternative to "just" calling FDynamicRHI::CreateTexture2D
    // but that function is very hard to pattern scan for, and we already have it here, so why not use it?
    using namespace asmjit;
    using namespace asmjit::x86;

    SPDLOG_INFO("Attempting to JIT a function to call the original function!");

    auto& insn_bytes = !from_second ? rtm->texture_create_insn_bytes : rtm->texture_create_insn_bytes2;

    const auto ix = utility::decode_one(insn_bytes.data(), insn_bytes.size());

    if (!ix) {
        SPDLOG_ERROR("Failed to decode instruction!");
        return;
    }
    
    // We can't do it to the normal E8 call because the code is not in the same area
    // so RIP relative calls are not possible through the emulator. will just have to
    // resolve those manually through disassembly.
    uintptr_t func_ptr = 0;

    if (!g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
        // Set up the emulator. We will use it to emulate the function call.
        // All we need from it is where the function call lands, so we can call it for real.
        auto emu_ctx = utility::ShemuContext(
            (uintptr_t)insn_bytes.data(),
            insn_bytes.size());

        SPDLOG_INFO("Insn bytes size: {}", insn_bytes.size());
        for (size_t i = 0; i < insn_bytes.size(); ++i) {
            SPDLOG_INFO("Byte[{}]: {:x}", i, insn_bytes[i]);
        }

        emu_ctx.ctx->Registers.RegRcx = ctx.rcx;
        emu_ctx.ctx->Registers.RegRdx = ctx.rdx;
        emu_ctx.ctx->Registers.RegR8 = ctx.r8;
        emu_ctx.ctx->Registers.RegR9 = ctx.r9;
        emu_ctx.ctx->Registers.RegRbx = ctx.rbx;
        emu_ctx.ctx->Registers.RegRax = ctx.rax;
        emu_ctx.ctx->Registers.RegRdi = ctx.rdi;
        emu_ctx.ctx->Registers.RegRsi = ctx.rsi;
        emu_ctx.ctx->Registers.RegR10 = ctx.r10;
        emu_ctx.ctx->Registers.RegR11 = ctx.r11;
        emu_ctx.ctx->Registers.RegR12 = ctx.r12;
        emu_ctx.ctx->Registers.RegR13 = ctx.r13;
        emu_ctx.ctx->Registers.RegR14 = ctx.r14;
        emu_ctx.ctx->Registers.RegR15 = ctx.r15;

        // if disasm is call [rsp+N] we need to set RSP to the actual stack
        // otherwise emulation will fail.
        // conversely, if we set RSP when it's NOT using RSP in the register
        // it will also fail.
        if (ix->Operands[0].Type == ND_OP_MEM && ix->Operands[0].Info.Memory.HasBase &&
            ix->Operands[0].Info.Memory.Base == NDR_RSP)
        {
            emu_ctx.ctx->Registers.RegRsp = ctx.rsp;
            emu_ctx.ctx->Stack = (ND_UINT8*)ctx.rsp;
            emu_ctx.ctx->StackBase = ctx.rsp;
            SPDLOG_INFO("Setting RSP to {:x} for emulation!", ctx.rsp);
        } else {
            SPDLOG_INFO("Not setting RSP for emulation!");
        }

        emu_ctx.ctx->MemThreshold = 1;

        if (emu_ctx.emulate((uintptr_t)insn_bytes.data(), 1) != SHEMU_SUCCESS) {
            SPDLOG_ERROR("Failed to emulate instruction!: {} RIP: {:x}", emu_ctx.status, emu_ctx.ctx->Registers.RegRip);
            return;
        }
    
        SPDLOG_INFO("Emu landed at {:x}", emu_ctx.ctx->Registers.RegRip);
        func_ptr = emu_ctx.ctx->Registers.RegRip;

        if (func_ptr == 0) {
            SPDLOG_ERROR("Function pointer is null after emulation!");
            return;
        }
    } else {
        const auto target = g_hook->get_render_target_manager()->pre_texture_hook.target_address();
        func_ptr = target + 5 + *(int32_t*)&insn_bytes.data()[1];
    }

    SPDLOG_INFO("Function pointer: {:x}", func_ptr);

    /*CodeHolder code{};
    JitRuntime runtime{};
    code.init(runtime.environment());

    Assembler a{&code};
    
    static auto cloned_stack = std::make_unique<std::array<uint8_t, 0x3000>>();
    static auto cloned_registers = std::make_unique<std::array<uint8_t, 0x1000>>();

    auto aligned_stack = ((uintptr_t)&(*cloned_stack)[0x2000]);
    aligned_stack += (-(intptr_t)aligned_stack) & (40 - 1);

    memcpy((void*)aligned_stack, (void*)(ctx.rsp), 0x1000);

    static auto stack_ptr = std::make_unique<uintptr_t>();
    static auto post_register_storage = std::make_unique<uintptr_t>();

    // Store the original stack pointer.
    a.movabs(rax, (void*)stack_ptr.get());
    a.mov(ptr(rax), rsp);

    // Push all of the original registers onto the stack.
    a.movabs(rsp, (void*)&(*cloned_registers)[0x500]);
    //a.mov(rsp, rax);

    a.push(rcx);
    a.push(rdx);
    a.push(r8);
    a.push(r9);
    a.push(r10);
    a.push(r11);
    a.push(r12);
    a.push(r13);
    a.push(r14);
    a.push(r15);
    a.push(rbx);
    a.push(rbp);
    a.push(rsi);
    a.push(rdi);
    a.pushfq();

    a.mov(rax, (void*)post_register_storage.get());
    a.mov(ptr(rax), rsp);

    a.movabs(rsp, aligned_stack);


    a.mov(rdx, rcx); // func param
    a.movabs(rcx, ctx.rcx);
    //a.movabs(rdx, ctx.rdx);
    a.movabs(r8, ctx.r8);
    //a.movabs(r9, ctx.r9);
    const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
    a.mov(r9, (uint32_t)size.x);
    // move w into first stack argument
    a.mov(dword_ptr(rsp, 0x20), (uint32_t)size.y);
    a.movabs(r10, ctx.r10);
    a.movabs(r11, ctx.r11);
    a.movabs(r12, ctx.r12);
    a.movabs(r13, ctx.r13);
    a.movabs(r14, ctx.r14);
    a.movabs(r15, ctx.r15);
    a.movabs(rax, ctx.rax);
    a.movabs(rbx, ctx.rbx);
    a.movabs(rbp, ctx.rbp);
    a.movabs(rsi, ctx.rsi);
    a.movabs(rdi, ctx.rdi);

    // Correct the stack pointers inside the stack we cloned
    // to point to areas within the cloned stack if they were
    // pointing to the original stack.
    for (auto stack_var = 0; stack_var < 0x1000; stack_var += sizeof(void*)) {
        auto stack_var_ptr = (uintptr_t*)(aligned_stack + stack_var);

        if (*stack_var_ptr >= ctx.rsp && *stack_var_ptr < ctx.rsp + 0x1000) {
            SPDLOG_INFO("Correcting stack var at 0x{:x}", stack_var);
            *stack_var_ptr = aligned_stack + (*stack_var_ptr - ctx.rsp);
        }
    }

    auto correct_register = [&](auto& reg) {
        if (reg >= ctx.rsp && reg < ctx.rsp + 0x1000) {
            SPDLOG_INFO("Correcting Register");
            reg = aligned_stack + (reg - ctx.rsp);
        }

    };
    for (auto insn_byte : g_hook->get_render_target_manager()->texture_create_insn_bytes) {
        a.db(insn_byte);
    }

    a.mov(rsp, post_register_storage.get());
    a.mov(rsp, ptr(rsp));
    //a.mov(rsp, rcx);

    // Pop all of the original registers off of the stack.
    a.popfq();
    a.pop(rdi);
    a.pop(rsi);
    a.pop(rbp);
    a.pop(rbx);
    a.pop(r15);
    a.pop(r14);
    a.pop(r13);
    a.pop(r12);
    a.pop(r11);
    a.pop(r10);
    a.pop(r9);
    a.pop(r8);
    a.pop(rdx);
    a.pop(rcx);

    //a.pop(rsp); // Restore the original stack pointer.
    a.movabs(rsp, (void*)stack_ptr.get());
    a.mov(rsp, ptr(rsp));

    a.ret();

    uintptr_t code_addr{};
    runtime.add(&code_addr, &code);

    SPDLOG_INFO("JITed address: {:x}", code_addr);

    //MessageBox(0, "debug now", "debug", 0);

    void (*func)(void* rdx) = (decltype(func))code_addr;

    static FTexture2DRHIRef out{};
    out.texture = nullptr;
    func(&out);*/

    auto call_with_context = [&](uintptr_t func, FTexture2DRHIRef& out) {
        CodeHolder code{};
        JitRuntime runtime{};
        code.init(runtime.environment());

        Assembler a{&code};

        auto post_align_label = a.newLabel();

        a.push(rbx);

        a.mov(rcx, ctx.rcx);
        
        if (!g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
            a.movabs(rdx, (uintptr_t)&out);
        } else {
            a.mov(rdx, ctx.rdx);
        }

        a.mov(r8, ctx.r8);

        const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
        a.mov(r9, (uint32_t)size.x);

        a.sub(rsp, 0x100);
        a.mov(rbx, 0x100);
        a.test(rsp, sizeof(void*));
        a.jz(post_align_label);

        a.sub(rsp, 8);
        a.mov(rbx, 0x108);
        a.bind(post_align_label);

        a.mov(ptr(rsp, 0x20), (uint32_t)size.y);

        for (auto i = 0x28; i < 0x90; i += sizeof(void*)) {
            a.mov(rax, *(uintptr_t*)(ctx.rsp + i));
            a.mov(ptr(rsp, i), rax);
        }

        a.mov(rax, (void*)func);
        a.call(rax);

        a.add(rsp, rbx);
        a.pop(rbx);

        a.ret();

        uintptr_t code_addr{};
        runtime.add(&code_addr, &code);
        void (*jitted_func)() = (decltype(jitted_func))code_addr;

        jitted_func();
    };

    static FTexture2DRHIRef out{};
    static FTexture2DRHIRef shader_out{};

    // Use the dedicated (taller) UI-target size instead of the shared scene RT size so LGUI's full per-eye canvas
    // fits without clipping the bottom. This callback allocates ONLY the UI target (rtm->ui_target), so the scene
    // RT is unaffected.
    const auto ui_size = FFakeStereoRenderingHook::get_ui_target_size();
    const auto size = Vector2f{(float)ui_size.width, (float)ui_size.height};
    const auto stack_args = (uintptr_t*)(ctx.rsp + 0x20);

    SPDLOG_INFO("About to call the original!");
    
    if (!rtm->is_pre_texture_call_e8) {
        SPDLOG_INFO("Calling register version of texture create");

        if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            if (ctx.r9 == 0 || IsBadReadPtr((void*)ctx.r9, sizeof(void*))) {
                SPDLOG_INFO("Possible UE 5.0.3 detected, not 5.1 or above");
                rtm->is_using_texture_desc = false;
                rtm->is_version_5_0_3 = true;
                rtm->is_version_greq_5_1;
            }
        }

        if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            SPDLOG_INFO("Calling UE5 texture desc version of texture create");

            void (*func)(
                uintptr_t rhi,
                FTexture2DRHIRef* out,
                uintptr_t command_list,
                uintptr_t desc,
                uintptr_t stack_0, // Stack dummies in-case this is the wrong function
                uintptr_t stack_1,
                uintptr_t stack_2,
                uintptr_t stack_3,
                uintptr_t stack_4,
                uintptr_t stack_5,
                uintptr_t stack_6,
                uintptr_t stack_7,
                uintptr_t stack_8) = (decltype(func))func_ptr;

            // Scan for the render target width and height in the desc
            // and replace it with the desktop resolution (This is for the UI texture)
            const auto scan_x = VR::get()->get_hmd_width() * 2;
            const auto scan_y = VR::get()->get_hmd_height();

            std::optional<int32_t> width_offset{};
            std::optional<int32_t> height_offset{};

            int32_t old_width{};
            int32_t old_height{};

            for (auto i = 0; i < 0x100; ++i) {
                auto& x = *(int32_t*)(ctx.r9 + i);
                auto& y = *(int32_t*)(ctx.r9 + i + 4);

                if (x == scan_x && y == scan_y) {
                    SPDLOG_INFO("UE5: Found render target width and height at offset: {:x}", i);

                    width_offset = i;
                    height_offset = i + 4;

                    old_width = x;
                    old_height = y;

                    x = size.x;
                    y = size.y;

                    uint8_t* format = (uint8_t*)(ctx.r9 + width_offset.value() + 15);

                    // some games have 10 bit format
                    if (*format == 18) {
                        *format = 2; // PF_B8G8R8A8
                    }

                    break;
                }
            }

            func(ctx.rcx, &out, ctx.r8, ctx.r9,
                stack_args[0], stack_args[1], 
                stack_args[2], stack_args[3],
                stack_args[4],
                stack_args[5], stack_args[6],
                stack_args[7], stack_args[8]
            );

            if (width_offset && height_offset) {
                auto& x = *(int32_t*)(ctx.r9 + *width_offset);
                auto& y = *(int32_t*)(ctx.r9 + *height_offset);

                x = old_width;
                y = old_height;
            }

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
            }
        } else if (rtm->is_using_texture_desc) { // extremely rare.
            SPDLOG_INFO("Calling UE4 texture desc version of texture create");

            void (*func)(
                uintptr_t rhi,
                uintptr_t desc,
                TRefCountPtr<IPooledRenderTarget>* out,
                uintptr_t name // wchar_t*
            ) = (decltype(func))func_ptr;

            // Scan for the render target width and height in the desc
            // and replace it with the desktop resolution (This is for the UI texture)
            const auto scan_x = VR::get()->get_hmd_width() * 2;
            const auto scan_y = VR::get()->get_hmd_height();

            std::optional<int32_t> width_offset{};
            std::optional<int32_t> height_offset{};

            int32_t old_width{};
            int32_t old_height{};

            for (auto i = 0; i < 0x100; ++i) {
                auto& x = *(int32_t*)(ctx.rdx + i);
                auto& y = *(int32_t*)(ctx.rdx + i + 4);

                if (x == scan_x && y == scan_y) {
                    SPDLOG_INFO("UE4: Found render target width and height at offset: {:x}", i);

                    width_offset = i;
                    height_offset = i + 4;

                    old_width = x;
                    old_height = y;

                    x = size.x;
                    y = size.y;
                    break;
                }
            }

            static TRefCountPtr<IPooledRenderTarget> real_out{};

            func(ctx.rcx, ctx.rdx, &real_out, ctx.r9);

            if (real_out.reference != nullptr) {
                const auto& tex = real_out.reference->item.texture;
                const auto& shader = real_out.reference->item.srt;
                out.texture = tex.texture;
                shader_out.texture = shader.texture;
            }

            if (width_offset && height_offset) {
                auto& x = *(int32_t*)(ctx.rdx + *width_offset);
                auto& y = *(int32_t*)(ctx.rdx + *height_offset);

                x = old_width;
                y = old_height;
            }

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.r8;
            }
        } else { // most common version.
            SPDLOG_INFO("Calling common version of texture create (several arguments)");

            void (*func)(
                uintptr_t rhi,
                FTexture2DRHIRef* out,
                uintptr_t command_list,
                uintptr_t w,
                uintptr_t h,
                uintptr_t format,
                uintptr_t mips,
                uintptr_t samples,
                uintptr_t flags,
                uintptr_t create_info,
                uintptr_t additional,
                uintptr_t additional2) = (decltype(func))func_ptr;

            func(ctx.rcx, &out, ctx.r8, size.x, size.y, 2, 
                stack_args[2], stack_args[3], stack_args[4], 
                stack_args[5], stack_args[6], stack_args[7]);

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
            }
        }

        rtm->ui_target = out.texture;
    } else {
        SPDLOG_INFO("Calling E8 version of texture create");
        
        // check if RCX is near the stack pointer
        // if it is then it's a different form of E8 call that takes the texture in the first parameter.
        if (ctx.rcx != 0 && std::abs((int64_t)ctx.rcx - (int64_t)ctx.rsp) <= 0x300) {
            SPDLOG_INFO("Weird form of E8 call detected...");

            // RDX check is to make sure RDX is a pointer and not something like the width which would be a relatively small integer
            if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1 && ctx.rdx >= 65535) {
                SPDLOG_INFO("Calling UE5 texture desc version of texture create");

                void (*func)(
                    FTexture2DRHIRef* out,
                    uintptr_t desc,
                    uintptr_t r8,
                    uintptr_t r9
                ) = (decltype(func))func_ptr;

                // Scan for the render target width and height in the desc
                // and replace it with the desktop resolution (This is for the UI texture)
                const auto scan_x = VR::get()->get_hmd_width() * 2;
                const auto scan_y = VR::get()->get_hmd_height();

                std::optional<int32_t> width_offset{};
                std::optional<int32_t> height_offset{};

                int32_t old_width{};
                int32_t old_height{};

                for (auto i = 0; i < 0x100; ++i) {
                    auto& x = *(int32_t*)(ctx.rdx + i);
                    auto& y = *(int32_t*)(ctx.rdx + i + 4);

                    if (x == scan_x && y == scan_y) {
                        SPDLOG_INFO("UE5: Found render target width and height at offset: {:x}", i);

                        width_offset = i;
                        height_offset = i + 4;

                        old_width = x;
                        old_height = y;

                        x = size.x;
                        y = size.y;
                        break;
                    }
                }

                func(&out, ctx.rdx, ctx.r8, ctx.r9);

                if (width_offset && height_offset) {
                    auto& x = *(int32_t*)(ctx.rdx + *width_offset);
                    auto& y = *(int32_t*)(ctx.rdx + *height_offset);

                    x = old_width;
                    y = old_height;
                }

                if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                    SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rcx;
                }
            } else {
                // Format
                ctx.r9 = 2; // PF_B8G8R8A8

                void (*func)(
                    FTexture2DRHIRef* out,
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t samples,
                    uintptr_t flags,
                    uintptr_t a7,
                    uintptr_t a8,
                    uintptr_t a9,
                    uintptr_t additional,
                    uintptr_t additional2) = (decltype(func))func_ptr;

                func(&out, (uint32_t)size.x, (uint32_t)size.y, 2,
                    stack_args[0], stack_args[1], 
                    stack_args[2], stack_args[3],
                    stack_args[4],
                    stack_args[7], stack_args[8], stack_args[9]);
            }
        } else {
            ctx.r8 = 2; // PF_B8G8R8A8

            std::optional<int> previous_stack_found_index{};
            std::optional<int> previous_stack_repeating_index{};

            std::optional<int> texture_argument_index{};
            std::optional<int> shader_argument_index{};

            for (auto i = 0; i < 10; ++i) {
                const auto stack_ptr = stack_args[i];

                if (std::abs((int64_t)stack_ptr - (int64_t)ctx.rsp) <= 0x300) {
                    if (previous_stack_found_index && *previous_stack_found_index == i - 1) {
                        previous_stack_repeating_index = i;
                    }

                    previous_stack_found_index = i;
                    SPDLOG_INFO("Stack pointer found at arg index {} ({} stack)", i + 4, i);
                } else if (previous_stack_repeating_index && *previous_stack_repeating_index == i - 1) {
                    texture_argument_index = i - 2;
                    shader_argument_index = i - 1;
                    SPDLOG_INFO("Texture argument may be at index {} ({} stack)", *texture_argument_index + 4, *texture_argument_index);
                    SPDLOG_INFO("Shader argument may be at index {} ({} stack)", *shader_argument_index + 4, *shader_argument_index);
                    break;
                }
            }

            if (!texture_argument_index && !shader_argument_index) {
                // operate on a wild guess (hardcoded function signature)
                SPDLOG_INFO("Calling E8 version of texture create with hardcoded function signature");

                void (*func)(
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t samples,
                    uintptr_t flags,
                    uintptr_t a7,
                    uintptr_t a8,
                    uintptr_t a9,
                    FTexture2DRHIRef* out,
                    FTexture2DRHIRef* shader_out,
                    uintptr_t additional,
                    uintptr_t additional2) = (decltype(func))func_ptr;

                func((uint32_t)size.x, (uint32_t)size.y, 2, ctx.r9,
                    stack_args[0], stack_args[1], 
                    stack_args[2], stack_args[3],
                    stack_args[4],
                    &out, &shader_out,
                    stack_args[7], stack_args[8]);
            } else {
                // dynamically generate the function call
                SPDLOG_INFO("Calling E8 version of texture create with dynamically generated function signature");

                void (*func)(
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t stack_0,
                    uintptr_t stack_1,
                    uintptr_t stack_2,
                    uintptr_t stack_3,
                    uintptr_t stack_4,
                    uintptr_t stack_5,
                    uintptr_t stack_6,
                    uintptr_t stack_7,
                    uintptr_t stack_8) = (decltype(func))func_ptr;

                std::array<uintptr_t, 9> cloned_stack{};
                for (auto i = 0; i < 9; ++i) {
                    cloned_stack[i] = stack_args[i];
                }

                cloned_stack[*texture_argument_index] = (uintptr_t)&out;
                cloned_stack[*shader_argument_index] = (uintptr_t)&shader_out;

                func((uint32_t)size.x, (uint32_t)size.y, 2, ctx.r9,
                    cloned_stack[0], cloned_stack[1], 
                    cloned_stack[2], cloned_stack[3],
                    cloned_stack[4],
                    cloned_stack[5], cloned_stack[6],
                    cloned_stack[7], cloned_stack[8]);

                if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                    SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)stack_args[*texture_argument_index];
                }
            }
        }

        rtm->ui_target = out.texture;
    }

    if (out.texture == nullptr) {
        SPDLOG_ERROR("Failed to create UI texture!");
    } else {
        SPDLOG_INFO("Created UI texture at {:x}", (uintptr_t)out.texture);
    }

    //call_with_context((uintptr_t)func, out);

    SPDLOG_INFO("Called the original function!");

    // Cause stuff like the VR ui texture to get recreated.
    VR::get()->reinitialize_renderer();
}

void VRRenderTargetManager_Base::texture_hook_callback(safetyhook::Context& ctx, bool from_second) {
    auto rtm = g_hook->get_render_target_manager();

    SPDLOG_INFO("Post texture hook called!");
    SPDLOG_INFO(" Ref: {:x}", (uintptr_t)rtm->texture_hook_ref);

    if (!rtm->allocate_texture_called) {
        g_hook->set_should_recreate_textures(true);
        rtm->render_target = nullptr;
        rtm->ui_target = nullptr;
        rtm->texture_hook_ref = nullptr;

        SPDLOG_INFO("[Post texture hook] Allocate texture was not called, skipping...");
        return;
    }

    rtm->allocate_texture_called = false;

    // very rare...
    if (rtm->is_using_texture_desc && !rtm->is_version_greq_5_1) {
        const auto pooled_rt_container = (TRefCountPtr<IPooledRenderTarget>*)rtm->texture_hook_ref;

        if (pooled_rt_container != nullptr && pooled_rt_container->reference != nullptr) {
            rtm->texture_hook_ref = &pooled_rt_container->reference->item.texture;
        }
    }

    FRHITexture2D* texture = nullptr;

    if (rtm->texture_hook_ref != nullptr) {
        texture = rtm->texture_hook_ref->texture;

        // happens?
        if (texture == nullptr) {
            SPDLOG_INFO(" Texture is null, trying to get it from RAX...");

            const auto ref = (FTexture2DRHIRef*)ctx.rax;

            if (!IsBadReadPtr(ref, sizeof(void*)) && !IsBadReadPtr(ref->texture, sizeof(void*))) {
                texture = ref->texture;
            } else {
                SPDLOG_ERROR(" RAX is bad! Can't get texture!");
            }
        }

        if (texture != nullptr) {
            SPDLOG_INFO(" Resulting texture: {:x}", (uintptr_t)texture);
            SPDLOG_INFO(" Real resource: {:x}", (uintptr_t)texture->get_native_resource());
            
            FRHITexture2D::set_vtable(*(void**)texture);
        } else {
            SPDLOG_INFO(" Texture is still null!");
        }
    }

    SPDLOG_INFO(" last texture index: {}", rtm->last_texture_index);

    // DIAG: trace the identity/native-resource of the source texture being handed to the
    // compositor. If this never changes to a non-null, non-zero native resource, or if it
    // keeps flipping back to null, the black-screen origin is here rather than downstream in
    // D3D12Component.cpp.
    {
        static void* s_last_diag_texture = nullptr;
        static void* s_last_diag_native_resource = nullptr;
        const void* native_resource = texture != nullptr ? texture->get_native_resource() : nullptr;

        if (texture != (FRHITexture2D*)s_last_diag_texture || native_resource != s_last_diag_native_resource) {
            SPDLOG_INFO("[DIAG] render_target assignment changed: texture={:x} native_resource={:x} (was texture={:x} native_resource={:x})",
                (uintptr_t)texture, (uintptr_t)native_resource, (uintptr_t)s_last_diag_texture, (uintptr_t)s_last_diag_native_resource);
            s_last_diag_texture = texture;
            s_last_diag_native_resource = (void*)native_resource;
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[DIAG] render_target assignment unchanged: texture={:x} native_resource={:x}",
                (uintptr_t)texture, (uintptr_t)native_resource);
        }
    }

    rtm->render_target = texture;
    //rtm->ui_target = texture;
    rtm->texture_hook_ref = nullptr;
    ++rtm->last_texture_index;
}

bool VRRenderTargetManager_Base::is_scene_capture_world_stale() const {
    if (this->scene_capture_actor == nullptr || this->scene_capture_world == nullptr) {
        return false;
    }

    auto engine = sdk::UEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;

    // The engine's current world no longer matches the world we spawned into - a full level
    // transition happened underneath us (as opposed to sub-level streaming within the same
    // persistent world). Our actor/component are about to be (or already are) orphaned from a
    // dying world; tear ourselves down proactively rather than waiting to notice via GC/null checks.
    if (world == nullptr || (void*)world != this->scene_capture_world) {
        return true;
    }

    // The world hasn't changed yet, but it may already be marked for teardown (bIsTearingDown).
    static auto world_class = world->get_class();
    if (world_class != nullptr) {
        static auto is_tearing_down_prop = world_class->find_property(L"bIsTearingDown");
        if (is_tearing_down_prop != nullptr) {
            auto prop_addr = (uint8_t*)((uintptr_t)world + is_tearing_down_prop->get_offset());
            if (prop_addr != nullptr && (*prop_addr & 1) != 0) {
                return true;
            }
        }
    }

    return false;
}

void VRRenderTargetManager_Base::destroy_scene_capture() try {
    const auto current_generation = g_hook != nullptr ? g_hook->get_view_target_generation() : 0;
    SPDLOG_INFO("[DIAG] destroy_scene_capture() called (generation={}): scene_capture_actor={:x} in_flight_target={:x} scene_capture_target_valid={}",
        current_generation, (uintptr_t)(sdk::AActor*)this->scene_capture_actor, (uintptr_t)this->in_flight_target, this->scene_capture_target.valid());

    if (this->scene_capture_actor != nullptr && this->in_flight_target == nullptr) {
        SPDLOG_INFO("Destroying scene capture!");

        if (this->scene_capture_actor.valid()) {
            // Actor/component are intentionally never rooted (see create_scene_capture()), so no
            // remove_from_root() call is needed here - they're free to be collected normally by
            // their owning world's GC pass if we don't get here first.
            this->scene_capture_actor->destroy_actor();
        }
    }

    if (this->in_flight_target == nullptr) {
        this->scene_capture_actor = nullptr;
        this->scene_capture_component = nullptr;
        this->scene_capture_target = nullptr;
        this->scene_capture_world = nullptr;
        this->scene_capture_ready_time = std::chrono::steady_clock::time_point{};

        // Immediate assignment for local thread safety, sync job for queue ordering
        this->scene_capture_target_rhi_thread = nullptr;

        RHIThreadWorker::get().enqueue([this]() -> void { this->scene_capture_target_rhi_thread = nullptr; });
    } else {
        // NOTE: destroy_scene_capture() was requested (typically alongside a view-target reallocation,
        // generation bumped above/at the caller) but skipped its actual teardown because in_flight_target
        // is still non-null. This means the OLD scene-capture texture/actor remains bound past the point
        // a new generation was requested. If a frame is composited/submitted using this stale scene
        // capture target after the reallocation, that is a candidate race for single-image ghosting/
        // double-image artifacts. Log clearly so this can be correlated against reallocation and
        // composite/submission generation numbers in a captured log.
        SPDLOG_INFO("[DIAG] destroy_scene_capture() SKIPPED teardown (generation={}): in_flight_target={:x} still set - old scene capture target remains bound past this generation boundary",
            current_generation, (uintptr_t)this->in_flight_target);
    }
} catch (const std::exception& e) {
    SPDLOG_ERROR("[VRRenderTargetManager] Exception in destroy_scene_capture: {}", e.what());
    this->scene_capture_target = nullptr;
    this->scene_capture_target_rhi_thread = nullptr;
    this->scene_capture_actor = nullptr;
    this->scene_capture_component = nullptr;
    this->scene_capture_world = nullptr;
    this->scene_capture_ready_time = std::chrono::steady_clock::time_point{};
} catch (...) {
    SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in destroy_scene_capture!");
}

FRHITexture2D* VRRenderTargetManager_Base::get_scene_capture_render_target() {
    if (this->in_flight_target != nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_render_target: in_flight_target != nullptr, returning null");
        return nullptr;
    }

    const auto is_same_as_rhi_thread = RHIThreadWorker::get().is_same_thread();

    // Cache smart pointer locally to avoid thread torn reads
    const auto sct = is_same_as_rhi_thread ? this->scene_capture_target_rhi_thread : this->scene_capture_target;

    if (sct == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_render_target: sct is null (is_same_as_rhi_thread={})", is_same_as_rhi_thread);
        return nullptr;
    }

    try {
        if (!sct.valid()) {
            SPDLOG_WARN("[VRRenderTargetManager] get_scene_capture_render_target: sct not valid, nulling out (is_same_as_rhi_thread={})", is_same_as_rhi_thread);
            // Null out thread references immediately to prevent log spam during level changes
            if (is_same_as_rhi_thread) {
                this->scene_capture_target_rhi_thread = nullptr;
            } else {
                this->scene_capture_target = nullptr;
            }
            return nullptr;
        }

        auto rsrc = (sdk::FTextureRenderTargetResource*)sct->get_resource();
        if (rsrc == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_render_target: sct->get_resource() returned null");
            return nullptr;
        }

        auto rsrc_frt = rsrc->as_render_target();
        if (rsrc_frt != nullptr) {
            auto tex_ref = rsrc_frt->get_render_target_texture();
            if (tex_ref != nullptr && *tex_ref != nullptr) {
                return *tex_ref;
            }

            SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_render_target: render_target_texture ref/value is null (tex_ref={:x})", (uintptr_t)tex_ref);
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_render_target: rsrc->as_render_target() returned null (rsrc={:x})", (uintptr_t)rsrc);
        }
    } catch (...) {
        // Quiet down exception handling during level load/tear-down
        SPDLOG_WARN("[VRRenderTargetManager] Caught exception dereferencing scene capture target!");

        if (is_same_as_rhi_thread) {
            this->scene_capture_target_rhi_thread = nullptr;
        } else {
            this->scene_capture_target = nullptr;
        }
    }

    return nullptr;
}

sdk::UTexture* VRRenderTargetManager_Base::get_scene_capture_utexture() {
    if (this->in_flight_target != nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_utexture: in_flight_target != nullptr, returning null");
        return nullptr;
    }

    const auto& utex = this->scene_capture_target;

    if (utex == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VRRenderTargetManager] get_scene_capture_utexture: scene_capture_target is null");
        return nullptr;
    }

    try {
        if (utex.valid()) {
            return (sdk::UTexture*)utex;
        }

        SPDLOG_WARN("[VRRenderTargetManager] Scene capture target is not a UTexture! Texture probably deleted on level change! (ptr={:x})", (uintptr_t)utex.get());

        GameThreadWorker::get().enqueue([this]() -> void {
            this->in_flight_target = nullptr;
            this->destroy_scene_capture();
        });
    } catch (...) {
        SPDLOG_ERROR("[VRRenderTargetManager] Exception in get_scene_capture_utexture! Texture probably deleted on level change!");

        GameThreadWorker::get().enqueue([this]() -> void {
            this->in_flight_target = nullptr;
            this->destroy_scene_capture();
        });
    }

    return nullptr;
}

bool VRRenderTargetManager_Base::create_scene_capture() try {
    if (this->in_flight_target != nullptr) {
        return false;
    }

    // This is necessary for offset calculations to succeed.
    if (FRHITexture2D::get_vtable() == nullptr) {
        SPDLOG_WARN("[VRRenderTargetManager] FRHITexture2D vtable is null, waiting for it to be set!");
        return false;
    }

    // Cooldown/throttle: during a level transition the engine tick can flicker (resume for a frame,
    // then stall again), which previously let every caller's loading-guard open briefly and
    // re-trigger this whole actor-spawn + FRenderTarget rehook cascade. That work itself eats enough
    // game-thread time to cause the next stall, creating a self-sustaining loop that starves level
    // streaming of the cycles it needs to finish loading. Refuse to actually recreate more often than
    // this, regardless of which call site is asking, so a flickering tick can't retrigger the loop.
    static constexpr auto scene_capture_recreate_cooldown = std::chrono::milliseconds(3000);
    const auto now = std::chrono::steady_clock::now();
    const auto since_last_create = now - this->last_scene_capture_create_time;

    // DIAG: the cooldown above only limits FREQUENCY, not whether the tick resumption that opened
    // the loading-guard was real. A tick_stalled flicker (false for a single frame during heavy
    // level-streaming churn) previously let one create_scene_capture() call slip through mid-storm,
    // recreating the RTV/SRV descriptor heaps and rebinding textures on the very same frame the
    // engine's own render thread was already taking multiple seconds to complete (observed hitting
    // 3857ms for a single frame, immediately followed by Present failing with
    // DXGI_ERROR_DEVICE_REMOVED/DEVICE_HUNG - a GPU TDR). Require the tick to have been reported
    // non-stalled for several CONSECUTIVE checks before treating a resumption as real, mirroring the
    // resolution-change debounce in need_reallocate_view_target() below.
    {
        static uint32_t s_consecutive_non_stalled_checks = 0;
        // DIAG: raised from 5 after a rare crash (DXGI_ERROR_DEVICE_REMOVED/DEVICE_HUNG, TDR) was
        // observed where a scene-capture-texture rebind (new SRV/RTV heap creation) landed on the
        // very next frame after a 2072ms single-frame spike during a level transition - i.e. 5
        // consecutive non-stalled checks was not always enough to guarantee the GPU had actually
        // caught up before we did more descriptor heap/resource work. A higher requirement makes us
        // wait longer after the tick looks stable before recreating scene capture resources.
        static constexpr uint32_t required_consecutive_non_stalled_checks = 15;

        if (VR::get() != nullptr && VR::get()->is_engine_tick_stalled()) {
            s_consecutive_non_stalled_checks = 0;
            SPDLOG_WARN("[VRRenderTargetManager] create_scene_capture() refused - engine tick is currently stalled.");
            return false;
        }

        ++s_consecutive_non_stalled_checks;

        if (s_consecutive_non_stalled_checks < required_consecutive_non_stalled_checks) {
            SPDLOG_WARN("[VRRenderTargetManager] create_scene_capture() deferred - waiting for engine tick to stabilize ({}/{}), to avoid recreating render resources during a transient tick resumption mid level-transition.",
                s_consecutive_non_stalled_checks, required_consecutive_non_stalled_checks);
            return false;
        }
    }

    // Refuse to (re)create while the view-target reallocation debounce is still stabilizing on a
    // new size (e.g. a resolution change mid level-transition). Creating now would size the scene
    // capture texture using get_hmd_width()/get_hmd_height() at the OLD resolution, just before the
    // reallocation lands and swaps the destination eye texture to the NEW resolution - producing a
    // mismatched scene-capture/eye-texture pair that has been observed to fail SRV descriptor heap
    // creation and take the whole D3D12 device down (DXGI_ERROR_DEVICE_REMOVED).
    if (is_view_target_reallocation_pending()) {
        SPDLOG_WARN("[VRRenderTargetManager] create_scene_capture() deferred - view target reallocation is pending/stabilizing.");
        return false;
    }

    if (this->last_scene_capture_create_time.time_since_epoch().count() != 0 && since_last_create < scene_capture_recreate_cooldown) {
        SPDLOG_WARN("[VRRenderTargetManager] create_scene_capture() throttled - only {}ms since last creation (cooldown={}ms). "
                     "This usually means something is repeatedly invalidating the scene capture during a level transition.",
            std::chrono::duration_cast<std::chrono::milliseconds>(since_last_create).count(), scene_capture_recreate_cooldown.count());
        return false;
    }

    this->last_scene_capture_create_time = now;

    destroy_scene_capture();

    SPDLOG_INFO("Creating scene capture!");

    auto kismet_rendering = sdk::UKismetRenderingLibrary::get();

    if (kismet_rendering == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UKismetRenderingLibrary!");
        return false;
    }

    static auto scene_capture_c = sdk::USceneCaptureComponent2D::static_class();

    if (scene_capture_c == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get USceneCaptureComponent2D class!");
        return false;
    }

    auto ugs = sdk::UGameplayStatics::get();

    if (ugs == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UGameplayStatics!");
        return false;
    }

    auto engine = sdk::UGameEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UGameEngine!");
        return false;
    }

    auto world = engine->get_world();

    if (world == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UWorld!");
        return false;
    }

    static auto actor_c = sdk::AActor::static_class();

    if (actor_c == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get AActor class!");
        return false;
    }

    this->scene_capture_actor = ugs->spawn_actor(world, actor_c, glm::vec3{0, 0, 0});

    if (this->scene_capture_actor == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to spawn actor!");
        return false;
    }

    // NOTE: We intentionally do NOT add_to_root() the actor/component here. Rooting them keeps
    // them alive indefinitely regardless of their owning UWorld, but during a *full* level
    // transition (e.g. character-select -> game world) the engine's teardown/GC pass expects
    // every actor belonging to the old world to actually be collectible so it can confirm the
    // old world is fully gone before finishing initialization of the new one. A rooted actor left
    // dangling from a torn-down world stalls that handshake (observed as the loading screen
    // getting stuck at a fixed percentage). Instead we proactively call destroy_scene_capture()
    // ourselves the moment we detect the owning world is tearing down or has changed (see
    // is_local_pawn_missing()/update_boot_phase_tracking() callers and the world-change watchdog
    // in begin_render_viewfamily_real), so we never need to rely on GC sweeping these out from
    // under us and never need to keep them rooted past their world's natural lifetime.
    this->scene_capture_component = (sdk::USceneCaptureComponent2D*)this->scene_capture_actor->add_component_by_class(scene_capture_c, false);

    if (this->scene_capture_component == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to add scene capture component!");
        return false;
    }

    // Remember which world we spawned into so is_scene_capture_world_stale() can detect a full
    // level transition (as opposed to sub-level streaming) and proactively tear us down before
    // the engine's own GC pass needs to.
    this->scene_capture_world = world;

    const float clear_color[4] {0.0f, 0.0f, 0.0f, 1.0f};
    auto tgt_raw =
        kismet_rendering->create_render_target_2d(world, VR::get()->get_hmd_width(), VR::get()->get_hmd_height(), 2, clear_color, false);

    if (tgt_raw == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to create texture!");
        return false;
    }

    // NOTE: Do NOT add_to_root() this render target. UKismetRenderingLibrary::CreateRenderTarget2D
    // creates it with Outer = the UWorld we passed in. Rooting it keeps that entire world reachable
    // across a LoadMap, and the engine's VerifyLoadMapWorldCleanup() then raises a hard "Fatal
    // error!" ("World ... not cleaned up by garbage collection") when the old world is still
    // referenced. Observed directly when a 2D->VR mode switch recreated the scene capture in the
    // LaunchScene world right as the main map began loading. GC safety across threads is already
    // handled by UObjectReference::valid() + the in_flight_target handshake below.
    sdk::UObjectReference tgt{tgt_raw};

    SPDLOG_INFO("[VRRenderTargetManager] Created texture target: {:x}", (uintptr_t)tgt.get());
    this->scene_capture_actor->finish_add_component(this->scene_capture_component);

    this->scene_capture_component->set_texture_target(tgt);

    // We don't actually want this to tick.
    // We are just using the property as a convenient way to keep the texture alive without crashing.
    this->scene_capture_component->set_visibility(false);
    if (auto capture_every_frame = scene_capture_c->find_property(L"bCaptureEveryFrame"); capture_every_frame != nullptr) {
        *capture_every_frame->get_data<bool>(this->scene_capture_component) = false;
    }

    // Without this, the engine doesn't consider the capture's FSceneView to be "continuously live"
    // since bCaptureEveryFrame is false above, so per-view time-dependent state (WPO/wind phase,
    // shadow invalidation caching, foliage LOD dithering) can go stale or desync from the primary
    // view instead of being refreshed each time we manually drive the capture. This is what was
    // causing wind-animated foliage to appear static and shadows/LOD to be left-eye-dominated in
    // the right eye. bAlwaysPersistRenderingState tells the engine to keep that per-view state
    // updated/persisted across captures even though the component itself isn't auto-ticking.
    if (auto always_persist = scene_capture_c->find_property(L"bAlwaysPersistRenderingState"); always_persist != nullptr) {
        *always_persist->get_data<bool>(this->scene_capture_component) = true;
    } else {
        SPDLOG_WARN("[VRRenderTargetManager] bAlwaysPersistRenderingState property not found on USceneCaptureComponent2D - "
                    "wind/shadow/LOD desync between eyes may persist");
    }

    static bool already_updated{false};
    static std::array<uintptr_t, 100> original_frender_target_vtable{};
    static auto gamma_increase_fn = +[](const sdk::FRenderTarget* frt) -> float {
        auto rtm = g_hook->get_render_target_manager();
        auto viewport = rtm != nullptr ? rtm->get_viewport() : nullptr;

        if (viewport != nullptr) {
            return viewport->get_display_gamma();
        }

        return 2.2f;
    };

    static auto hook_frt = [](sdk::FRenderTarget* frt) {
        if (frt == nullptr) {
            SPDLOG_WARN("[FRenderTarget] FRenderTarget is null! Can't hook!");
            return;
        }

        SPDLOG_INFO("[FRenderTarget] Hooking FRenderTarget!");

        auto& vtable = *(void**)frt;
        memcpy(original_frender_target_vtable.data(), vtable, original_frender_target_vtable.size() * sizeof(uintptr_t));

        if (auto display_gamma_index = sdk::FRenderTarget::get_display_gamma_index(); display_gamma_index != 0) {
            original_frender_target_vtable[*display_gamma_index] = (uintptr_t)gamma_increase_fn;
            vtable = original_frender_target_vtable.data();
            SPDLOG_INFO("[FRenderTarget] Hooked FRenderTarget!");
        } else {
            SPDLOG_WARN("[FRenderTarget] Gamma index not found, can't hook!");
        }
    };

    static const auto utex_c = sdk::UTexture::static_class();

    // Enqueue offset lookup on the render thread because that's when the resource is actually created.
    if (!already_updated) {
        this->in_flight_target = tgt;

        // Repeats every render loop for 5 seconds, times out if the texture is not created.
        RenderThreadWorker::ConditionalJobFunc render_thread_conditional_task = [this, tgt]() -> bool {
            try {
                if (!tgt.valid()) {
                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                    GameThreadWorker::get().enqueue([this]() -> void {
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                    });
                    return true;
                }
    
                if (sdk::UTexture::update_render_resource_offset_texture2d(tgt)) {
                    SPDLOG_INFO("Successfully updated render resource offset for scene capture target!");
    
                    if (auto rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource(); rsrc != nullptr) {
                        const bool success = sdk::FTextureRenderTargetResource::update_render_target_vtable_offset(rsrc);
                        const auto frt = success ? rsrc->as_render_target() : nullptr;
    
                        if (frt != nullptr) {
                            sdk::FRenderTarget::update_offsets(frt);

                            if (frt->get_render_target_texture() == nullptr || *frt->get_render_target_texture() == nullptr) {
                                SPDLOG_WARN("Waiting for render target texture to be valid...");
                                return false;
                            }
    
                            hook_frt(frt);
    
                            RHIThreadWorker::get().enqueue([this, tgt]() -> void {
                                if (!tgt.valid()) {
                                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                                    this->scene_capture_target_rhi_thread = nullptr;
                                    return;
                                }

                                this->scene_capture_target_rhi_thread = tgt;
                            });
                            
                            GameThreadWorker::get().enqueue([this, tgt]() -> void {
                                if (!tgt.valid()) {
                                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                                    this->in_flight_target = nullptr;
                                    destroy_scene_capture();
                                    return;
                                }

                                this->scene_capture_target = tgt;
                                this->in_flight_target = nullptr;
                                this->scene_capture_ready_time = std::chrono::steady_clock::now();

                                // DIAG: right-eye startup-lag investigation. Records the boot-phase state at
                                // the exact moment the scene capture becomes "ready" - if boot phase is still
                                // active here, the same-pass/grace-period guards will keep suppressing the
                                // real stereoscopic path for a while longer even though this handle is valid,
                                // which is a likely source of the right eye lagging behind on first launch.
                                SPDLOG_INFO("[DIAG] Scene capture texture created! (boot_phase_active={})", is_boot_phase_active());
                                SPDLOG_INFO("Scene capture texture created!");
                            });
    
                            already_updated = true;
        
                            return true;
                        }
    
                        SPDLOG_WARN("Waiting for render target to be valid...");
    
                        return false; // Keep waiting until it works.
                    }
                } else {
                    SPDLOG_ERROR("Failed to update render resource offset for scene capture target!");
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture (offset lookup): {}", e.what());
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            } catch (...) {
                SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture (offset lookup)!");
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            }

            return false;
        };

        RenderThreadWorker::ConditionalJobTimeoutFunc render_thread_on_timeout = [this]() {
            SPDLOG_ERROR("Timed out waiting for scene capture texture to be created!");
            GameThreadWorker::get().enqueue([this]() -> void {
                this->in_flight_target = nullptr;
                destroy_scene_capture();
            });
        };

        RenderThreadWorker::get().enqueue_conditional(render_thread_conditional_task, render_thread_on_timeout, std::chrono::seconds(2));
    
        SPDLOG_INFO("Waiting for scene capture texture to be created...");
    } else {
        this->in_flight_target = tgt;

        RenderThreadWorker::ConditionalJobFunc render_thread_conditional_task = [this, tgt]() -> bool {
            try {
                if (!tgt.valid()) {
                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                    GameThreadWorker::get().enqueue([this]() -> void {
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                    });
    
                    return true;
                }
    
                auto rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource();
                auto frt = rsrc != nullptr ? rsrc->as_render_target() : nullptr;
                auto frttex = frt != nullptr ? frt->get_render_target_texture() : nullptr;
    
                // Wait until FRenderTarget is not null.
                if (frt == nullptr || frttex == nullptr || *frttex == nullptr) {
                    SPDLOG_WARN("Waiting for render target to be valid...");
                    return false;
                }
    
                hook_frt(frt);
    
                RHIThreadWorker::get().enqueue([this, tgt]() -> void {
                    if (!tgt.valid()) {
                        SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                        this->scene_capture_target_rhi_thread = nullptr;
                        return;
                    }

                    this->scene_capture_target_rhi_thread = tgt;
                });
    
                GameThreadWorker::get().enqueue([this, tgt]() -> void {
                    if (!tgt.valid()) {
                        SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                        return;
                    }
    
                    this->in_flight_target = nullptr;
                    this->scene_capture_target = tgt;
                    this->scene_capture_ready_time = std::chrono::steady_clock::now();

                    // DIAG: right-eye startup-lag investigation. Records the boot-phase state at the
                    // exact moment the scene capture becomes "ready" - see the sibling log at the other
                    // scene_capture_ready_time assignment site above for the matching rationale.
                    SPDLOG_INFO("[DIAG] Scene capture texture ready (boot_phase_active={})", is_boot_phase_active());

                    // DIAG: confirm the scene capture texture actually resolves to a usable
                    // FRHITexture2D/native resource at the moment it becomes "ready". If the
                    // render target texture is black afterwards, this at least proves whether
                    // the resource exists and what its identity is.
                    try {
                        auto rsrc = tgt.valid() ? (sdk::FTextureRenderTargetResource*)tgt->get_resource() : nullptr;
                        auto frt = rsrc != nullptr ? rsrc->as_render_target() : nullptr;
                        auto frttex_ref = frt != nullptr ? frt->get_render_target_texture() : nullptr;
                        auto frttex = frttex_ref != nullptr ? *frttex_ref : nullptr;
                        SPDLOG_INFO("[DIAG] Scene capture texture fully created! utexture={:x} rhi_texture={:x} native_resource={:x}",
                            (uintptr_t)(sdk::UTexture*)tgt, (uintptr_t)frttex, frttex != nullptr ? (uintptr_t)frttex->get_native_resource() : 0);
                    } catch (...) {
                        SPDLOG_INFO("Scene capture texture fully created! (failed to resolve DIAG identity)");
                    }
                });
    
                return true;
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture: {}", e.what());
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            } catch (...) {
                SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture!");
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            }
        };

        RenderThreadWorker::ConditionalJobTimeoutFunc render_thread_on_timeout = [this]() {
            SPDLOG_ERROR("Timed out waiting for scene capture texture to be created!");
            GameThreadWorker::get().enqueue([this]() -> void {
                this->in_flight_target = nullptr;
                destroy_scene_capture();
            });
        };

        RenderThreadWorker::get().enqueue_conditional(render_thread_conditional_task, render_thread_on_timeout, std::chrono::seconds(2));

        SPDLOG_INFO("Waiting for scene capture texture to be created...");
    }

    return true;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture: {}", e.what());
    return false;
} catch (...) {
    SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture!");
    return false;
}

// This is a very special fix for cases where engine modifications
// can add a second call to UpdateViewportRHI right before the place we expect it to get called
// The fact that they get called back-to-back over and over causes huge performance problems
// because the viewport texture keeps getting recreated over and over.
// This hook attempts to only allow the last call to UpdateViewportRHI inside of EnqueueBeginRenderFrame to do anything
// Usually there's only one call to UpdateViewportRHI inside of EnqueueBeginRenderFrame, but (very rarely) there can be two.
__declspec(noinline) void FFakeStereoRenderingHook::update_viewport_rhi_hook(void* viewport, size_t destroyed, size_t new_size_x, size_t new_size_y, size_t new_window_mode, size_t preferred_pixel_format) {
    auto call_orig = [&]() {
        g_hook->m_update_viewport_rhi_hook->get_original<void(*)(void*, size_t, size_t, size_t, size_t, size_t)>()(viewport, destroyed, new_size_x, new_size_y, new_window_mode, preferred_pixel_format);
    };

    SPDLOG_INFO_ONCE("UpdateViewportRHI (embedded): {:x}", (uintptr_t)_ReturnAddress());

    const auto hmd_active = VR::get()->is_hmd_active();
    static bool modified_use_separate_rt = false;

    if (!hmd_active) {
        if (modified_use_separate_rt) {
            modified_use_separate_rt = false;

            const auto rtm = g_hook->get_render_target_manager();

            if (rtm != nullptr) {
                if (const auto offset = rtm->get_viewport_force_separate_rt_offset()) {
                    SPDLOG_INFO_ONCE("Resetting bUseSeparateRenderTarget to false!");
                    auto& use_separate_rt = *(bool*)((uintptr_t)viewport + (*offset - 1));
                    use_separate_rt = false;
                }
            }
        }

        call_orig();
        return;
    }

    struct FunctionInfo {
        std::vector<uintptr_t> return_addrs{}; // in order of call
        size_t count{0};
    };

    static std::mutex mtx{};
    static std::unordered_map<uintptr_t, uintptr_t> functions_within{};
    static std::unordered_map<uintptr_t, FunctionInfo> function_infos{};

    {
        std::scoped_lock _{mtx};

        const auto return_addr = (uintptr_t)_ReturnAddress();
        auto function_within = functions_within.find(return_addr);

        if (function_within == functions_within.end()) {
            const auto result = utility::find_virtual_function_start(return_addr);

            if (result) {
                functions_within[return_addr] = *result;
            } else {
                functions_within[return_addr] = 0;
            }

            function_within = functions_within.find(return_addr);

            if (function_within->second != 0) {
                ++function_infos[function_within->second].count;
            }

            function_infos[function_within->second].return_addrs.push_back(return_addr);

            SPDLOG_INFO("Added new call of UpdateViewportRHI to function {:x} (count: {})", function_within->second, function_infos[function_within->second].count);
        }

        if (function_within->second == 0) {
            SPDLOG_INFO_ONCE("Could not find vfunc start for call of UpdateViewportRHI, calling original.");
            call_orig();
            return;
        }

        const auto& function_info = function_infos[function_within->second]; 

        // We only care about corrections where UpdateViewportRHI is called more than once in the same function.
        if (function_info.count <= 1 || function_info.return_addrs.empty()) {
            call_orig();
            return;
        }

        if (!g_hook->m_rendertarget_manager_embedded_in_stereo_device) {
            const auto rtm = g_hook->get_render_target_manager();

            if (rtm != nullptr) {
                if (const auto offset = rtm->get_viewport_force_separate_rt_offset()) {
                    auto& should_force_separate_rt = *(bool*)((uintptr_t)viewport + *offset);
                    auto& use_separate_rt = *(bool*)((uintptr_t)viewport + (*offset - 1));

                    if (!should_force_separate_rt) {
                        SPDLOG_INFO_ONCE("UpdateViewportRHI was called without should_force_separate_rt being set to true, setting flags.");
                        should_force_separate_rt = true;
                        use_separate_rt = true;
                        modified_use_separate_rt = true;

                        call_orig(); // Call original to guarantee RHI setup completes instead of dropping the frame execution
                        return;
                    }
                }
            }
        } else {      
            // We only want the last function to be called.
            // We don't need to call the original here because it will get called by the last function.
            // if we call the original here, it will cause performance issues.
            if (function_info.return_addrs.back() != return_addr) {
                return;
            }
        }
    }

    
    if (!g_hook->m_rendertarget_manager_embedded_in_stereo_device) {
        call_orig();
        return;
    }

    auto& rtm = g_hook->get_embedded_rtm();

    static std::chrono::steady_clock::time_point last_time_hmd_active{};
    static bool hmd_was_active = false;
    bool should_call_orig = false;

    if (hmd_active && !hmd_was_active) {
        last_time_hmd_active = std::chrono::steady_clock::now();
        hmd_was_active = true;
    } else if (!hmd_active) {
        hmd_was_active = false;
        should_call_orig = true;
    }

    if (hmd_active) {
        should_call_orig = std::chrono::steady_clock::now() - last_time_hmd_active <= std::chrono::milliseconds(2000);
        //should_call_orig = should_call_orig || (std::chrono::steady_clock::now() - rtm.last_time_needed_hmd_reallocate <= std::chrono::milliseconds(2000));
    }

    if (should_call_orig) {
        rtm.should_use_separate_rt_called = false;
        rtm.need_reallocate_viewport_render_target_called = false;
        call_orig();
        return;
    }

    if (!rtm.should_use_separate_rt_called) {
        SPDLOG_INFO_ONCE("Skipping UpdateViewportRHI (embedded) because ShouldUseSeparateRenderTarget() was not called!");
        return; // Do not call at all.
    }

    if (!rtm.need_reallocate_viewport_render_target_called) {
        const auto need_reallocate = g_hook->get_render_target_manager()->need_reallocate_view_target(*(sdk::FViewport*)viewport);

        if (!need_reallocate) {
            SPDLOG_INFO_ONCE("Skipping UpdateViewportRHI (embedded) because NeedReallocateViewportRenderTarget() was not called and we don't need to reallocate anyway!");
            rtm.should_use_separate_rt_called = false;
            return; // Do not call at all.
        }

        SPDLOG_INFO_ONCE("We need to reallocate the viewport render target even though NeedReallocateViewportRenderTarget() was not called!");
        //rtm.last_time_needed_hmd_reallocate = std::chrono::steady_clock::now();
    }

    call_orig();
    rtm.should_use_separate_rt_called = false;
    rtm.need_reallocate_viewport_render_target_called = false;
}

void FFakeStereoRenderingHook::attempt_hook_update_viewport_rhi(uintptr_t return_address) {
    if (/*!m_rendertarget_manager_embedded_in_stereo_device ||*/ m_special_detected || m_attempted_hook_update_viewport_rhi) {
        return;
    }

    m_attempted_hook_update_viewport_rhi = true;

    if (m_update_viewport_rhi_hook == nullptr) {
        SPDLOG_INFO("Attempting to hook UpdateViewportRHI...");

        const auto init_dynamic_rhi = utility::find_virtual_function_start(return_address);

        if (init_dynamic_rhi) {
            SPDLOG_INFO("Found InitDynamicRHI: {:x}", *init_dynamic_rhi);

            const auto init_dynamic_rhi_ptr = utility::scan_ptr(*utility::get_module_within(*init_dynamic_rhi), *init_dynamic_rhi);
            if (!init_dynamic_rhi_ptr) {
                SPDLOG_ERROR("Failed to find InitDynamicRHI pointer!");
                return;
            }

            const auto update_viewport_rhi_ptr = *init_dynamic_rhi_ptr - (sizeof(void*) * 2);

            if (*(void**)update_viewport_rhi_ptr == nullptr || IsBadReadPtr(*(void**)update_viewport_rhi_ptr, sizeof(void*))) {
                SPDLOG_ERROR("Failed to find UpdateViewportRHI!");
                return;
            }

            // Make sure this is no displacement reference to this. This can mean we accidentally found the vtable for IViewportRenderTargetProvider
            // The vfunc pointer should be in the middle of the vtable, not the start.
            if (utility::scan_displacement_reference(*utility::get_module_within(*init_dynamic_rhi), update_viewport_rhi_ptr)) {
                SPDLOG_ERROR("Found displacement reference to UpdateViewportRHI, this is probably the vtable for IViewportRenderTargetProvider, aborting!");
                return;
            }

            m_update_viewport_rhi_hook = std::make_unique<PointerHook>((void**)update_viewport_rhi_ptr, &update_viewport_rhi_hook);
        } else {
            SPDLOG_ERROR("Failed to find InitDynamicRHI, cannot hook UpdateViewportRHI!");
        }
    }
}

bool VRRenderTargetManager_Base::allocate_render_target_texture(uintptr_t return_address, FTexture2DRHIRef* tex, FTexture2DRHIRef* shader_resource) {
    this->texture_hook_ref = tex;
    this->shader_resource_hook_ref = shader_resource;
    this->allocate_texture_called = true;

    if (!this->set_up_texture_hook) {
        ZoneScopedN("VRRenderTargetManager_Base::allocate_render_target_texture initialization");
        SPDLOG_INFO("AllocateRenderTargetTexture retaddr: {:x}", return_address);

        g_hook->attempt_hook_update_viewport_rhi(return_address);

        SPDLOG_INFO("Scanning for call instr...");

        bool next_call_is_not_the_right_one = false;

        auto is_string_nearby = [](uintptr_t addr, std::wstring_view str) {
            const auto addr_module = utility::get_module_within(addr);
            if (!addr_module) {
                return false;
            }

            const auto module_size = utility::get_module_size(*addr_module);
            const auto module_end = (uintptr_t)*addr_module + *module_size - 0x1000;

            // Find all possible strings, not just the first one
            for (auto str_addr = utility::scan_string(*addr_module, str.data(), true); 
                str_addr.has_value(); 
                str_addr = utility::scan_string(*str_addr + 1, (module_end - (*str_addr + 1)), str.data(), true)) 
            {
                // Scan for ALL references to this string
                for (auto string_ref = utility::scan_displacement_reference(*addr_module, (uintptr_t)*str_addr);
                    string_ref.has_value();
                    string_ref = utility::scan_displacement_reference(*string_ref + 1, (module_end - (*string_ref + 1)), (uintptr_t)*str_addr))
                {
                    const auto string_ref_func_start = utility::find_function_start((uintptr_t)*string_ref);
                    const auto return_addr_func_start = utility::find_function_start(addr);

                    SPDLOG_INFO("String ref func start: {:x}", (uintptr_t)*string_ref_func_start);
                    SPDLOG_INFO("Return addr func start: {:x}", (uintptr_t)*return_addr_func_start);

                    if (string_ref_func_start && return_addr_func_start && *string_ref_func_start == *return_addr_func_start) {
                        return true;
                    }
                }
            }

            return false;
        };

        // This string is present in UE5 (>= 5.1) and used when using texture descriptors to create textures.
        // that means this is UE5 and the function will take a texture descriptor instead of a bunch of arguments.
        if (is_string_nearby(return_address, L"BufferedRT")) {
            SPDLOG_INFO("Found string ref for BufferedRT, this is UE5!");
            this->is_using_texture_desc = true;
            this->is_version_greq_5_1 = true;
        }

        // Present in a specific game or game(s), somewhere around 4.8-4.12 (?)
        // indicates that texture descriptors are being used.
        if (is_string_nearby(return_address, L"SceneViewBuffer")) {
            SPDLOG_INFO("Found string ref for SceneViewBuffer, texture descriptors are being used!");
            this->is_using_texture_desc = true;
            this->is_version_greq_5_1 = false;

            next_call_is_not_the_right_one = true; // not seen a case where this isn't true (yet)
        }

        // Now, we need to emulate from where AllocateRenderTargetTexture returns from
        // we will set RAX to false, to get the control flow correct
        // and then keep emulating until we hit the call we want
        // Previously, we were using just straight linear disassembly to do this, and it mostly worked
        // but in one game, there was an unconditional branch after the call instead of flowing
        // directly into the next instruction.
        auto emu = utility::ShemuContext{*utility::get_module_within(return_address)};

        emu.ctx->Registers.RegRax = 0;
        emu.ctx->Registers.RegRip = (ND_UINT64)return_address;
        emu.ctx->MemThreshold = 100;

        const std::vector<std::string> bad_patterns_before_call = {
            "B2 32", // mov dl, 32h, (seen in UE5 debug/dev builds)
            "B2 2A", // mov dl, 2Ah, (seen in UE4.23 debug/dev builds)
            "B2 2B", // mov dl, 2Bh, (seen in UE4.25 debug/dev builds)
            "BA 2F 00 00 00", // mov edx, 2Fh (seen in UE5 debug/dev builds)
            "F6 85 ? ? ? ? 05", // test byte ptr [rbp+?], 5 (seen in UE5 debug/dev builds)
        };

        while(true) {
            if (emu.ctx->InstructionsCount > 200) {
                SPDLOG_WARN("Emulated too many instructions without finding the call, aborting!");
                break;
            }

            const auto ip = emu.ctx->Registers.RegRip;
            const auto bytes = (uint8_t*)ip;
            const auto decoded = utility::decode_one((uint8_t*)ip);

            if (ip != 0) {
                for (const auto& pattern : bad_patterns_before_call) {
                    if (utility::scan(ip, 100, pattern).value_or(0) == ip) {
                        SPDLOG_INFO("Found bad pattern before call, skipping next call: {:x} ({})", ip, pattern);
                        next_call_is_not_the_right_one = true;
                        break;
                    }
                }
            }
            
            if (!next_call_is_not_the_right_one) try {
                const auto addr = utility::resolve_displacement(ip);

                if (addr && !IsBadReadPtr((void*)*addr, 12)) {
                    if (std::wstring_view{(const wchar_t*)*addr}.starts_with(L"BufferedRT")) {
                        this->is_using_texture_desc = true;
                        this->is_version_greq_5_1 = true;

                        SPDLOG_INFO("Found usage of string \"BufferedRT\" while analyzing AllocateRenderTargetTexture!");
                    } else if (std::string_view{(const char*)*addr}.starts_with("IsInRenderingThread") && std::string_view{decoded->Mnemonic}.starts_with("LEA") && decoded->Operands[0].Type == ND_OP_REG && decoded->Operands[0].Info.Register.Reg == NDR_RCX) {
                        SPDLOG_INFO("Found usage of string \"IsInRenderingThread\" while analyzing AllocateRenderTargetTexture, skipping next call!");
                        next_call_is_not_the_right_one = true;
                    }
                }
            } catch(...) {

            }

            // make sure we are not emulating any instructions that write to memory
            // so we can just set the IP to the next instruction
            if (decoded) {
                const auto is_call = std::string_view{decoded->Mnemonic}.starts_with("CALL");

                if (decoded->MemoryAccess & ND_ACCESS_ANY_WRITE || is_call) {
                    // We are looking for the call instruction
                    // This instruction calls RHICreateTargetableShaderResource2D(TexSizeX, TexSizeY, SceneTargetFormat, 1, TexCreate_None,
                    // TexCreate_RenderTargetable, false, CreateInfo, BufferedRTRHI, BufferedSRVRHI); Which sets up the BufferedRTRHI and
                    // BufferedSRVRHI variables.
                    if (is_call && !next_call_is_not_the_right_one && bytes[0] == 0xE8) try {
                        // Analyze some of the instructions inside the call first
                        // If it has a mov eax, 0x800, then returns, we can skip this function
                        const auto fn = utility::calculate_absolute(ip + 1);
                        SPDLOG_INFO("Analyzing call at {:x} to {:x}", ip, fn);

                        if (auto result = utility::scan(fn, 10, "41 B8 30 00 00 00"); result.has_value() && *result == fn) {
                            SPDLOG_INFO("First instruction is a mov r8d, 30h, skipping this call!");
                            next_call_is_not_the_right_one = true;
                        } else if (auto result = utility::scan(fn, 50, "B8 00 08 00 00 C3"); result.has_value()) {
                            SPDLOG_INFO("First few instructions are a mov eax, 800h, ret, skipping this call!");
                            next_call_is_not_the_right_one = true;
                        } else if (this->is_version_greq_5_1) { // Limiting the scope of this to newer UE5 versions so we don't potentially break older versions
                            const auto module_fn_within = utility::get_module_within(fn);
                            const auto next_insn = (uint8_t*)(ip + decoded->Length);

                            // Seen on UE5.3.2 development builds
                            if (auto result = utility::scan_disasm(fn, 15, "BD 01 00 00 00"); result.has_value()) {
                                // This string is not unicode
                                if (utility::find_string_reference_in_path(fn, "InGPUMask != 0", false).has_value()) {
                                    SPDLOG_INFO("Found InGPUMask != 0 string within the function and mov ebp, 1, skipping this call!");
                                    next_call_is_not_the_right_one = true;
                                }
                            } else if (next_insn[0] == 0x84 && next_insn[1] == 0xC0) { // test al, al
                                if (auto ref = utility::find_string_reference_in_path((uintptr_t)next_insn, "IsInRenderingThread()", false); ref.has_value()) {
                                    if (ref->addr > (uintptr_t)next_insn && ref->addr - (uintptr_t)next_insn < 30) {
                                        SPDLOG_INFO("Found IsInRenderingThread() instead of the function we want, skipping this call!");
                                        next_call_is_not_the_right_one = true;
                                    }
                                }
                            } else if (utility::find_pattern_in_path((uint8_t*)fn, 30, true, "66 41 C7 40 34 00 FF")) {
                                SPDLOG_INFO("Found 66 41 C7 40 34 00 FF pattern within the function, skipping this call!");
                                next_call_is_not_the_right_one = true;
                            } else {
                                // Check how many instructions are in the call. If there's <= 30 AND there's no call/jmp in it, this is not the right one
                                size_t insn_count = 0;
                                bool encountered_branch = false;
                                utility::exhaustive_decode((uint8_t*)fn, 200, [&](const utility::ExhaustionContext& ctx) -> utility::ExhaustionResult {
                                    if (std::string_view{ctx.instrux.Mnemonic}.starts_with("CALL") || std::string_view{ctx.instrux.Mnemonic}.starts_with("JMP")) {
                                        encountered_branch = true;
                                        return utility::ExhaustionResult::BREAK;
                                    }

                                    return utility::ExhaustionResult::CONTINUE;
                                });

                                if (insn_count <= 30 && !encountered_branch) {
                                    SPDLOG_INFO("Function at {:x} only has {} instructions and no calls/branches, skipping this call!", fn, insn_count);
                                    next_call_is_not_the_right_one = true;
                                }
                            }
                        }
                    } catch(...) {
                        SPDLOG_INFO("Failed to analyze call at {:x}", ip);
                    }

                    if (is_call && !next_call_is_not_the_right_one && bytes[0] == 0xFF && bytes[1] == 0x15) {
                        // well this definitely is not the right one, indirect calls have never called the function we wanted (I think)
                        SPDLOG_INFO("Found indirect call @ {:x}, skipping", ip);
                        next_call_is_not_the_right_one = true;
                    }

                    if (is_call && !next_call_is_not_the_right_one) {
                        const auto post_call = (uintptr_t)ip + decoded->Length;
                        SPDLOG_INFO("AllocateRenderTargetTexture post_call: {:x}, rel {:x}", post_call, post_call - (uintptr_t)*utility::get_module_within((void*)post_call));

                        if (*(uint8_t*)ip == 0xE8) {
                            SPDLOG_INFO("E8 call found!");
                            this->is_pre_texture_call_e8 = true;
                        } else {
                            SPDLOG_INFO("E8 call not found, assuming register call!");
                        }

                        // So we can call the original texture create function again.
                        this->texture_create_insn_bytes.resize(decoded->Length);
                        memcpy(this->texture_create_insn_bytes.data(), (void*)ip, decoded->Length);

                        if (this->is_version_greq_5_1 && !this->is_pre_texture_call_e8 && bytes[-7] == 0x48 && bytes[-6] == 0x8B && bytes[-5] == 0x0D && bytes[0] == 0xFF && bytes[1] == 0x94) {
                            // Scan forward for a similar one and also hook that
                            auto second_call = utility::scan((uintptr_t)ip + decoded->Length, 0x60, "48 8B 0D ? ? ? ? FF 94 ? ? ? ? ?");

                            if (second_call) {
                                // So we can call the original texture create function again.
                                this->texture_create_insn_bytes2.resize(decoded->Length);
                                memcpy(this->texture_create_insn_bytes2.data(), (void*)(*second_call + 7), decoded->Length);

                                SPDLOG_INFO("Found second call at {:x}", *second_call);
                                auto post_second_call = *second_call + 7 + decoded->Length;
                                //auto texture_hook_result = safetyhook::MidHook::create((void*)post_second_call, &VRRenderTargetManager::texture_hook_callback);
                                auto texture_hook_result = safetyhook::MidHook::create((void*)post_second_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::texture_hook_callback(ctx, true);
                                });

                                if (!texture_hook_result.has_value()) {
                                    const auto e = texture_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create post second texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create post second texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->texture_hook2 = std::move(texture_hook_result.value());
                                    SPDLOG_INFO("Successfully created second texture hook!");
                                }

                                auto pre_second_call = *second_call + 7;
                                //auto pre_texure_hook_result = safetyhook::MidHook::create((void*)pre_second_call, &VRRenderTargetManager::pre_texture_hook_callback);
                                auto pre_texure_hook_result = safetyhook::MidHook::create((void*)pre_second_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::pre_texture_hook_callback(ctx, true);
                                });

                                if (!pre_texure_hook_result.has_value()) {
                                    const auto e = pre_texure_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create pre second texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create pre second texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->pre_texture_hook2 = std::move(pre_texure_hook_result.value());
                                    SPDLOG_INFO("Successfully created second pre texture hook!");
                                }
                            } else {
                                SPDLOG_INFO("Second call not detected! Continuing...");
                            }
                        }

                        //auto texture_hook_result = safetyhook::MidHook::create((void*)post_call, &VRRenderTargetManager::texture_hook_callback);
                        auto texture_hook_result = safetyhook::MidHook::create((void*)post_call, +[](safetyhook::Context& ctx) -> void {
                            VRRenderTargetManager::texture_hook_callback(ctx, false);
                        });

                        if (!texture_hook_result.has_value()) {
                            const auto e = texture_hook_result.error();

                            if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                SPDLOG_ERROR("Failed to create post texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                            } else {
                                SPDLOG_ERROR("Failed to create post texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                            }
                        } else {
                            this->texture_hook = std::move(texture_hook_result.value());
                        }

                        //auto pre_texure_hook_result = safetyhook::MidHook::create((void*)ip, &VRRenderTargetManager::pre_texture_hook_callback);
                        auto pre_texure_hook_result = safetyhook::MidHook::create((void*)ip, +[](safetyhook::Context& ctx) -> void {
                            VRRenderTargetManager::pre_texture_hook_callback(ctx, false);
                        });

                        if (!pre_texure_hook_result.has_value()) {
                            const auto e = pre_texure_hook_result.error();

                            if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                SPDLOG_ERROR("Failed to create pre texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                            } else {
                                SPDLOG_ERROR("Failed to create pre texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                            }
                        } else {
                            this->pre_texture_hook = std::move(pre_texure_hook_result.value());
                        }
                        this->set_up_texture_hook = true;

                        return false;
                    }

                    SPDLOG_INFO("Skipping write to memory instruction at {:x} ({:x} bytes, landing at {:x})", ip, decoded->Length, ip + decoded->Length);
                    emu.ctx->Registers.RegRip += decoded->Length;
                    emu.ctx->Instruction = *decoded; // pseudo-emulate the instruction
                    ++emu.ctx->InstructionsCount;

                    if (is_call) {
                        next_call_is_not_the_right_one = false;
                    }
                } else if (emu.emulate() != SHEMU_SUCCESS) { // only emulate the non-memory write instructions
                    SPDLOG_INFO("Emulation failed at {:x} ({:x} bytes, landing at {:x})", ip, decoded->Length, ip + decoded->Length);
                    // instead of just adding it onto the RegRip, we need to use the ip we had previously from the decode
                    // because the emulator can move the instruction pointer after emulate() is called
                    emu.ctx->Registers.RegRip = ip + decoded->Length;
                    continue;
                }
            } else {
                break;
            }
        }

        SPDLOG_ERROR("Failed to find call instruction!");
    }

    return false;
}

bool VRRenderTargetManager::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples) {
    // So, what's happening here is instead of using this method
    // to actually create our textures, we are going to
    // get the return address, scan forward for the next call instruction
    // and insert a midhook after the next call instruction.
    // The purpose of this is to get the texture that is being created
    // by the engine itself after we return false from this function.
    // When we return false from this function, it indicates
    // to the engine that we are letting the engine itself
    // create the texture, rather than us creating it ourselves.
    // This should allow maximum compatibility across engine versions.
    /*const auto dynamic_rhi = *(uintptr_t*)((uintptr_t)sdk::get_ue_module(L"Engine") + 0x3309C50);
    const auto command_list = (uintptr_t)sdk::get_ue_module(L"Engine") + 0x330AE70;
    struct {
        void* bulk_data{nullptr};
        void* rsrc_array{nullptr};

        struct {
            uint32_t color_binding{1};
            float color[4]{};
        } clear_value_binding;

        uint32_t gpu_mask{1};
        bool without_native_rsrc{false};
        const TCHAR* debug_name{"BufferedRT"};
        uint32_t extended_data{};
    } create_info;

    const void (*RHICreateTexture2D_RenderThread)(
        uintptr_t rhi,
        FTexture2DRHIRef* out,
        uintptr_t command_list,
        uint32_t w,
        uint32_t h,
        uint8_t format,
        uint32_t mips,
        uint32_t samples,
        ETextureCreateFlags flags,
        void* create_info) = (*(decltype(RHICreateTexture2D_RenderThread)**)dynamic_rhi)[178];

    *(uint64_t*)&TargetableTextureFlags |= (uint64_t)ETextureCreateFlags::ShaderResource | (uint64_t)Flags;
    RHICreateTexture2D_RenderThread(dynamic_rhi, &OutTargetableTexture, command_list, SizeX, SizeY, 2, NumMips, NumSamples, TargetableTextureFlags, &create_info);

    const auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();
    RHICreateTexture2D_RenderThread(dynamic_rhi, &OutShaderResourceTexture, command_list, (uint32_t)size.x, (uint32_t)size.y, 2, NumMips, NumSamples, TargetableTextureFlags, &create_info);

    this->render_target = OutTargetableTexture.texture;
    this->ui_target = OutShaderResourceTexture.texture;

    OutShaderResourceTexture.texture = OutTargetableTexture.texture;*/

    m_last_allocate_render_target_return_address = (uintptr_t)_ReturnAddress();
    SPDLOG_INFO("AllocateRenderTargetTexture called from: {:x}", m_last_allocate_render_target_return_address - (uintptr_t)*utility::get_module_within((void*)m_last_allocate_render_target_return_address));

    // So, if CalculateRenderTargetSize was *never* called before this function
    // that means we have the virtual index of this function wrong, and we must swap the vtable out.
    // also, if this function was called very close to NeedReallocateDepthTexture, that also means
    // the virtual index is wrong, and we must swap the vtable out.
    const auto is_incorrect_vtable = 
        m_last_calculate_render_size_return_address == 0 ||
        m_last_allocate_render_target_return_address - m_last_needs_reallocate_depth_texture_return_address <= 0x200;

    if (is_incorrect_vtable) {
        // oh no this is the wrong vtable!!!! we need to fix it  nOW!!!
        SPDLOG_INFO("AllocateRenderTargetTexture called instead of AllocateDepthTexture! Fixing...");
        SPDLOG_INFO("Switching to old render target manager! Incorrect function called!");
        //g_hook->switch_to_old_rendertarget_manager();

        // Do a switcharoo on the vtable of this object to the old one because we will crash if we don't.
        // I've decided against actually switching the entire object over in favor of just vtable
        // swapping for now even though it's kind of a hack.
        const auto fake_object = std::make_unique<VRRenderTargetManager_418>();
        *(void**)this = *(void**)fake_object.get();

        return false;
    }

    this->depth_analysis_passed = true;

    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);

    //return true;
}

bool VRRenderTargetManager_418::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint32_t Flags,
        uint32_t TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture, FTexture2DRHIRef& OutShaderResourceTexture,
        uint32_t NumSamples) 
{
    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);
}

bool VRRenderTargetManager_Special::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples) 
{
    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);
}
