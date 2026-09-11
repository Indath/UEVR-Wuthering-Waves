#pragma once

#define NOMINMAX

#include <memory>
#include <string>
#include <atomic>
#include <optional>

#include <sdk/Math.hpp>

#include "vr/runtimes/OpenVR.hpp"
#include "vr/runtimes/OpenXR.hpp"

#include "vr/D3D11Component.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/OverlayComponent.hpp"

#include "vr/FFakeStereoRenderingHook.hpp"
#include "vr/RenderTargetPoolHook.hpp"
#include "vr/CVarManager.hpp"

#include "Mod.hpp"

#undef max
#include <tracy/Tracy.hpp>

class VR : public Mod {
public:
    enum RenderingMethod {
        NATIVE_STEREO = 0,
        SYNCHRONIZED = 1,
        ALTERNATING = 2,
    };

    enum SynchronizeStage {
        EARLY = 0,
        LATE = 1,
        VERY_LATE = 2,
    };

    enum SyncedSequentialMethod {
        SKIP_TICK = 0,
        SKIP_DRAW = 1,
    };

    enum AimMethod : int32_t {
        GAME,
        HEAD,
        RIGHT_CONTROLLER,
        LEFT_CONTROLLER,
        TWO_HANDED_RIGHT,
        TWO_HANDED_LEFT,
    };

    enum DPadMethod : int32_t {
        RIGHT_TOUCH,
        LEFT_TOUCH,
        LEFT_JOYSTICK,
        RIGHT_JOYSTICK,
        GESTURE_HEAD,
        GESTURE_HEAD_RIGHT,
    };

    enum HORIZONTAL_PROJECTION_OVERRIDE : int32_t {
        HORIZONTAL_DEFAULT,
        HORIZONTAL_SYMMETRIC,
        HORIZONTAL_MIRROR
    };

    enum VERTICAL_PROJECTION_OVERRIDE : int32_t {
        VERTICAL_DEFAULT,
        VERTICAL_SYMMETRIC,
        VERTICAL_MATCHED
    };

    static const inline std::string s_action_pose = "/actions/default/in/Pose";
    static const inline std::string s_action_grip_pose = "/actions/default/in/GripPose";
    static const inline std::string s_action_trigger = "/actions/default/in/Trigger";
    static const inline std::string s_action_grip = "/actions/default/in/Grip";
    static const inline std::string s_action_joystick = "/actions/default/in/Joystick";
    static const inline std::string s_action_joystick_click = "/actions/default/in/JoystickClick";

    static const inline std::string s_action_a_button_left = "/actions/default/in/AButtonLeft";
    static const inline std::string s_action_b_button_left = "/actions/default/in/BButtonLeft";
    static const inline std::string s_action_a_button_touch_left = "/actions/default/in/AButtonTouchLeft";
    static const inline std::string s_action_b_button_touch_left = "/actions/default/in/BButtonTouchLeft";

    static const inline std::string s_action_a_button_right = "/actions/default/in/AButtonRight";
    static const inline std::string s_action_b_button_right = "/actions/default/in/BButtonRight";
    static const inline std::string s_action_a_button_touch_right = "/actions/default/in/AButtonTouchRight";
    static const inline std::string s_action_b_button_touch_right = "/actions/default/in/BButtonTouchRight";

    static const inline std::string s_action_dpad_up = "/actions/default/in/DPad_Up";
    static const inline std::string s_action_dpad_right = "/actions/default/in/DPad_Right";
    static const inline std::string s_action_dpad_down = "/actions/default/in/DPad_Down";
    static const inline std::string s_action_dpad_left = "/actions/default/in/DPad_Left";
    static const inline std::string s_action_system_button = "/actions/default/in/SystemButton";
    static const inline std::string s_action_thumbrest_touch_left = "/actions/default/in/ThumbrestTouchLeft";
    static const inline std::string s_action_thumbrest_touch_right = "/actions/default/in/ThumbrestTouchRight";

public:
    static std::shared_ptr<VR>& get();

    std::string_view get_name() const override { return "VR"; }

    std::optional<std::string> clean_initialize();
    std::optional<std::string> on_initialize_d3d_thread() {
        return clean_initialize();
    }

    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        return {
            {"Runtime", false},
            {"Unreal", false},
            {"Input", false},
            {"Camera", false},
            {"Keybinds", false},
            {"Console/CVars", true},
            {"Compatibility", true},
            {"Debug", true},
        };
    }

    // texture bounds to tell OpenVR which parts of the submitted texture to render (default - use the whole texture).
    // Will be modified to accommodate forced symmetrical eye projection
    vr::VRTextureBounds_t m_right_bounds{0.0f, 0.0f, 1.0f, 1.0f};
    vr::VRTextureBounds_t m_left_bounds{0.0f, 0.0f, 1.0f, 1.0f};

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    
    void on_draw_ui() override;
    void on_draw_sidebar_entry(std::string_view name) override;
    void on_pre_imgui_frame() override;

    void handle_keybinds();
    void on_frame() override;

    void on_present() override;
    void on_post_present() override;

    void on_device_reset() override {
        get_runtime()->on_device_reset();

        if (m_fake_stereo_hook != nullptr) {
            m_fake_stereo_hook->on_device_reset();
        }

        if (m_is_d3d12) {
            m_d3d12.on_reset(this);
        } else {
            m_d3d11.on_reset(this);
        }
    }

    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;
    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override;
    void on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) override;
    void update_imgui_state_from_xinput_state(XINPUT_STATE& state, bool is_vr_controller);

    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;
    void on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                             const float world_to_meters, Vector3f* view_location, bool is_double) override;
    void on_pre_viewport_client_draw(void* viewport_client, void* viewport, void* canvas) override;

    void update_hmd_state(bool from_view_extensions = false, uint32_t frame_count = 0);
    void update_action_states();
    void update_dpad_gestures();

    void reinitialize_renderer() {
        if (m_is_d3d12) {
            m_d3d12.force_reset();
        } else {
            m_d3d11.force_reset();
        }
    }


    Vector4f get_position(uint32_t index, bool grip = true)  const;
    Vector4f get_velocity(uint32_t index)  const;
    Vector4f get_angular_velocity(uint32_t index)  const;
    Matrix4x4f get_hmd_rotation(uint32_t frame_count) const;
    Matrix4x4f get_hmd_transform(uint32_t frame_count) const;
    Matrix4x4f get_rotation(uint32_t index, bool grip = true)  const;
    Matrix4x4f get_transform(uint32_t index, bool grip = true) const;
    vr::HmdMatrix34_t get_raw_transform(uint32_t index) const;

    Vector4f get_grip_position(uint32_t index) const {
        return get_position(index, true);
    }

    Vector4f get_aim_position(uint32_t index) const {
        return get_position(index, false);
    }

    Matrix4x4f get_grip_rotation(uint32_t index) const {
        return get_rotation(index, true);
    }

    Matrix4x4f get_aim_rotation(uint32_t index) const {
        return get_rotation(index, false);
    }

    Matrix4x4f get_grip_transform(uint32_t hand_index) const;
    Matrix4x4f get_aim_transform(uint32_t hand_index) const;

    Vector4f get_eye_offset(VRRuntime::Eye eye) const;
    Vector4f get_current_offset();
    
    Matrix4x4f get_eye_transform(uint32_t index);
    Matrix4x4f get_current_eye_transform(bool flip = false);
    Matrix4x4f get_projection_matrix(VRRuntime::Eye eye, bool flip = false);
    Matrix4x4f get_current_projection_matrix(bool flip = false);

    bool is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;

    bool is_action_active_any_joystick(vr::VRActionHandle_t action) const {
        if (is_action_active(action, m_left_joystick)) {
            return true;
        }

        if (is_action_active(action, m_right_joystick)) {
            return true;
        }

        return false;
    }
    Vector2f get_joystick_axis(vr::VRInputValueHandle_t handle) const;

    vr::VRActionHandle_t get_action_handle(std::string_view action_path) {
        if (auto it = m_action_handles.find(action_path.data()); it != m_action_handles.end()) {
            return it->second;
        }

        return vr::k_ulInvalidActionHandle;
    }

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle);
    
    float get_standing_height();
    Vector4f get_standing_origin();
    void set_standing_origin(const Vector4f& origin);

    glm::quat get_rotation_offset();
    void set_rotation_offset(const glm::quat& offset);
    void recenter_view();
    void recenter_horizon();


    template<typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    runtimes::OpenXR* get_openxr_runtime() const {
        return m_openxr.get();
    }

    runtimes::OpenVR* get_openvr_runtime() const {
        return m_openvr.get();
    }

    // Returns true if UEngine::Tick (game thread) hasn't run recently, which is a strong
    // signal the game is on a loading screen / mid level-transition. This is far more
    // reliable than reading engine-internal bitfields like UWorld::bIsTearingDown, which
    // only reflect the OLD world being torn down and say nothing about new-world/streaming
    // loads still in progress.
    bool is_engine_tick_stalled(std::chrono::milliseconds threshold = std::chrono::milliseconds(750)) const {
        // m_last_engine_tick is default-constructed to the epoch (time_point{}) until the engine's
        // first tick actually occurs. Before that point this must report "not stalled", otherwise
        // callers (e.g. Framework::hook_monitor()'s D3D rehook backoff) would treat pre-hook/pre-tick
        // startup as a permanent loading screen and never escalate to install the initial hook at all.
        if (m_last_engine_tick.time_since_epoch().count() == 0) {
            return false;
        }

        return (std::chrono::steady_clock::now() - m_last_engine_tick) > threshold;
    }

    bool is_diag_verbose_logging_enabled() const {
        return m_diag_verbose_logging;
    }

    bool is_diag_log_pose_refresh_timing_enabled() const {
        return m_diag_log_pose_refresh_timing;
    }

    void set_diag_log_pose_refresh_timing(bool value) {
        m_diag_log_pose_refresh_timing = value;
    }

    uint64_t get_diag_pose_refresh_call_count() const {
        return m_diag_pose_refresh_call_count;
    }

    int get_diag_nsf_pass2_frame_count_mode() const {
        return m_diag_nsf_pass2_frame_count_mode;
    }

    bool& diag_nsf_frame_diff_logger() {
        return m_diag_nsf_frame_diff_logger;
    }

    bool is_hmd_active() const {
        if (m_disable_vr) {
            return false;
        }

        auto runtime = get_runtime();

        if (runtime == nullptr) {
            return false;
        }

        return runtime->ready() || (m_stereo_emulation_mode && runtime->loaded);
    }

    auto get_hmd() const {
        return m_openvr->hmd;
    }

    auto& get_openvr_poses() const {
        return m_openvr->render_poses;
    }

    auto& get_overlay_component() {
        return m_overlay_component;
    }

    uint32_t get_hmd_width() const;
    uint32_t get_hmd_height() const;

    const auto& get_eyes() const {
        return get_runtime()->eyes;
    }

    auto get_frame_count() const {
        return m_frame_count;
    }

    auto& get_controllers() const {
        return m_controllers;
    }

    // NOTE: Deliberately NOT defined inline here. This function is called from many places across
    // the DLL (game thread action-state updates, render-thread overlay/slate quad generation, etc.),
    // and being re-inlined separately at each call site with different surrounding register pressure
    // was implicated in a crash where a float bit-pattern (e.g. 1.0f == 0x3F800000) was misread as
    // part of a pointer, causing an access violation reading a bogus address. Keeping a single
    // out-of-line compiled instance (defined in VR.cpp) avoids that class of inlining/codegen issue.
    bool is_using_controllers() const;

    bool is_using_controllers_within(std::chrono::seconds seconds) const {
        return m_controllers_allowed->value() && is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= seconds;
    }

    int get_hmd_index() const {
        return 0;
    }

    int get_left_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 2 : 1;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[1] : m_controllers[0]) : -1;
        }

        return -1;
    }

    int get_right_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 1 : 2;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[0] : m_controllers[1]) : -1;
        }

        return -1;
    }

    auto get_left_joystick() const {
        if (!m_swap_controllers->value()) {
            return m_left_joystick;
        }

        return m_right_joystick;
    }

    auto get_right_joystick() const {
        if (!m_swap_controllers->value()) {
            return m_right_joystick;
        }

        return m_left_joystick;
    }

    bool is_gui_enabled() const {
        return m_enable_gui->value();
    }

    auto get_camera_forward_offset() const {
        return m_camera_forward_offset->value();
    }

    auto get_camera_right_offset() const {
        return m_camera_right_offset->value();
    }

    auto get_camera_up_offset() const {
        return m_camera_up_offset->value();
    }

    auto get_world_scale() const {
        return m_world_scale->value();
    }

    auto is_stereo_emulation_enabled() const {
        return m_stereo_emulation_mode;
    }

    void reset_present_event() {
        ResetEvent(m_present_finished_event);
    }

    void wait_for_present() {
        if (!m_wait_for_present) {
            return;
        }

        if (m_frame_count <= m_game_frame_count) {
            //return;
        }

        if (WaitForSingleObject(m_present_finished_event, 11) == WAIT_TIMEOUT) {
            //timed_out = true;
        }

        m_game_frame_count = m_frame_count;
        //ResetEvent(m_present_finished_event);
    }

    auto& get_vr_mutex() {
        return m_openvr_mtx;
    }

    bool is_using_afr() const {
        // DIAG: live debug-page toggle to test whether reporting is_using_afr()==false (while
        // staying in Synchronized rendering mode / VR active) restores normal gamepad UI behavior.
        if (m_diag_force_afr_off) {
            return false;
        }

        return m_rendering_method->value() == RenderingMethod::ALTERNATING || 
               m_rendering_method->value() == RenderingMethod::SYNCHRONIZED ||
               m_extreme_compat_mode->value() == true;
    }

    bool is_using_synchronized_afr() const {
        if (m_diag_force_afr_off) {
            return false;
        }

        return m_rendering_method->value() == RenderingMethod::SYNCHRONIZED ||
               (m_extreme_compat_mode->value() && m_rendering_method->value() == RenderingMethod::NATIVE_STEREO);
    }

    // When enabled, AdjustViewRect and calculate_stereo_view_offset stop maintaining their own
    // independent call-scoped eye-parity counters (avr_call_index/offset_call_index) and instead
    // derive true_index directly from the SAME frame-parity source the D3D11/D3D12 compositor uses
    // (m_render_frame_count % 2 == m_left_eye_interval). This eliminates a desync between the render
    // hooks and the compositor that was the root cause of both incorrect 2D-screen/desktop-spectator
    // UI scaling and lost native gamepad UI confirm/navigation input in Synchronized Sequential mode.
    //
    // Confirmed via testing: unifying eye-parity this way breaks true stereoscopic VR rendering
    // (the right eye intermittently goes black and stops tracking the HMD), because the compositor's
    // m_render_frame_count parity is not a safe substitute for the render hooks' own call-scoped
    // alternation in real stereo rendering. It DOES work correctly for the 2D-screen (non-stereo)
    // rendering path, so it is hard-restricted to only take effect while is_using_2d_screen() is true,
    // regardless of the option's value, to guarantee it can never affect true stereo VR rendering.
    bool is_unified_frame_parity_enabled() const {
        return m_unify_afr_frame_parity->value() && is_using_2d_screen();
    }

    // Returns 0 for the left eye, 1 for the right eye, using the compositor's own frame-parity source.
    uint32_t get_unified_true_index() const {
        return (uint32_t)(m_render_frame_count % 2 == m_left_eye_interval ? 0 : 1);
    }

    bool is_synced_forced_second_draw_disabled() const {
        return m_diag_disable_forced_second_draw;
    }

    SynchronizeStage get_synchronize_stage() {
        return (SynchronizeStage) m_sync_mode->value();
    }

    SyncedSequentialMethod get_synced_sequential_method() const {
        return (SyncedSequentialMethod)m_synced_afr_method->value();
    }

    uint32_t get_lowest_xinput_index() const {
        return m_lowest_xinput_user_index;
    }

    auto& get_render_target_pool_hook() const {
        return m_render_target_pool_hook;
    }

    void set_world_to_meters(float value) {
        m_world_to_meters = value;
    }

    float get_world_to_meters() const {
        return m_world_to_meters * m_world_scale->value();
    }

    float get_depth_scale() const {
        return m_depth_scale->value();
    }

    bool is_depth_enabled() const {
        return m_enable_depth->value();
    }

    bool is_decoupled_pitch_enabled() const {
        return m_decoupled_pitch->value();
    }

    bool is_decoupled_pitch_ui_adjust_enabled() const {
        return m_decoupled_pitch_ui_adjust->value();
    }

    void set_decoupled_pitch(bool value) {
        m_decoupled_pitch->value() = value;
    }

    void set_aim_allowed(bool value) {
        m_aim_temp_disabled = !value;
    }

    bool is_aim_allowed() const {
        return !m_aim_temp_disabled;
    }

    AimMethod get_aim_method() const {
        if (m_aim_temp_disabled) {
            return AimMethod::GAME;
        }

        return (AimMethod)m_aim_method->value();
    }

    void set_aim_method(AimMethod method) {
        if ((size_t)method >= s_aim_method_names.size()) {
            method = AimMethod::GAME;
        }

        m_aim_method->value() = method;
    }

    AimMethod get_movement_orientation() const {
        return (AimMethod)m_movement_orientation->value();
    }

    float get_aim_speed() const {
        return m_aim_speed->value();
    }
    
    bool is_aim_multiplayer_support_enabled() const {
        return m_aim_multiplayer_support->value();
    }

    bool is_aim_pawn_control_rotation_enabled() const {
        return m_aim_use_pawn_control_rotation->value();
    }

    bool is_aim_modify_player_control_rotation_enabled() const {
        return m_aim_modify_player_control_rotation->value();
    }

    bool is_aim_interpolation_enabled() const {
        return m_aim_interp->value();
    }
    
    bool is_any_aim_method_active() const {
        return m_aim_method->value() > AimMethod::GAME && !m_aim_temp_disabled;
    }

    bool is_headlocked_aim_enabled() const {
        return m_aim_method->value() == AimMethod::HEAD && !m_aim_temp_disabled;
    }

    bool is_controller_aim_enabled() const {
        const auto value = m_aim_method->value();
        return !m_aim_temp_disabled && (value == AimMethod::LEFT_CONTROLLER || value == AimMethod::RIGHT_CONTROLLER || value == AimMethod::TWO_HANDED_LEFT || value == AimMethod::TWO_HANDED_RIGHT);
    }

    bool is_controller_movement_enabled() const {
        const auto value = m_movement_orientation->value();
        return value == AimMethod::LEFT_CONTROLLER || value == AimMethod::RIGHT_CONTROLLER || value == AimMethod::TWO_HANDED_LEFT || value == AimMethod::TWO_HANDED_RIGHT;
    }

    bool wants_blueprint_load() const {
        return m_load_blueprint_code->value();
    }

    bool is_splitscreen_compatibility_enabled() const {
        return m_splitscreen_compatibility_mode->value();
    }

    uint32_t get_requested_splitscreen_index() const {
        return m_splitscreen_view_index->value();
    }

    bool is_sceneview_compatibility_enabled() const {
        return m_sceneview_compatibility_mode->value();
    }

    bool is_native_stereo_fix_enabled() const {
        // Native Stereo Fix's scene-capture/compositing path was previously gated to only ever run
        // when NOT using AFR/Synchronized-Sequential rendering. Testing showed that this scene-capture
        // path is actually what keeps the 2D-screen UI scaled correctly to the desktop resolution AND
        // keeps gamepad confirm/navigation input working (both were broken in Synced Sequential without
        // it - the UI was rendered at HMD resolution and Slate's hit-testing/focus no longer lined up
        // with what was drawn on screen). Allow it to run even while is_using_afr() is true when this
        // option is enabled.
        const auto raw_value = m_native_stereo_fix->value();
        const auto allow_with_afr = m_native_stereo_fix_allow_with_afr->value();
        const auto using_afr = is_using_afr();
        const auto suspended = m_native_stereo_fix_suspended.load(std::memory_order_relaxed);

        bool result{};

        if (allow_with_afr) {
            result = raw_value && !suspended;
        } else {
            result = raw_value && !using_afr && !suspended;
        }

        // DIAG: the "Enabled" checkbox under Native Stereo Fix can appear unchecked in the UI while
        // the composited/effective result still reads true. Log every input to the decision (not just
        // the final result) so a raw_value=true (checkbox not actually persisted/applied), a stuck
        // suspended=true->false transition, or an unexpected using_afr flip can each be told apart.
        {
            static uint64_t s_diag_call_count = 0;
            static bool s_diag_last_result = false;
            static bool s_diag_has_last_result = false;
            ++s_diag_call_count;

            const auto result_changed = !s_diag_has_last_result || s_diag_last_result != result;

            if (result_changed || s_diag_call_count % 601 == 1) {
                SPDLOG_INFO("[DIAG] is_native_stereo_fix_enabled (#{}): raw_value={} allow_with_afr={} using_afr={} suspended={} -> result={}",
                    s_diag_call_count, raw_value, allow_with_afr, using_afr, suspended, result);
            }

            s_diag_last_result = result;
            s_diag_has_last_result = true;
        }

        return result;
    }

    // Automatically flips Native Stereo Fix off (falling back to its already-working "disabled"
    // compositing path, not the mirror path) while a level transition/loading screen is detected,
    // then flips it back on once things settle. This mirrors the manual A/B toggle workflow that
    // was confirmed to avoid the scene-capture-actor lifecycle conflict during level transitions,
    // but automatically so the user doesn't have to do it by hand every time.
    void set_native_stereo_fix_suspended(bool suspended) {
        m_native_stereo_fix_suspended.store(suspended, std::memory_order_relaxed);
    }

    bool is_native_stereo_fix_suspended() const {
        return m_native_stereo_fix_suspended.load(std::memory_order_relaxed);
    }

    bool is_native_stereo_fix_same_pass_enabled() const {
        return m_native_stereo_fix_same_pass->value();
    }

    // DIAG/EXPERIMENTAL: re-enables the previously-disabled StereoPass=PRIMARY override for the
    // secondary (right-eye) view inside sceneview_constructor. This WAS confirmed in an earlier
    // session to cause full black-screen (main menu characters black, both eyes black in-game)
    // under some conditions, which is why it was disabled and left dormant behind the (now
    // effectively no-op) "Use Same Stereo Pass" toggle. This separate toggle exists purely to
    // re-test that exact code path with decisive added logging around set_stereo_pass() and the
    // FSceneView constructor entry, to correlate exactly which frames/views/conditions the
    // black-screen recurs under. Defaults to OFF - only enable intentionally for diagnostics.
    bool is_native_stereo_fix_same_pass_force_primary_enabled() const {
        return m_native_stereo_fix_same_pass_force_primary->value();
    }

    bool is_native_stereo_fix_right_eye_shadows_enabled() const {
        return m_native_stereo_fix_right_eye_shadows->value();
    }

    bool is_native_stereo_fix_auto_suspend_enabled() const {
        return m_native_stereo_fix_auto_suspend->value();
    }

    bool is_native_stereo_fix_null_pass2_view_state_enabled() const {
        return m_native_stereo_fix_null_pass2_view_state->value();
    }

    bool is_native_stereo_fix_sync_pose_enabled() const {
        return m_native_stereo_fix_sync_pose->value() || m_diag_double_vision_fix_master->value();
    }

    // NOTE: position sync used to be a separate opt-in toggle from rotation sync. Direct in-headset
    // testing proved that freezing ONLY rotation or ONLY position (via the manual Camera Freeze debug
    // tool) still produced the left/right eye desync - freezing BOTH simultaneously was required to
    // eliminate it. This makes sense: Pass1/Pass2 reading a different combined pose (position+rotation
    // together) is the actual divergence, so correcting only one component still leaves the two eyes
    // rendering from different world transforms. Position sync is therefore no longer independently
    // toggleable - it's always applied together with rotation sync (both driven by the single
    // NativeStereoFixSyncPose toggle), since applying only one is now known to not work.
    bool is_native_stereo_fix_sync_pose_position_enabled() const {
        return m_native_stereo_fix_sync_pose->value() || m_diag_double_vision_fix_master->value();
    }

    // Blend factor used when forcing Pass2 (right eye) rotation/position toward Pass1 (left eye)'s
    // cached pose in calculate_stereo_view_offset(). 1.0 = old behavior (Pass2 fully snaps to Pass1's
    // pose, i.e. the right eye completely stops tracking the live animated camera for that frame,
    // which reads as the right eye "lagging"/freezing relative to the left). 0.0 = sync disabled (Pass2
    // keeps its own live pose, full desync). A value in between splits the difference so the right eye
    // still moves with the animation (reducing the disorienting one-sided lag) at the cost of not fully
    // eliminating desync - trading a smaller, more symmetric-feeling error for the previous full/frozen
    // one-eye error.
    float get_native_stereo_fix_sync_pose_blend_alpha() const {
        if (m_diag_double_vision_fix_master->value()) {
            return 1.0f;
        }

        return m_native_stereo_fix_sync_pose_blend_alpha->value();
    }

    // DIAG: test-only toggle. When enabled, a detected hard-cut no longer instantly snaps Pass2's
    // rotation/position to Pass1's pose in a single frame (blend_alpha=1.0 applied once) - instead the
    // effective alpha ramps from 0 up to 1 linearly over
    // get_diag_gradual_hard_cut_convergence_duration_ms(), so the two eyes ease back into agreement
    // over that window instead of one eye teleporting into place. Purely additive/test toggle so it
    // can be A/B compared against the existing instant-snap behavior before deciding whether to make
    // it the default.
    bool is_diag_gradual_hard_cut_convergence_enabled() const {
        return m_diag_gradual_hard_cut_convergence->value();
    }

    float get_diag_gradual_hard_cut_convergence_duration_ms() const {
        return (float)m_diag_gradual_hard_cut_convergence_duration_ms->value();
    }

    // DIAG: see FFakeStereoRenderingHook::is_within_post_hard_cut_window() for full rationale. Normally
    // the full-strength sync-pose snap only applies to the single frame that crosses the hard-cut spike
    // threshold; ordinary sub-threshold jitter during the REST of that same skill/dash/camera-transition
    // animation falls back to the gentler blend_alpha slider. Enabling this keeps the spike detector
    // temporarily more sensitive (lower multiplier) for get_diag_post_hard_cut_sensitivity_boost_window_ms()
    // after a hard cut fires, so residual jitter throughout the same animation also gets fully corrected
    // instead of just the one frame that tripped the original threshold - targeting the momentary blur
    // that remains during dashes/fast turns even with the existing fix.
    bool is_diag_post_hard_cut_sensitivity_boost_enabled() const {
        return m_diag_post_hard_cut_sensitivity_boost->value();
    }

    // How long (ms) after a hard cut fires the boosted (more sensitive) spike multiplier stays active.
    uint64_t get_diag_post_hard_cut_sensitivity_boost_window_ms() const {
        return (uint64_t)m_diag_post_hard_cut_sensitivity_boost_window_ms->value();
    }

    // Replacement spike multiplier used while inside the post-hard-cut boost window (normally 6.0x
    // baseline; lowering this makes the spike test trip on smaller relative deltas).
    float get_diag_post_hard_cut_sensitivity_boost_multiplier() const {
        return (float)m_diag_post_hard_cut_sensitivity_boost_multiplier->value() / 10.0f;
    }

    // DIAG: FIX ATTEMPT #1 for dash/fast-turn blur. Real-capture data (dash-blur capture workflow)
    // showed the blur frames all have rot_delta_deg==0.0000 (no genuine rotational eye mismatch) while
    // pos_delta stays moderately elevated (~15-30 units) EVERY frame because the master fix's
    // blend_alpha=1.0 fallback keeps force-snapping Pass2's position to Pass1's now-stale cached
    // position even when is_hard_cut is false - i.e. it's fighting normal per-eye motion parallax
    // during fast continuous movement, not fixing a real cut. Confirmed real glitches (NumPad2 marks)
    // instead always show substantial rot_delta_deg (2-19+ degrees). Enabling this gates the
    // POSITION-ONLY portion of the sync (rotation sync is unaffected) behind a minimum rot_delta_deg
    // threshold: if the current frame's rot_delta is below the threshold, the position snap/blend is
    // skipped for that frame (falls back to Pass2's own live position), since a real hard cut always
    // shows up rotationally too. A/B test against DiagSustainedMotionPositionSyncSuppression below and
    // against leaving both off (previous behavior).
    bool is_diag_rotation_gated_position_sync_enabled() const {
        return m_diag_rotation_gated_position_sync->value();
    }

    // Minimum rot_delta_deg (see NSF-POSE-DIVERGE logging) required this frame for the position sync
    // to still be applied while DiagRotationGatedPositionSync is enabled. Defaults to 1.0 degree -
    // comfortably above sensor/float noise but far below the smallest real hard-cut rot_delta_deg
    // observed in capture data (~2.4 degrees).
    float get_diag_rotation_gated_position_sync_threshold_deg() const {
        return (float)m_diag_rotation_gated_position_sync_threshold_deg->value() / 10.0f;
    }

    // DIAG: safety ceiling for BOTH position-sync suppression tests below. Real-world capture proved
    // some hard cuts are PURELY positional - a large scripted position jump (e.g. a skill/teleport)
    // with almost no camera rotation (rot_delta_deg observed as low as ~0.08-0.11 degrees, well under
    // the rotation-gate threshold above) - which the rotation gate alone would wrongly classify as
    // dash-like continuous motion and suppress, bringing back double vision (pos_delta observed
    // 300-455+ units in that case, versus 2-90 units during confirmed dash-only frames). Suppression
    // is therefore only allowed when pos_delta is ALSO below this ceiling; a large positional jump
    // always forces the full snap regardless of how small rot_delta is. Defaults to 120 units -
    // comfortably above the largest dash-only pos_delta observed (~90) but well below the smallest
    // pure-position-cut pos_delta observed (~300).
    float get_diag_position_sync_suppression_pos_delta_ceiling() const {
        return (float)m_diag_position_sync_suppression_pos_delta_ceiling->value();
    }

    // DIAG: FIX ATTEMPT #2 for dash/fast-turn blur (alternative to #1 above, can be combined). Instead
    // of gating on rotation every frame, this tracks how many CONSECUTIVE Pass2 calls have had
    // rot_delta_deg below the same threshold above; once that streak reaches the configured frame
    // count, the position-only sync snap is suppressed for the rest of the streak (treating sustained
    // near-zero-rotation divergence as continuous player-driven motion/dash rather than a one-off cut).
    // A single frame with rot_delta_deg above the threshold resets the streak, so a real hard cut
    // (which always shows a rotation spike) still gets the full instant position snap immediately.
    bool is_diag_sustained_motion_position_sync_suppression_enabled() const {
        return m_diag_sustained_motion_position_sync_suppression->value();
    }

    // How many consecutive low-rotation Pass2 frames must elapse before the position sync is
    // suppressed while DiagSustainedMotionPositionSyncSuppression is enabled. Defaults to 3 - enough
    // to ignore a single-frame reporting fluke but still react within ~50-100ms of a dash starting.
    uint32_t get_diag_sustained_motion_position_sync_suppression_frames() const {
        return (uint32_t)m_diag_sustained_motion_position_sync_suppression_frames->value();
    }

    // DIAG: normally the full blend_alpha=1.0 snap only applies on a detected hard-cut-grade spike
    // (see is_nsf_sync_pose_hard_cut()); ordinary small per-frame divergence during continuous motion
    // uses the slider's blend_alpha instead. This proved insufficient for a reported multi-frame
    // divergence (one eye snapping instantly to a UI/menu camera target while the other eye visibly
    // interpolates/zooms back into alignment over ~1 second when a UI menu is opened during/after a
    // skill animation) - so this forces the hard-cut branch (full 1.0 snap, unconditionally) on EVERY
    // Pass2 call while enabled, regardless of the spike-detector's verdict, and logs unconditionally
    // (not throttled) so we can confirm with certainty that the sync is actually engaging on every
    // single frame during the reported glitch window, not just on detected spikes.
    bool is_native_stereo_fix_sync_pose_force_full_enabled() const {
        return m_native_stereo_fix_sync_pose_force_full->value();
    }

    bool is_diag_double_vision_fix_master_enabled() const {
        return m_diag_double_vision_fix_master->value();
    }

    // DIAG: see m_diag_sync_pose_verbose_logging declaration for rationale - gates the high-frequency
    // NSF-POSE-DIVERGE/-TRACE, NSF-SYNC-FORCE-FULL, and NSF-EXCLUDED-INDEX-SYNCED log lines.
    bool is_diag_sync_pose_verbose_logging_enabled() const {
        return m_diag_sync_pose_verbose_logging->value();
    }

    // DIAG: gates the in-headset visual indicator (see D3D12Component's right-eye composite) that
    // flashes when the scene capture RT has been continuously null for a visually-meaningful amount
    // of time and the compositor is presenting a stale last-known-good texture instead. Lets the user
    // confirm in real time whether a perceived blur/double-image moment lines up with this condition.
    bool is_scene_capture_stall_indicator_enabled() const {
        return m_scene_capture_stall_indicator->value();
    }

    // DIAG: user-triggered marker key. When pressed, logs the current timestamp plus the live
    // scene-capture stall state, so a captured log can be greped for the exact moment the user
    // perceived a blur/double-image glitch instead of relying on estimated minute-level timestamps.
    bool is_glitch_marker_key_down() const {
        return GetAsyncKeyState(m_glitch_marker_vkey) & 0x8000;
    }

    // DIAG: frame dump-on-marker BURST. Pressing the glitch marker key arms a TIME-based dump window
    // (not a fixed frame count) so a multi-frame divergence (e.g. one eye snapping instantly to a
    // UI/menu camera target while the other eye visibly interpolates/zooms back into alignment over
    // roughly a second) can be seen unfolding over the ACTUAL reported duration, regardless of the
    // live framerate at the time (a fixed frame count would only cover ~66ms of a ~1s event at 90fps,
    // and would cover an even smaller fraction if framerate dips during the very glitch being
    // captured). The D3D12 compositor (which is the only place that has both the left/game texture
    // and the right/scene-capture texture live at once) checks this once per frame while the window
    // is active and, every Nth frame (see GLITCH_FRAME_DUMP_STRIDE), writes the composited frame to
    // disk tagged with a shared burst id + sequence index - striding avoids ~90 full-resolution disk
    // writes in one second, which could itself introduce stutter that pollutes the very capture we're
    // trying to take.
    static constexpr int64_t GLITCH_FRAME_DUMP_WINDOW_MS = 1200;
    static constexpr uint32_t GLITCH_FRAME_DUMP_STRIDE = 3; // dump every 3rd frame (~30fps sampling at 90fps)

    void request_glitch_frame_dump() {
        m_glitch_frame_dump_burst_id.fetch_add(1, std::memory_order_relaxed);
        m_glitch_frame_dump_seq.store(0, std::memory_order_relaxed);
        m_glitch_frame_dump_stride_counter.store(0, std::memory_order_relaxed);

        const auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + GLITCH_FRAME_DUMP_WINDOW_MS;
        m_glitch_frame_dump_deadline_ms.store(deadline_ms, std::memory_order_relaxed);
    }

    // Returns the next sequence index to use for this dump if a dump should happen this frame
    // (window still active AND this is a stride-selected frame), or std::nullopt otherwise.
    std::optional<uint32_t> consume_glitch_frame_dump_request() {
        const auto deadline_ms = m_glitch_frame_dump_deadline_ms.load(std::memory_order_relaxed);

        if (deadline_ms == 0) {
            return std::nullopt;
        }

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (now_ms >= deadline_ms) {
            // Window expired; disarm so we don't keep checking the clock every frame for nothing.
            m_glitch_frame_dump_deadline_ms.store(0, std::memory_order_relaxed);
            return std::nullopt;
        }

        if ((m_glitch_frame_dump_stride_counter.fetch_add(1, std::memory_order_relaxed) % GLITCH_FRAME_DUMP_STRIDE) != 0) {
            return std::nullopt;
        }

        return m_glitch_frame_dump_seq.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t get_glitch_frame_dump_burst_id() const {
        return m_glitch_frame_dump_burst_id.load(std::memory_order_relaxed);
    }

    // DIAG: opens a short verbose-logging window (see is_glitch_eye_diag_window_active()) alongside
    // the frame dump above. Ability/cutscene VFX (e.g. camera-facing translucent afterimage effects)
    // can desync per-eye stereo separation without the main scene's camera pose/view_rect/generation
    // diverging at all, so this lets us log the actual computed per-eye eye_separation/head_offset
    // vectors in calculate_stereo_view_offset for a few seconds around the marker press, instead of
    // relying on a single-frame snapshot that can't show the effect's per-eye behavior over time.
    void request_glitch_eye_diag_window() {
        const auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
        m_glitch_eye_diag_deadline_ms.store(deadline_ms, std::memory_order_relaxed);
    }

    bool is_glitch_eye_diag_window_active() const {
        const auto deadline_ms = m_glitch_eye_diag_deadline_ms.load(std::memory_order_relaxed);
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return now_ms < deadline_ms;
    }

    // DIAG: dash-blur numeric capture. Two-key workflow: press the ARM key (see
    // m_dash_capture_arm_vkey) the instant a dash/skill camera-transition BEGINS, which starts logging
    // every relevant NSF sync-pose value (rot/pos delta, hard-cut verdict, blend alpha, EMA baseline)
    // EVERY SINGLE FRAME (not throttled) for the next kDashCaptureMaxFrames frames or
    // kDashCaptureTimeoutMs, whichever comes first. Press the MARK key (see m_dash_capture_mark_vkey)
    // the instant the visual blur is actually PERCEIVED, which stamps a distinct log line at that exact
    // frame without stopping the capture, so the surrounding per-frame values can be correlated against
    // the precise moment the blur was seen (not just "sometime during a ~1.5s dash").
    static constexpr int m_dash_capture_arm_vkey = VK_NUMPAD1;
    static constexpr int m_dash_capture_mark_vkey = VK_NUMPAD2;
    static constexpr uint32_t kDashCaptureMaxFrames = 200;
    static constexpr int64_t kDashCaptureTimeoutMs = 3000;

    bool is_dash_capture_arm_key_down() const {
        return GetAsyncKeyState(m_dash_capture_arm_vkey) & 0x8000;
    }

    bool is_dash_capture_mark_key_down() const {
        return GetAsyncKeyState(m_dash_capture_mark_vkey) & 0x8000;
    }

    void request_dash_capture() {
        m_dash_capture_frames_remaining.store(kDashCaptureMaxFrames, std::memory_order_relaxed);
        const auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + kDashCaptureTimeoutMs;
        m_dash_capture_deadline_ms.store(deadline_ms, std::memory_order_relaxed);
        m_dash_capture_seq.store(0, std::memory_order_relaxed);
    }

    void request_dash_capture_mark() {
        m_dash_capture_mark_pending.store(true, std::memory_order_relaxed);
    }

    // Called once per Pass2 (right eye) NSF sync-pose evaluation. Returns the sequence index to tag
    // this frame's log line with if the capture is still active, or std::nullopt if not capturing.
    // marked_out is set true exactly once (the first call after request_dash_capture_mark() was called),
    // so the caller can tag that specific frame's log line as the perceived-blur moment.
    std::optional<uint32_t> consume_dash_capture_frame(bool& marked_out) {
        marked_out = m_dash_capture_mark_pending.exchange(false, std::memory_order_relaxed);

        const auto deadline_ms = m_dash_capture_deadline_ms.load(std::memory_order_relaxed);
        if (deadline_ms == 0) {
            return std::nullopt;
        }

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto frames_remaining = m_dash_capture_frames_remaining.load(std::memory_order_relaxed);

        if (now_ms >= deadline_ms || frames_remaining == 0) {
            m_dash_capture_deadline_ms.store(0, std::memory_order_relaxed);
            return std::nullopt;
        }

        m_dash_capture_frames_remaining.fetch_sub(1, std::memory_order_relaxed);
        return m_dash_capture_seq.fetch_add(1, std::memory_order_relaxed);
    }

    bool is_diag_log_final_eye_pose_enabled() const {
        return m_diag_log_final_eye_pose->value();
    }

    bool is_diag_suppress_extra_view_enabled() const {
        return m_diag_suppress_extra_view->value();
    }

    int32_t get_diag_suppress_view_index() const {
        return m_diag_suppress_view_index->value();
    }

    bool is_diag_log_true_index_alias_enabled() const {
        return m_diag_log_true_index_alias->value();
    }

    bool is_diag_log_world_to_meters_enabled() const {
        return m_diag_log_world_to_meters->value();
    }

    bool is_diag_log_raw_view_index_enabled() const {
        return m_diag_log_raw_view_index->value();
    }

    // See the comment at its usage site in calculate_stereo_view_offset for full rationale: excludes
    // a specific raw view_index from ever writing to/reading from the NSF sync-pose cache (NOT a full
    // render skip - rendering of that index, if any, is untouched). Confirmed via in-headset testing
    // that view_index=1 does not correspond to a visible rendered eye, and it races with the real
    // view_index=3 eye call on the same true_index, plausibly explaining eye-switching glitch reports.
    bool is_diag_exclude_view_index_from_sync_cache_enabled() const {
        return m_diag_exclude_view_index_from_sync_cache->value() || m_diag_double_vision_fix_master->value();
    }

    int32_t get_diag_exclude_view_index_from_sync_cache() const {
        if (m_diag_double_vision_fix_master->value()) {
            return 1;
        }

        return m_diag_exclude_view_index_from_sync_cache_index->value();
    }

    // See usage site in calculate_stereo_view_offset. When the excluded view_index (above) is hit,
    // this controls whether that view still receives the SAME synced rotation/position the real eyes
    // converge to (just without writing its own result back into the cache), instead of being left
    // completely untouched. Intended to fix shadow-cascade/occlusion-culling desync (index=1 appears
    // to feed a sub-view-per-frame system like shadow depth passes, not a renderable eye) while
    // preserving the existing eye blur/desync fix, which relies on that index never writing the cache.
    bool is_diag_apply_synced_pose_to_excluded_view_index_enabled() const {
        return m_diag_apply_synced_pose_to_excluded_view_index->value();
    }

    // Bitmask over sceneview_xref::eye_fields (bit N = flip the N-th discovered field). Default all.
    uint32_t get_diag_nsf_pass2_eye_field_mask() const {
        return (uint32_t)m_diag_nsf_pass2_eye_field_mask;
    }

    // When enabled, Native Stereo Fix never spawns/creates the scene-capture actor/component.
    // The right eye is instead just a mirror of the left (game) texture, via the existing
    // fallback compositing path in D3D11Component/D3D12Component that already runs when the
    // scene capture texture is null. No actor lifetime, no depth, and no level-transition
    // conflicts, at the cost of a flat (non-stereoscopic) right eye.
    bool is_native_stereo_fix_mirror_enabled() const {
        return m_native_stereo_fix_mirror->value();
    }

    // Auto-mirror-on-cinematic: when enabled, the right eye automatically falls back to mirroring
    // the left eye (same mechanism as the manual "Mirror Right Eye" toggle above) whenever the game's
    // own APlayerController::bCinematicMode flag is set, which UE sets true during Sequencer/Matinee
    // cutscenes. This targets the GPU-double-buffered VFX/foliage-wind desync (frozen/laggy right eye
    // during camera transitions) that CPU-side frame-counter fixes could not touch - see
    // is_in_cinematic_mode() for the underlying detection. Independent of the manual mirror toggle so
    // either can be used/tested on its own.
    bool is_native_stereo_fix_auto_mirror_on_cinematic_enabled() const {
        return m_native_stereo_fix_auto_mirror_on_cinematic->value();
    }

    // Auto-mirror-on-UI-blank: when enabled, the right eye automatically falls back to mirroring the
    // left eye whenever LGUI's draw hook has gone silent for at least m_lgui_draw_silence_threshold_ms -
    // i.e. the game's own UI/quad has gone blank. NOTE: in testing this only fired on world load/menu
    // transitions, NOT during the actual skill/VFX animations - kept as an optional secondary trigger
    // (off by default) but superseded by auto-mirror-on-motion below for the animation-blur symptom.
    bool is_native_stereo_fix_auto_mirror_on_ui_blank_enabled() const {
        return m_native_stereo_fix_auto_mirror_on_ui_blank->value();
    }

    // Returns true if LGUI's draw hook has been silent long enough to be treated as "UI blanked" for
    // the auto-mirror trigger. Returns false if the hook has never drawn yet (startup) since that is
    // not a meaningful "blanked" state.
    bool is_ui_blanked_for_mirror_trigger() const;

    // Auto-mirror-on-motion: when enabled, the right eye automatically falls back to mirroring the left
    // eye for a short window after NSF's sync-pose logic (calculate_stereo_view_offset) measures a
    // meaningful Pass1/Pass2 rotation or position delta - i.e. the live animated camera pose itself is
    // changing between the two eyes' render calls this frame. This directly targets "camera motion
    // vectors changing due to animations/VFX", which is the actual condition behind the disorienting
    // right-eye lag (bCinematicMode and UI-blank do not correlate with it). Defaults on.
    bool is_native_stereo_fix_auto_mirror_on_motion_enabled() const {
        return m_native_stereo_fix_auto_mirror_on_motion->value();
    }

    // Returns true if a pose-divergence event happened recently enough (within
    // m_pose_divergence_mirror_window_ms) to be treated as "the camera is currently in an
    // animation/VFX motion window" for the auto-mirror trigger.
    bool is_camera_in_motion_for_mirror_trigger() const {
        if (m_fake_stereo_hook == nullptr) {
            return false;
        }

        return m_fake_stereo_hook->get_ms_since_last_pose_divergence() <= m_pose_divergence_mirror_window_ms;
    }

    // Combined decision for "should the right eye be a mirror of the left eye THIS frame" - true if
    // the manual mirror toggle is on, auto-mirror-on-cinematic is enabled AND bCinematicMode==true,
    // auto-mirror-on-UI-blank is enabled AND LGUI's UI draw has gone silent, or auto-mirror-on-motion is
    // enabled AND the camera pose has recently diverged between eyes (animation/VFX motion).
    bool should_mirror_right_eye_this_frame() {
        if (is_native_stereo_fix_mirror_enabled()) {
            return true;
        }

        if (is_native_stereo_fix_auto_mirror_on_cinematic_enabled() && is_in_cinematic_mode()) {
            return true;
        }

        if (is_native_stereo_fix_auto_mirror_on_ui_blank_enabled() && is_ui_blanked_for_mirror_trigger()) {
            return true;
        }

        if (is_native_stereo_fix_auto_mirror_on_motion_enabled() && is_camera_in_motion_for_mirror_trigger()) {
            return true;
        }

        return false;
    }

    // Polls the local player controller's bCinematicMode bool property (set true by UE's Sequencer/
    // Matinee cutscene system) via the same find_property/FBoolProperty pattern already used elsewhere
    // in this codebase (e.g. bUsePawnControlRotation). Cheap: one controller lookup plus one cached
    // property/bitmask read. Returns false (safe default - no auto-mirror) if the controller, its
    // class, or the property itself cannot be resolved.
    bool is_in_cinematic_mode();

    // Only report the tall-UI target fix as active when Native Stereo Fix itself is active AND the toggle is on.
    // In AFR / NSF OFF the LGUI canvas is not per-eye-tall, so growing the UI target would distort the UI.
    bool is_native_stereo_fix_tall_ui_enabled() const {
        return m_native_stereo_fix_tall_ui->value() && is_native_stereo_fix_enabled();
    }

    bool are_loading_guards_disabled() const {
        return m_disable_loading_guards->value();
    }

    bool is_ahud_compatibility_enabled() const {
        return m_compatibility_ahud->value();
    }

    bool is_ghosting_fix_enabled() const {
        return m_ghosting_fix->value();
    }

    // DIAG/PERF: fully skips the redirected-LGUI-UI per-frame submission (wait/acquire/copy/clear/draw
    // on the UI OpenVR/OpenXR swapchain(s)) so the cost of the redirect itself can be A/B tested at
    // runtime without needing an external Lua script to change how the game renders its UI. The LGUI
    // render target is still captured/redirected upstream (get_ui_target); this only stops us from
    // submitting it to the compositor every frame.
    bool is_lgui_ui_redirect_disabled() const {
        return m_disable_lgui_ui_redirect->value();
    }

    // DIAG: toggles which recovery strategy the generalized game-exe UAF exception handler
    // (FFakeStereoRenderingHook.cpp) uses for WRITE access violations on the known stale/freed-object
    // fault family. "Skip the write" (legacy behavior, this ON) let the game keep running through most
    // occurrences and was stable in the majority of sessions, but was later observed to occasionally
    // convert what would have been a crash into a permanent hang (the skipped write turned out to be
    // load-bearing for game-thread progress in that instance). "Let it crash" (this OFF) removes that
    // mitigation so those faults propagate as a real, immediate, diagnosable crash instead. Default ON
    // to preserve the previously stable behavior; turn OFF only if you want a full crash dump for a
    // specific repro instead of a possible hang.
    bool is_lgui_uaf_store_recovery_disabled() const {
        return m_disable_lgui_uaf_store_recovery->value();
    }

    // DIAG: gate for the forward-store-skip guard added to the generalized MOV/MOVZX load-recovery
    // path in FFakeStereoRenderingHook.cpp. That guard scans a short window past a recovered read and
    // skips a downstream store that uses a register tainted by the zeroed load result, to prevent a
    // follow-on write access violation. It's being A/B tested against a report of a black/no-visual
    // left eye + menu crash: default OFF (guard ACTIVE) is the new behavior; turn this ON to fully
    // disable the guard (falling back to only zeroing the load and letting any follow-on store crash
    // normally) to determine whether this guard is responsible for the left-eye regression.
    bool is_lgui_uaf_load_store_guard_disabled() const {
        return m_disable_lgui_uaf_load_store_guard->value();
    }

    // DIAG: gate for the plausible-float guard in the generalized MOV/MOVZX load-recovery path. That
    // recovery assumes a faulting base register holds a stale/freed pointer and forces the load result
    // to 0, but was observed (rva=23e1f889) faulting on a base register holding the raw bit pattern
    // 0xbf800000 - which is exactly -1.0f as a float, not garbage - suggesting this particular fault is
    // NOT a stale pointer at all, but a legitimate small float (e.g. a per-eye scale/sign constant) that
    // just happens to also be an invalid address when treated as one. Zeroing the load result in that
    // case could corrupt real eye/view scale state and cause a black/no-visual eye, rather than actually
    // recovering a UAF. Default OFF (guard ACTIVE): when the base register's raw bits reinterpret as a
    // plausible small float, skip the zero-forcing recovery entirely and let the fault propagate normally
    // instead. Turn this ON to fully disable the guard (revert to the old unconditional zero-and-recover
    // behavior for every faulting load in this family) to A/B test whether this guard is what's needed to
    // fix the black-eye regression, or whether it's unrelated.
    bool is_lgui_uaf_float_guard_disabled() const {
        return m_disable_lgui_uaf_float_guard->value();
    }

    // DIAG/EXPERIMENT: gates a higher-level alternative to the a3 RDG-texture shadow-copy/quarantine redirect
    // (see LGUI_PATCH_A3_REFS in FFakeStereoRenderingHook.cpp). Instead of duplicating the per-frame transient
    // FRDGTexture object, this hooks the shared, class-level FRenderTarget::GetRenderTargetTexture() vtable slot
    // directly (the same slot resolved by sdk::FRenderTarget::update_offsets/get_render_target_texture_index).
    // CRASH HISTORY: two independent DXGI_ERROR_DEVICE_REMOVED crashes have been traced to actually redirecting
    // this call to ui_target. Rev 1 returned &rtm->get_ui_target() directly - the address of the live, mutable
    // ui_target member - which RDG can capture and dereference later (deferred Execute, possibly off thread); a
    // logged "Tall-UI mode changed... forcing UI target reallocation" reassigning ui_target in that window was
    // followed by device removal. Rev 2 snapshotted the pointer value into a thread_local slot to fix that, but
    // a second crash log showed the redirect firing and, in the same frame, the device being removed while
    // D3D12Component.cpp set up the right-eye stereo blit - i.e. the underlying *resource*, not just the raw
    // pointer, had already been freed/recreated by the time deferred GPU work referencing it actually ran.
    // As a result, lgui_frt_get_render_target_texture_hook() no longer redirects anything regardless of this
    // toggle's value - it only classifies/logs the retaddr and always calls through to the original function.
    // This toggle is kept only to enable/disable that diagnostic logging; a real fix needs the same per-call
    // lifetime/quarantine tracking the a3 shadow-copy pool already implements.
    bool is_lgui_frt_redirect_disabled() const {
        return m_disable_lgui_frt_redirect->value();
    }

    // EXPERIMENT: see the [LGUI_SHADOW_TEX] logging in pre_texture_hook_callback and
    // lgui_frt_get_render_target_texture_hook. When enabled, an additional persistent texture is created
    // (only via the "common version" texture-create branch, the only one observed to fire for this game
    // build) sized to match what the LGUI FRenderTarget redirect call site actually wants, instead of
    // handing it the intentionally-wider ui_target. Default OFF until validated in-headset.
    bool is_lgui_shadow_ui_texture_enabled() const {
        return m_enable_lgui_shadow_ui_texture->value();
    }

    // EXPERIMENT: when a brand-new a3 key is added to the LGUI shadow-copy pool (lgui_copy_pool) -
    // i.e. the pool grows this frame, which reliably correlates with a new UI element/menu/submenu
    // appearing - force-clear the ENTIRE pool (and its quarantine) instead of just adding the new
    // entry. This throws away every existing shadow-copy slot so all currently-visible LGUI content
    // is forced to re-acquire a fresh shadow copy on its very next redirect call, on the theory that
    // stale/mismatched slots left over from a previous UI state are what causes new UI (e.g. opening
    // a menu) to intermittently fail to redirect correctly. Default OFF; this is a blunt, easy-to-try
    // A/B test, not a targeted fix - clearing the whole pool is wasteful if only one entry was
    // actually stale, and could itself cause a brief visual hiccup for content whose copy was still
    // legitimately in use.
    bool is_lgui_clear_pool_on_new_a3_enabled() const {
        return m_enable_lgui_clear_pool_on_new_a3->value();
    }

    // EXPERIMENT: the a2 ref scan in FFakeStereoRenderingHook.cpp currently walks EVERY 8-byte slot from
    // offset 0x0 to 0x3d0 (~122 slots) and patches any slot whose raw value happens to equal the a3 pointer.
    // Ground-truth layout comments in that file identify only THREE offsets (0x270/0x348/0x3a0) as what
    // LGUI's Execute pass actually dereferences to find its render target - the rest of the range is scanned
    // "just in case" a match shows up there too. During a submenu/nested-popup churn burst, addresses get
    // freed and reused fast enough that an unrelated a2 field can transiently hold the same bit pattern as a
    // stale/reused a3 value, causing the blind scan to patch a field that was never actually a render-target
    // reference. When enabled, restrict the scan/patch to exactly the three known-good offsets instead of the
    // whole 0x0-0x3d0 range, eliminating accidental value collisions with unrelated fields entirely (as
    // opposed to trying to validate matches after the fact, which proved unreliable). Default OFF so it can
    // be A/B tested against the existing full-range scan.
    bool is_lgui_restrict_ref_scan_to_known_offsets_enabled() const {
        return m_enable_lgui_restrict_ref_scan_to_known_offsets->value();
    }

    // DIAG/EXPERIMENT: alternative keying strategy for the a3 shadow-copy pool in FFakeStereoRenderingHook.cpp.
    // The existing pool is keyed by the a3 pointer itself - a transient per-frame FRDGTexture WRAPPER object that
    // Unreal's RDG allocator recreates at a new address every single frame (confirmed by [LGUI_A3_CHURN]: hundreds
    // of distinct a3 addresses per session). Every one of those must be individually allocated, tracked, aged out
    // (LGUI_COPY_STALE_FRAMES), and quarantined (LGUI_COPY_QUARANTINE_FRAMES) - and the well-known submenu/nested-
    // popup crashes have all been variations of that bookkeeping racing against the real RDG execute timing, or
    // racing against the engine reusing a freed a3 ADDRESS for a new, unrelated object before our quarantine window
    // closes (see repeated "[LGUI_A3_LIFECYCLE] address REUSE" warnings).
    // [LGUI_RHI_STABILITY] already proves the underlying GPU-backed RHI resource each a3 wraps (a3+RDG_RESOURCE_RHI_OFF)
    // is drawn from a small, STABLE set (typically 2-8 distinct resources, matching eyes/menu layers, with high
    // reuse counts) - i.e. the wrapper churns constantly but the real resource does not. Keying the shadow-copy
    // pool by that stable RHI pointer instead of the transient a3 wrapper pointer means a slot is only created once
    // per real GPU resource (not once per frame), never aged out by a frame-count guess, and never subject to a3-
    // address-reuse confusion, because we never look at the a3 address for identity - only for redirecting THIS
    // call's reference into the resource-keyed slot. Default OFF (old a3-keyed pool active); turn ON to A/B test
    // this against the existing pool for the submenu-crash reproduction case.
    bool is_lgui_rhi_keyed_copy_pool_enabled() const {
        return m_enable_lgui_rhi_keyed_copy_pool->value();
    }

    // DIAG: A/B test to determine whether the runtime's own depth-based compositor reprojection
    // (submitted via XR_KHR_composition_layer_depth, see OpenXR::end_frame) is responsible for a
    // transient double-image/ghosting artifact seen only in-headset (not in screenshots) during
    // animation/camera-cut transitions. Disabling this stops us from ever passing has_depth=true to
    // end_frame, so the runtime falls back to orientation-only reprojection with no depth layer at all.
    bool is_depth_submission_disabled() const {
        return m_disable_depth_submission->value();
    }

    auto& get_fake_stereo_hook() {
        return m_fake_stereo_hook;
    }

    void set_pre_flattened_rotation(const glm::quat& rot) {
        std::unique_lock _{m_decoupled_pitch_data.mtx};
        m_decoupled_pitch_data.pre_flattened_rotation = rot;
    }

    auto get_pre_flattened_rotation() const {
        std::shared_lock _{m_decoupled_pitch_data.mtx};
        return m_decoupled_pitch_data.pre_flattened_rotation;
    }

    bool is_using_2d_screen() const {
        return m_2d_screen_mode->value();
    }

    bool is_roomscale_enabled() const {
        return m_roomscale_movement->value() && !m_aim_temp_disabled;
    }

    bool is_roomscale_sweep_enabled() const {
        return m_roomscale_sweep->value();
    }

    bool is_dpad_shifting_enabled() const {
        return m_dpad_shifting->value();
    }

    DPadMethod get_dpad_method() const {
        return (DPadMethod)m_dpad_shifting_method->value();
    }

    bool is_snapturn_enabled() const {
        return m_snapturn->value();
    }

    void set_snapturn_enabled(bool value) {
        m_snapturn->value() = value;
    }

    float get_snapturn_js_deadzone() const {
        return m_snapturn_joystick_deadzone->value();
    }

    int get_snapturn_angle() const {
        return m_snapturn_angle->value();
    }

    float get_controller_pitch_offset() const {
        return m_controller_pitch_offset->value();
    }

    bool should_skip_post_init_properties() const {
        return m_compatibility_skip_pip->value();
    }
    
    bool should_skip_uobjectarray_init() const {
        return m_compatibility_skip_uobjectarray_init->value();
    }

    bool is_extreme_compatibility_mode_enabled() const {
        return m_extreme_compat_mode->value();
    }

    auto get_horizontal_projection_override() const {
        return m_horizontal_projection_override->value();
    }

    auto get_vertical_projection_override() const {
        return m_vertical_projection_override->value();
    }

    bool should_grow_rectangle_for_projection_cropping() const {
        return m_grow_rectangle_for_projection_cropping->value();
    }

    vrmod::D3D11Component& d3d11() {
        return m_d3d11;
    }

    vrmod::D3D12Component& d3d12() {
        return m_d3d12;
    }

    uint32_t get_present_thread_id() const {
        return m_present_thread_id;
    }

private:
    Vector4f get_position_unsafe(uint32_t index) const;
    Vector4f get_velocity_unsafe(uint32_t index) const;
    Vector4f get_angular_velocity_unsafe(uint32_t index) const;

private:
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();

    bool detect_controllers();
    bool is_any_action_down();

    std::optional<std::string> reinitialize_openvr() {
        spdlog::info("Reinitializing OpenVR");
        std::scoped_lock _{m_openvr_mtx};

        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        m_openvr.reset();

        // Reinitialize openvr input, hopefully this fixes the issue
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openvr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenVR: {}", *e);
        }

        return e;
    }

    std::optional<std::string> reinitialize_openxr() {
        spdlog::info("Reinitializing OpenXR");
        std::scoped_lock _{m_openvr_mtx};

        if (m_is_d3d12) {
            m_d3d12.openxr().destroy_swapchains();
        } else {
            m_d3d11.openxr().destroy_swapchains();
        }

        m_openxr.reset();
        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openxr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenXR: {}", *e);
        }

        return e;
    }

    float m_nearz{ 0.1f };
    float m_farz{ 3000.0f };
    float m_world_to_meters{1.0f}; // Placeholder, it gets set later in a hook

    std::unique_ptr<FFakeStereoRenderingHook> m_fake_stereo_hook{ std::make_unique<FFakeStereoRenderingHook>() };
    std::unique_ptr<RenderTargetPoolHook> m_render_target_pool_hook{ std::make_unique<RenderTargetPoolHook>() };
    std::unique_ptr<CVarManager> m_cvar_manager{ std::make_unique<CVarManager>() };

    void add_components_vr() {
        m_components = {
            m_fake_stereo_hook.get(),
            m_render_target_pool_hook.get(),
            m_cvar_manager.get(),
            &m_overlay_component
        };
    }

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()}; // will point to the real runtime if it exists
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};

    mutable TracyLockable(std::recursive_mutex, m_openvr_mtx);
    mutable TracyLockable(std::recursive_mutex, m_reinitialize_mtx);
    mutable TracyLockable(std::recursive_mutex, m_actions_mtx);
    mutable std::shared_mutex m_rotation_mtx{};

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    glm::vec3 m_overlay_rotation{-1.550f, 0.0f, -1.330f};
    glm::vec4 m_overlay_position{0.0f, 0.06f, -0.07f, 1.0f};
    
    Vector4f m_standing_origin{ 0.0f, 1.5f, 0.0f, 0.0f };
    glm::quat m_rotation_offset{ glm::identity<glm::quat>() };

    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};

    Vector4f m_raw_projections[2]{};

    vrmod::D3D11Component m_d3d11{};
    vrmod::D3D12Component m_d3d12{};
    vrmod::OverlayComponent m_overlay_component;
    bool m_disable_overlay{false};

    // Action set handles
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    // Action handles
    vr::VRActionHandle_t m_action_pose{ };
    vr::VRActionHandle_t m_action_trigger{ };
    vr::VRActionHandle_t m_action_grip{ };
    vr::VRActionHandle_t m_action_grip_pose{ };
    vr::VRActionHandle_t m_action_joystick{};
    vr::VRActionHandle_t m_action_joystick_click{};

    vr::VRActionHandle_t m_action_a_button_right{};
    vr::VRActionHandle_t m_action_a_button_touch_right{};
    vr::VRActionHandle_t m_action_b_button_right{};
    vr::VRActionHandle_t m_action_b_button_touch_right{};

    vr::VRActionHandle_t m_action_a_button_left{};
    vr::VRActionHandle_t m_action_a_button_touch_left{};
    vr::VRActionHandle_t m_action_b_button_left{};
    vr::VRActionHandle_t m_action_b_button_touch_left{};

    vr::VRActionHandle_t m_action_dpad_up{};
    vr::VRActionHandle_t m_action_dpad_right{};
    vr::VRActionHandle_t m_action_dpad_down{};
    vr::VRActionHandle_t m_action_dpad_left{};

    vr::VRActionHandle_t m_action_system_button{};
    vr::VRActionHandle_t m_action_haptic{};
    vr::VRActionHandle_t m_action_thumbrest_touch_left{};
    vr::VRActionHandle_t m_action_thumbrest_touch_right{};

    std::unordered_map<std::string, std::reference_wrapper<vr::VRActionHandle_t>> m_action_handles {
        { s_action_pose, m_action_pose },
        { s_action_grip_pose, m_action_grip_pose },
        { s_action_trigger, m_action_trigger },
        { s_action_grip, m_action_grip },
        { s_action_joystick, m_action_joystick },
        { s_action_joystick_click, m_action_joystick_click },

        { s_action_a_button_left, m_action_a_button_left },
        { s_action_b_button_left, m_action_b_button_left },
        { s_action_a_button_touch_left, m_action_a_button_touch_left },
        { s_action_b_button_touch_left, m_action_b_button_touch_left },

        { s_action_a_button_right, m_action_a_button_right },
        { s_action_b_button_right, m_action_b_button_right },
        { s_action_a_button_touch_right, m_action_a_button_touch_right },
        { s_action_b_button_touch_right, m_action_b_button_touch_right },

        { s_action_dpad_up, m_action_dpad_up },
        { s_action_dpad_right, m_action_dpad_right },
        { s_action_dpad_down, m_action_dpad_down },
        { s_action_dpad_left, m_action_dpad_left },

        { s_action_system_button, m_action_system_button },
        { s_action_thumbrest_touch_left, m_action_thumbrest_touch_left },
        { s_action_thumbrest_touch_right, m_action_thumbrest_touch_right },

        // Out
        { "/actions/default/out/Haptic", m_action_haptic },
    };

    // Input sources
    vr::VRInputValueHandle_t m_left_joystick{};
    vr::VRInputValueHandle_t m_right_joystick{};

    std::chrono::steady_clock::time_point m_last_controller_update{};
    std::chrono::steady_clock::time_point m_last_xinput_update{};
    std::chrono::steady_clock::time_point m_last_xinput_spoof_sent{};
    std::chrono::steady_clock::time_point m_last_xinput_l3_r3_menu_open{};
    std::chrono::steady_clock::time_point m_last_interaction_display{};
    std::chrono::steady_clock::time_point m_last_engine_tick{};

    // See set_native_stereo_fix_suspended()/is_native_stereo_fix_enabled().
    std::atomic<bool> m_native_stereo_fix_suspended{false};

    // See request_glitch_frame_dump()/consume_glitch_frame_dump_request().
    std::atomic<int64_t> m_glitch_frame_dump_deadline_ms{0};
    std::atomic<uint32_t> m_glitch_frame_dump_seq{0};
    std::atomic<uint32_t> m_glitch_frame_dump_stride_counter{0};
    std::atomic<uint32_t> m_glitch_frame_dump_burst_id{0};

    // See request_glitch_eye_diag_window()/is_glitch_eye_diag_window_active().
    std::atomic<int64_t> m_glitch_eye_diag_deadline_ms{0};

    // See request_dash_capture()/request_dash_capture_mark()/consume_dash_capture_frame().
    std::atomic<int64_t> m_dash_capture_deadline_ms{0};
    std::atomic<uint32_t> m_dash_capture_frames_remaining{0};
    std::atomic<uint32_t> m_dash_capture_seq{0};
    std::atomic<bool> m_dash_capture_mark_pending{false};

    uint32_t m_lowest_xinput_user_index{};

    std::chrono::nanoseconds m_last_input_delay{};
    std::chrono::nanoseconds m_avg_input_delay{};

    static const inline std::vector<std::string> s_rendering_method_names {
        "Native Stereo",
        "Synchronized Sequential",
        "Alternating/AFR",
    };

    static const inline std::vector<std::string> s_sync_mode_names{
        "Early",
        "Late",
        "Very Late",
    };

    static const inline std::vector<std::string> s_synced_afr_method_names {
        "Skip Tick",
        "Skip Draw",
    };

    static const inline std::vector<std::string> s_aim_method_names {
        "Game",
        "Head/HMD",
        "Right Controller",
        "Left Controller",
        "Two Handed (Right)",
        "Two Handed (Left)",
    };

    static const inline std::vector<std::string> s_dpad_method_names {
        "Right Thumbrest + Left Joystick",
        "Left Thumbrest + Right Joystick",
        "Left Joystick (Disables Standard Joystick Input)",
        "Right Joystick (Disables Standard Joystick Input)",
        "Gesture (Head) + Left Joystick",
        "Gesture (Head) + Right Joystick",
    };

    static const inline std::vector<std::string> s_horizontal_projection_override_names{
        "Raw / default",
        "Symmetrical",
        "Mirrored",
    };

    static const inline std::vector<std::string> s_vertical_projection_override_names{
        "Raw / default",
        "Symmetrical",
        "Matched",
    };

    const ModCombo::Ptr m_rendering_method{ ModCombo::create(generate_name("RenderingMethod"), s_rendering_method_names) };
    const ModCombo::Ptr m_synced_afr_method{ ModCombo::create(generate_name("SyncedSequentialMethod"), s_synced_afr_method_names, 1) };

    const ModToggle::Ptr m_extreme_compat_mode{ ModToggle::create(generate_name("ExtremeCompatibilityMode"), false, true) };
    const ModToggle::Ptr m_uncap_framerate{ ModToggle::create(generate_name("UncapFramerate"), true) };
    const ModToggle::Ptr m_disable_blur_widgets{ ModToggle::create(generate_name("DisableBlurWidgets"), true) };
    const ModToggle::Ptr m_disable_hdr_compositing{ ModToggle::create(generate_name("DisableHDRCompositing"), true, true) };
    const ModToggle::Ptr m_disable_hzbocclusion{ ModToggle::create(generate_name("DisableHZBOcclusion"), true, true) };
    const ModToggle::Ptr m_disable_instance_culling{ ModToggle::create(generate_name("DisableInstanceCulling"), true, true) };
    // NSF (non-AFR native stereo) never force-disables r.DefaultFeature.MotionBlur the way the AFR
    // path already does (see update_hmd_state's is_using_afr() branch) - motion blur is authored/
    // reprojected for a single mono camera, and its per-pixel velocity buffer does not account for
    // the second eye's independent view; the two eyes' blur trails diverge and read as ghosting/
    // double-vision, most visible during large screen-space motion (fast camera pans, skill/ability
    // VFX, cutscene blends) which is exactly the residual symptom reported after pose-sync fixes.
    const ModToggle::Ptr m_disable_motion_blur_nsf{ ModToggle::create(generate_name("DisableMotionBlurNSF"), true, true) };
    // DIAG: forces r.SkinCache.Mode to 0 (disabled) at runtime. GPU Skin Cache double-buffers bone
    // transform data keyed off "current"/"previous" frame slots; since NSF renders both eyes within
    // the same engine frame as two separate render calls, a per-render-call (rather than per-tick)
    // buffer selection could make one eye sample a stale/previous-frame skeletal pose relative to the
    // other during fast animation (e.g. skill/ability poses), which would show up exactly as a one-
    // eye "snapped to an earlier pose" divergence. Off by default; toggle on to A/B test whether
    // disabling the skin cache changes/removes that artifact. Console command equivalent is
    // "r.SkinCache.Mode 0", exposed here instead since the in-game console is unstable/crashes.
    const ModToggle::Ptr m_disable_skin_cache_nsf{ ModToggle::create(generate_name("DisableSkinCacheNSF"), false) };
    // DIAG: logs the FINAL per-eye camera position/rotation (after head_offset/eye_separation/roomscale
    // are applied - i.e. the actual pose each eye renders from) every single frame, for both eyes,
    // unconditionally (throttled per-eye, not gated behind a glitch-marker window). This lets the two
    // eyes' full camera trajectories be directly compared frame-by-frame across an entire glitch
    // (e.g. a UI-open camera return) instead of only a short capture window, to answer directly: does
    // eye0 snap back to position while eye1 slowly lerps back (or vice versa)? If one eye's logged
    // position visibly lags/interpolates toward the other eye's position across several frames while
    // the other jumps immediately, that pinpoints an asymmetric camera-return interpolation (likely in
    // game/engine code driving the view target, not in our per-eye offset math) as the cause.
    const ModToggle::Ptr m_diag_log_final_eye_pose{ ModToggle::create(generate_name("DiagLogFinalEyePose"), false) };
    // DIAG: log evidence (NSF-FINAL-EYE-POSE captures during a glitch marker press) proves
    // calculate_stereo_view_offset() is called at least THREE times per frame during NSF, not two.
    // IMPORTANT: further captures across separate play sessions PROVED the raw view_index assigned to
    // that third call is NOT stable (it was 3 in one session, matched a REAL eye in another - see
    // m_diag_log_true_index_alias for why), so suppressing by a hardcoded view_index number is
    // unreliable and can suppress a genuine eye (confirmed in-headset: index 2 or 3 sticks one eye to
    // the HMD depending on session). Kept only as a manual, session-specific diagnostic - re-verify the
    // correct index via m_diag_log_final_eye_pose/m_diag_log_true_index_alias EVERY session before
    // using this, do not assume a fixed value like 3 is always correct.
    const ModToggle::Ptr m_diag_suppress_extra_view{ ModToggle::create(generate_name("DiagSuppressExtraView"), false) };
    const ModInt32::Ptr m_diag_suppress_view_index{ ModSliderInt32::create(generate_name("DiagSuppressViewIndex"), -1, 8, 3) };
    // DIAG: true_index (which decides "left" vs "right" and is what the sync-pose cache/eye-offset
    // math key off of) is derived purely from view_index's PARITY: true_index = (view_index + 1) % 2
    // or view_index % 2 depending on index_starts_from_one. That means two DIFFERENT view_index values
    // sharing the same parity (e.g. 1 and 3 once index_starts_from_one is true) ALIAS onto the exact
    // same true_index - so an "extra" call isn't just an ignorable third render, it can silently
    // overwrite/compete with a real eye's cached pose/offset state for that frame. This is a much
    // better candidate root cause for the reported doubled-image/eye desync than a simple stray view.
    // When enabled, logs an unthrottled [VR][NSF-TRUE-INDEX-ALIAS] warning the instant two different
    // view_index values collide onto the same true_index within one engine frame, pinpointing exactly
    // when/how often the aliasing happens without touching or risking any real eye's rendering.
    const ModToggle::Ptr m_diag_log_true_index_alias{ ModToggle::create(generate_name("DiagLogTrueIndexAlias"), false) };
    // DIAG: logs the world_to_meters value passed in on every stereo view-offset call, keyed by the
    // real true_index (so it stays consistent with NSF-TRUE-INDEX-ALIAS), and warns when a call's
    // world_to_meters differs from the last value seen for the OTHER true_index within the same
    // frame. Used to determine whether the reported double-image symptom could be caused by a
    // per-view world-scale mismatch rather than (or in addition to) camera-position/true_index
    // aliasing.
    const ModToggle::Ptr m_diag_log_world_to_meters{ ModToggle::create(generate_name("DiagLogWorldToMeters"), false) };
    // DIAG: unconditionally logs EVERY raw view_index this hook is called with, alongside true_index,
    // is_full_pass, is_using_afr, and a running per-index call count/recency. Used to build a single
    // authoritative table of which raw indices actually occur (observed 0-4 previously) instead of
    // inferring it indirectly from aliasing/pose diagnostics alone.
    const ModToggle::Ptr m_diag_log_raw_view_index{ ModToggle::create(generate_name("DiagLogRawViewIndex"), false) };
    // Off by default until confirmed stable across more sessions; user directly tested that excluding
    // view_index=1 from rendering entirely (a full early-return, stronger than this) caused no visible
    // regression (no stuck/frozen/blank eye). This toggle is the narrower, safer version of that test:
    // it only excludes the index from the sync-pose cache race, not from rendering.
    const ModToggle::Ptr m_diag_exclude_view_index_from_sync_cache{ ModToggle::create(generate_name("DiagExcludeViewIndexFromSyncCache"), false) };
    const ModInt32::Ptr m_diag_exclude_view_index_from_sync_cache_index{ ModSliderInt32::create(generate_name("DiagExcludeViewIndexFromSyncCacheIndex"), -1, 8, 1) };
    // See is_diag_apply_synced_pose_to_excluded_view_index_enabled() for full rationale: feeds the
    // excluded view_index the same converged/synced rotation+position as the real eyes (without
    // letting it write back into the cache), instead of leaving it completely untouched. Intended to
    // fix shadow-cascade/occlusion-culling desync attributed to that index while keeping the eye
    // blur/desync fix from DiagExcludeViewIndexFromSyncCache intact. Only has any effect when
    // DiagExcludeViewIndexFromSyncCache is also enabled.
    const ModToggle::Ptr m_diag_apply_synced_pose_to_excluded_view_index{ ModToggle::create(generate_name("DiagApplySyncedPoseToExcludedViewIndex"), false) };
    const ModToggle::Ptr m_desktop_fix{ ModToggle::create(generate_name("DesktopRecordingFix_V2"), true) };
    const ModToggle::Ptr m_enable_gui{ ModToggle::create(generate_name("EnableGUI"), true) };
    const ModToggle::Ptr m_enable_depth{ ModToggle::create(generate_name("PassDepthToRuntime"), false, true) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    const ModToggle::Ptr m_decoupled_pitch_ui_adjust{ ModToggle::create(generate_name("DecoupledPitchUIAdjust"), true) };
    const ModToggle::Ptr m_load_blueprint_code{ ModToggle::create(generate_name("LoadBlueprintCode"), false, true) };
    const ModToggle::Ptr m_2d_screen_mode{ ModToggle::create(generate_name("2DScreenMode"), false) };
    const ModToggle::Ptr m_roomscale_movement{ ModToggle::create(generate_name("RoomscaleMovement"), false) };
    const ModToggle::Ptr m_roomscale_sweep{ ModToggle::create(generate_name("RoomscaleMovementSweep"), true) };
    const ModToggle::Ptr m_swap_controllers{ ModToggle::create(generate_name("SwapControllerInputs"), false) };
    const ModCombo::Ptr m_horizontal_projection_override{ModCombo::create(generate_name("HorizontalProjectionOverride"), s_horizontal_projection_override_names)};
    const ModCombo::Ptr m_vertical_projection_override{ModCombo::create(generate_name("VerticalProjectionOverride"), s_vertical_projection_override_names)};
    const ModToggle::Ptr m_grow_rectangle_for_projection_cropping{ModToggle::create(generate_name("GrowRectangleForProjectionCropping"), false)};
    const ModCombo::Ptr m_sync_mode{ ModCombo::create(generate_name("SynchronizationMode"), s_sync_mode_names, 2) };

    // Snap turn settings and globals
    void gamepad_snapturn(XINPUT_STATE& state);
    void process_snapturn();
    
    const ModToggle::Ptr m_snapturn{ ModToggle::create(generate_name("SnapTurn"), false) };
    const ModSlider::Ptr m_snapturn_joystick_deadzone{ ModSlider::create(generate_name("SnapturnJoystickDeadzone"), 0.01f, 0.99f, 0.2f) };
    const ModInt32::Ptr m_snapturn_angle{ ModSliderInt32::create(generate_name("SnapturnTurnAngle"), 1, 359, 45) };
    bool m_snapturn_on_frame{false};
    bool m_snapturn_left{false};
    bool m_was_snapturn_run_on_input{false};

    const ModSlider::Ptr m_controller_pitch_offset{ ModSlider::create(generate_name("ControllerPitchOffset"), -90.0f, 90.0f, 0.0f) };

    // Aim method and movement orientation are not the same thing, but they can both have the same options
    const ModCombo::Ptr m_aim_method{ ModCombo::create(generate_name("AimMethod"), s_aim_method_names, AimMethod::GAME) };
    const ModCombo::Ptr m_movement_orientation{ ModCombo::create(generate_name("MovementOrientation"), s_aim_method_names, AimMethod::GAME) };
    AimMethod m_previous_aim_method{ AimMethod::GAME };
    const ModToggle::Ptr m_aim_use_pawn_control_rotation{ ModToggle::create(generate_name("AimUsePawnControlRotation"), false) };
    const ModToggle::Ptr m_aim_modify_player_control_rotation{ ModToggle::create(generate_name("AimModifyPlayerControlRotation"), false) };
    const ModToggle::Ptr m_aim_multiplayer_support{ ModToggle::create(generate_name("AimMPSupport"), false) };
    const ModToggle::Ptr m_aim_interp{ ModToggle::create(generate_name("AimInterp"), true, true) };
    const ModSlider::Ptr m_aim_speed{ ModSlider::create(generate_name("AimSpeed"), 0.01f, 25.0f, 15.0f) };
    const ModToggle::Ptr m_dpad_shifting{ ModToggle::create(generate_name("DPadShifting"), true) };
    const ModCombo::Ptr m_dpad_shifting_method{ ModCombo::create(generate_name("DPadShiftingMethod"), s_dpad_method_names, DPadMethod::RIGHT_TOUCH) };
    
    struct DPadGestureState {
        std::recursive_mutex mtx{};
        enum Direction : uint8_t {
            NONE,
            UP = 1 << 0,
            RIGHT = 1 << 1,
            DOWN = 1 << 2,
            LEFT = 1 << 3,
        };
        uint8_t direction{NONE};
    } m_dpad_gesture_state{};

    //const ModToggle::Ptr m_headlocked_aim{ ModToggle::create(generate_name("HeadLockedAim"), false) };
    //const ModToggle::Ptr m_headlocked_aim_controller_based{ ModToggle::create(generate_name("HeadLockedAimControllerBased"), false) };
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 30.0f) };
    const ModSlider::Ptr m_joystick_deadzone{ ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.2f) };
    const ModSlider::Ptr m_camera_forward_offset{ ModSlider::create(generate_name("CameraForwardOffset"), -4000.0f, 4000.0f, 0.0f) };
    const ModSlider::Ptr m_camera_right_offset{ ModSlider::create(generate_name("CameraRightOffset"), -4000.0f, 4000.0f, 0.0f) };
    const ModSlider::Ptr m_camera_up_offset{ ModSlider::create(generate_name("CameraUpOffset"), -4000.0f, 4000.0f, 0.0f) };
    const ModSlider::Ptr m_camera_fov_distance_multiplier{ ModSlider::create(generate_name("CameraFOVDistanceMultiplier"), 0.00f, 1000.0f, 0.0f) };
    const ModSlider::Ptr m_world_scale{ ModSlider::create(generate_name("WorldScale"), 0.01f, 10.0f, 1.0f) };
    const ModSlider::Ptr m_depth_scale{ ModSlider::create(generate_name("DepthScale"), 0.01f, 1.0f, 1.0f) };

    const ModToggle::Ptr m_ghosting_fix{ ModToggle::create(generate_name("GhostingFix"), false) };
    // DIAG/PERF: see is_lgui_ui_redirect_disabled().
    const ModToggle::Ptr m_disable_lgui_ui_redirect{ ModToggle::create(generate_name("DisableLGUIUIRedirect"), false) };
    // DIAG: see is_lgui_uaf_store_recovery_disabled(). Default ON (recovery active) to match the
    // legacy behavior that was stable in most sessions before the "let it crash" alternative existed.
    const ModToggle::Ptr m_disable_lgui_uaf_store_recovery{ ModToggle::create(generate_name("DisableLGUIUafStoreRecovery"), false) };
    // DIAG: see is_lgui_uaf_load_store_guard_disabled(). Default OFF (guard active); toggle ON to A/B
    // test whether this guard is the cause of a reported black/no-visual left eye + menu crash.
    const ModToggle::Ptr m_disable_lgui_uaf_load_store_guard{ ModToggle::create(generate_name("DisableLGUIUafLoadStoreGuard"), false) };
    // DIAG: see is_lgui_uaf_float_guard_disabled(). Default OFF (guard active); toggle ON to A/B test
    // whether recovering faults on plausible-float base registers unconditionally (old behavior) is
    // actually needed, or whether skipping the recovery for those fixes a black-eye regression.
    const ModToggle::Ptr m_disable_lgui_uaf_float_guard{ ModToggle::create(generate_name("DisableLGUIUafFloatGuard"), false) };
    // DIAG/EXPERIMENT: see is_lgui_frt_redirect_disabled(). Default ON (redirect DISABLED). The redirect now
    // returns the render-target-manager's persistent ui_target member (not a pointer tied to a3's transient
    // lifetime, which caused the earlier DXGI_ERROR_DEVICE_REMOVED crashes), gated the same way as the
    // proven-safe AHUD-compatibility viewport hook. Toggle OFF to enable this redirect (skips the a3
    // shadow-copy pool for this call path while active).
    const ModToggle::Ptr m_disable_lgui_frt_redirect{ ModToggle::create(generate_name("DisableLGUIFrtRedirect"), true) };
    // EXPERIMENT: see is_lgui_shadow_ui_texture_enabled(). Default OFF.
    const ModToggle::Ptr m_enable_lgui_shadow_ui_texture{ ModToggle::create(generate_name("EnableLGUIShadowUiTexture"), false) };
    // EXPERIMENT: see is_lgui_clear_pool_on_new_a3_enabled(). Default OFF.
    const ModToggle::Ptr m_enable_lgui_clear_pool_on_new_a3{ ModToggle::create(generate_name("EnableLGUIClearPoolOnNewA3"), false) };
    // EXPERIMENT: see is_lgui_restrict_ref_scan_to_known_offsets_enabled(). Default OFF.
    const ModToggle::Ptr m_enable_lgui_restrict_ref_scan_to_known_offsets{ ModToggle::create(generate_name("EnableLGUIRestrictRefScanToKnownOffsets"), false) };
    // DIAG/EXPERIMENT: see is_lgui_rhi_keyed_copy_pool_enabled(). Default OFF (old a3-pointer-keyed shadow-copy
    // pool active); toggle ON to switch to keying shadow-copy slots by the stable underlying RHI resource instead.
    const ModToggle::Ptr m_enable_lgui_rhi_keyed_copy_pool{ ModToggle::create(generate_name("EnableLGUIRhiKeyedCopyPool"), false) };
    // DIAG: see is_depth_submission_disabled().
    const ModToggle::Ptr m_disable_depth_submission{ ModToggle::create(generate_name("DisableDepthSubmission"), false) };
    const ModToggle::Ptr m_native_stereo_fix{ ModToggle::create(generate_name("NativeStereoFix"), false) };
    // Default OFF: with the real FSceneViewInitOptions offsets now resolved (sceneview_xref), this branch
    // actually executes in this game and null-derefs inside the engine (views->count=0 during construction).
    // The right-eye shadow fix below makes it unnecessary.
    const ModToggle::Ptr m_native_stereo_fix_same_pass{ ModToggle::create(generate_name("NativeStereoFixSamePass"), false) };
    // DIAG/EXPERIMENTAL: see is_native_stereo_fix_same_pass_force_primary_enabled(). Re-arms the
    // StereoPass=PRIMARY override for Pass2 that was previously disabled due to black-screen reports.
    // Default OFF - only for deliberate diagnostic re-testing with the added decisive logging.
    const ModToggle::Ptr m_native_stereo_fix_same_pass_force_primary{ ModToggle::create(generate_name("NativeStereoFixSamePassForcePrimary"), false) };
    // Flip the Pass2 (right eye) FSceneView's eye-identity metadata (StereoPass + cached copy, view index,
    // primary flag) to the left eye's values while it renders, so whole-scene shadows are set up for it.
    // Offsets/values are discovered at runtime by sceneview_xref, never hardcoded. Camera data untouched.
    const ModToggle::Ptr m_native_stereo_fix_right_eye_shadows{ ModToggle::create(generate_name("NativeStereoFixRightEyeShadows"), true) };
    // Auto-suspend NSF (fall back to the non-scene-capture compositing path) while a level transition
    // is detected, and resume once the world settles. Replicates the manual off/on toggle workflow.
    const ModToggle::Ptr m_native_stereo_fix_auto_suspend{ ModToggle::create(generate_name("NativeStereoFixAutoSuspend"), true) };
    // DIAG: render NSF Pass2 with a null FSceneViewState (no occlusion/TAA history) to test whether
    // shared per-view-state history is what freezes distant foliage in the second-rendered eye.
    const ModToggle::Ptr m_native_stereo_fix_null_pass2_view_state{ ModToggle::create(generate_name("NativeStereoFixNullPass2ViewState"), false) };
    // NSF renders both eyes within the same engine frame via two calculate_stereo_view_offset() calls
    // (Pass1=left, Pass2=right). Unlike AFR (which already caches/reuses eye 0's rotation across the
    // frame - see m_last_afr_rotation), NSF has no equivalent: each eye independently reads whatever the
    // live animated camera pose is at the moment it's queried. During a camera-transition animation
    // (cutscene blend, dash, ability camera shift) that pose can change between the two calls within
    // the same frame, producing a momentary left/right eye desync that resolves once the camera holds
    // still. When enabled, cache Pass1 (left)'s rotation/location for this frame and force Pass2
    // (right) to reuse it, tying the two eyes together for that single stereo frame without affecting
    // head tracking/parallax on any other frame.
    const ModToggle::Ptr m_native_stereo_fix_sync_pose{ ModToggle::create(generate_name("NativeStereoFixSyncPose"), false) };
    // DEPRECATED: position sync used to be independently toggleable here, A/B tested against
    // rotation-only sync. Direct in-headset testing (Camera Freeze debug tool) proved freezing only
    // ONE of position/rotation still produced the eye desync - both must move together. Position sync
    // is therefore now unconditionally tied to m_native_stereo_fix_sync_pose (see
    // is_native_stereo_fix_sync_pose_position_enabled()). This ModToggle is kept only so existing
    // saved configs/UI ordering aren't disrupted; its value is no longer read.
    const ModToggle::Ptr m_native_stereo_fix_sync_pose_position{ ModToggle::create(generate_name("NativeStereoFixSyncPosePosition"), false) };
    // See get_native_stereo_fix_sync_pose_blend_alpha(). 1.0 = Pass2 fully snaps to Pass1's pose
    // (default, matches original behavior); lower values let Pass2 keep some of its own live motion so
    // any residual lag is split between both eyes instead of the right eye alone appearing frozen.
    const ModSlider::Ptr m_native_stereo_fix_sync_pose_blend_alpha{ ModSlider::create(generate_name("NativeStereoFixSyncPoseBlendAlpha"), 0.0f, 1.0f, 1.0f) };
    // DIAG: see is_native_stereo_fix_sync_pose_force_full_enabled(). Off by default; only for deliberate
    // diagnostic re-testing to confirm the sync path engages on every frame during a reported glitch.
    const ModToggle::Ptr m_native_stereo_fix_sync_pose_force_full{ ModToggle::create(generate_name("NativeStereoFixSyncPoseForceFull"), false) };
    // Single convenience "master" toggle for the known double-image fix combo. Enabling it forces
    // NativeStereoFixSyncPose=true, NativeStereoFixSyncPoseBlendAlpha=1.0, and
    // DiagExcludeViewIndexFromSyncCache=true with its index set to 1 (all applied at once via
    // on_draw_ui's cascade logic when this toggle's checkbox is clicked) - the exact combination of
    // settings confirmed to eliminate the reported eye-pose double-image/blur. Disabling it forces
    // NativeStereoFixSyncPose=false and DiagExcludeViewIndexFromSyncCache=false, stopping all three
    // from firing. The individual toggles remain in the Debug tab's NS Double Vision Diagnostics
    // section for manual override/re-testing; this master toggle is a one-click shortcut over them.
    const ModToggle::Ptr m_diag_double_vision_fix_master{ ModToggle::create(generate_name("DiagDoubleVisionFixMaster"), false) };
    // DIAG: see is_diag_gradual_hard_cut_convergence_enabled(). Off by default - test toggle to A/B
    // the ramped convergence against the existing instant hard-cut snap before making it the default.
    const ModToggle::Ptr m_diag_gradual_hard_cut_convergence{ ModToggle::create(generate_name("DiagGradualHardCutConvergence"), false) };
    const ModInt32::Ptr m_diag_gradual_hard_cut_convergence_duration_ms{ ModSliderInt32::create(generate_name("DiagGradualHardCutConvergenceDurationMs"), 50, 3000, 1000) };
    // DIAG: see is_diag_post_hard_cut_sensitivity_boost_enabled(). Off by default - test toggle to A/B
    // against the plain hard-cut-only correction before making it the default. Window defaults to 500ms
    // (covers most skill/dash animation lengths); multiplier stored *10 as an int slider (default 20 ==
    // 2.0x, vs. the normal 6.0x baseline) since ModSliderInt32 is the available slider type here.
    const ModToggle::Ptr m_diag_post_hard_cut_sensitivity_boost{ ModToggle::create(generate_name("DiagPostHardCutSensitivityBoost"), false) };
    const ModInt32::Ptr m_diag_post_hard_cut_sensitivity_boost_window_ms{ ModSliderInt32::create(generate_name("DiagPostHardCutSensitivityBoostWindowMs"), 50, 2000, 500) };
    const ModInt32::Ptr m_diag_post_hard_cut_sensitivity_boost_multiplier{ ModSliderInt32::create(generate_name("DiagPostHardCutSensitivityBoostMultiplier"), 10, 60, 20) };
    // DIAG: see is_diag_rotation_gated_position_sync_enabled(). Off by default - test toggle to A/B
    // against leaving position sync unconditional and against the sustained-motion approach below.
    // Threshold stored *10 as an int slider (default 10 == 1.0 degree) since ModSliderInt32 is the
    // available slider type here.
    const ModToggle::Ptr m_diag_rotation_gated_position_sync{ ModToggle::create(generate_name("DiagRotationGatedPositionSync"), false) };
    const ModInt32::Ptr m_diag_rotation_gated_position_sync_threshold_deg{ ModSliderInt32::create(generate_name("DiagRotationGatedPositionSyncThresholdDeg"), 1, 100, 10) };
    // DIAG: see get_diag_position_sync_suppression_pos_delta_ceiling(). Shared ceiling for both
    // position-sync suppression tests, since a large pos_delta always indicates a real cut regardless
    // of rotation.
    const ModSlider::Ptr m_diag_position_sync_suppression_pos_delta_ceiling{ ModSlider::create(generate_name("DiagPositionSyncSuppressionPosDeltaCeiling"), 10.0f, 1000.0f, 120.0f) };
    // DIAG: gates the high-frequency sync-pose logging (NSF-POSE-DIVERGE/-TRACE, NSF-SYNC-FORCE-FULL,
    // NSF-EXCLUDED-INDEX-SYNCED) that fires every Pass2 call (throttled to 1-2/sec, but still constant
    // spam for as long as Sync Pose is enabled) - see is_diag_sync_pose_verbose_logging_enabled().
    // Off by default; only meant to be turned on briefly while actively reproducing/tuning the
    // scalers above, since it otherwise floods the log with little added value once the fix combo is
    // already confirmed working.
    const ModToggle::Ptr m_diag_sync_pose_verbose_logging{ ModToggle::create(generate_name("DiagSyncPoseVerboseLogging"), false) };

    // toggle to A/B against the plain rotation-gate approach above and against leaving both off.
    const ModToggle::Ptr m_diag_sustained_motion_position_sync_suppression{ ModToggle::create(generate_name("DiagSustainedMotionPositionSyncSuppression"), false) };
    const ModInt32::Ptr m_diag_sustained_motion_position_sync_suppression_frames{ ModSliderInt32::create(generate_name("DiagSustainedMotionPositionSyncSuppressionFrames"), 1, 30, 3) };
    // DIAG: in-headset visual indicator for the stale-scene-capture-freeze condition (see
    // is_scene_capture_stall_indicator_enabled()). Off by default since it's a diagnostic aid.
    const ModToggle::Ptr m_scene_capture_stall_indicator{ ModToggle::create(generate_name("SceneCaptureStallIndicator"), false) };
    // DIAG: virtual-key code for the manual glitch marker (default: NumPad0). Not exposed as a
    // persistent user-facing setting yet, just a fixed diagnostic keybind.
    static constexpr int m_glitch_marker_vkey = VK_NUMPAD0;
    const ModToggle::Ptr m_native_stereo_fix_mirror{ ModToggle::create(generate_name("NativeStereoFixMirror"), false) };
    // Automatically enables the same mirror-right-eye fallback above whenever the game reports
    // APlayerController::bCinematicMode==true (see is_in_cinematic_mode()). Defaults on since the
    // mirror path is cheaper than full NSF stereo (a straight GPU copy, no second scene render) and
    // targets a GPU-double-buffered VFX/foliage desync class that cannot be fixed via CPU-side state.
    const ModToggle::Ptr m_native_stereo_fix_auto_mirror_on_cinematic{ ModToggle::create(generate_name("NativeStereoFixAutoMirrorOnCinematic"), true) };
    // Automatically enables the mirror-right-eye fallback whenever LGUI's per-frame draw hook has not
    // fired for m_lgui_draw_silence_threshold_ms - i.e. the game's own UI has gone fully blank/redirected
    // away. NOTE: testing showed this only fires on world load/menu transitions, NOT during the actual
    // skill/VFX animations that cause the reported blur, so this is now OFF by default. Kept as an
    // optional secondary trigger; see m_native_stereo_fix_auto_mirror_on_motion below for the trigger
    // that actually correlates with skill/VFX animation camera motion.
    const ModToggle::Ptr m_native_stereo_fix_auto_mirror_on_ui_blank{ ModToggle::create(generate_name("NativeStereoFixAutoMirrorOnUiBlank"), false) };
    // How long LGUI must go without drawing (ms) before we consider the UI "blanked" for the purposes of
    // the auto-mirror-on-UI-blank trigger above. Kept fairly low since normal UI redraw cadence is every
    // frame; a genuine skill/VFX UI-hide window should exceed this quickly.
    static constexpr uint64_t m_lgui_draw_silence_threshold_ms = 150;
    // Automatically enables the mirror-right-eye fallback for a short window after NSF's sync-pose
    // logic (calculate_stereo_view_offset) measures a meaningful Pass1/Pass2 rotation or position delta,
    // i.e. the live camera pose is actually changing between the two eyes' render calls this frame -
    // exactly the "camera motion vectors changing due to animations/VFX" condition. Defaults on: this
    // targets the real symptom window, unlike bCinematicMode/UI-blank above.
    const ModToggle::Ptr m_native_stereo_fix_auto_mirror_on_motion{ ModToggle::create(generate_name("NativeStereoFixAutoMirrorOnMotion"), true) };
    // How long (ms) after the last detected pose-divergence event the auto-mirror-on-motion trigger
    // should keep considering the camera "in motion". A single spike would otherwise only cover one
    // frame; this window smooths that out across a short burst of animation frames without flip-
    // flopping the right-eye source every other frame.
    static constexpr uint64_t m_pose_divergence_mirror_window_ms = 250;
    // When Native Stereo Fix is on, LGUI lays its UI canvas out at the per-eye view rect height (which is taller
    // than the scene RT), so the bottom of the UI is clipped by the normal (scene-sized) UI render target. This
    // grows the UEVR-owned UI target to the full per-eye canvas height so the whole UI is captured, then presents
    // it cropped/stretched back to 16:9. Only valid for NSF ON: in AFR / NSF OFF the canvas is not tall, so growing
    // the target distorts the UI. Gated to NSF ON via is_native_stereo_fix_tall_ui_enabled().
    const ModToggle::Ptr m_native_stereo_fix_tall_ui{ ModToggle::create(generate_name("NativeStereoFixTallUI"), true) };
    // Allows Native Stereo Fix's scene-capture/compositing path (normally exclusive to non-AFR
    // rendering) to also run while Alternate-Frame-Rendering / Synchronized Sequential mode is active.
    // Confirmed to fix both incorrect 2D-screen UI scale (previously locked to HMD resolution) and
    // loss of native gamepad UI confirm/navigation input in Synchronized Sequential mode.
    const ModToggle::Ptr m_native_stereo_fix_allow_with_afr{ ModToggle::create(generate_name("NativeStereoFixAllowWithAFR"), false) };
    // Unifies AFR/Synchronized-Sequential eye-parity computation in AdjustViewRect and
    // calculate_stereo_view_offset with the D3D11/D3D12 compositor's own frame-parity source, fixing
    // incorrect 2D-screen/desktop-spectator UI scaling and loss of native gamepad UI confirm/navigation
    // input in Synchronized Sequential mode. Automatically restricted to 2D-screen mode only; has no
    // effect on true stereoscopic VR rendering (see is_unified_frame_parity_enabled()).
    const ModToggle::Ptr m_unify_afr_frame_parity{ ModToggle::create(generate_name("UnifyAFRFrameParity"), false) };
    const ModToggle::Ptr m_disable_loading_guards{ ModToggle::create(generate_name("DisableLoadingGuards"), false) };

    const ModSlider::Ptr m_custom_z_near{ ModSlider::create(generate_name("CustomZNear"), 0.001f, 100.0f, 0.01f, true) };
    const ModToggle::Ptr m_custom_z_near_enabled{ ModToggle::create(generate_name("EnableCustomZNear"), false, true) };

    const ModToggle::Ptr m_splitscreen_compatibility_mode{ ModToggle::create(generate_name("Compatibility_SplitScreen"), false, true) };
    const ModInt32::Ptr m_splitscreen_view_index{ ModInt32::create(generate_name("SplitscreenViewIndex"), 0, true) };

    const ModToggle::Ptr m_sceneview_compatibility_mode{ ModToggle::create(generate_name("Compatibility_SceneView"), false, true) };

    const ModToggle::Ptr m_compatibility_skip_pip{ ModToggle::create(generate_name("Compatibility_SkipPostInitProperties"), false, true) };
    const ModToggle::Ptr m_compatibility_skip_uobjectarray_init{ ModToggle::create(generate_name("Compatibility_SkipUObjectArrayInit"), false, true) };

    const ModToggle::Ptr m_compatibility_ahud{ ModToggle::create(generate_name("Compatibility_AHUD"), false, true) };

    // Keybinds
    const ModKey::Ptr m_keybind_recenter{ ModKey::create(generate_name("RecenterViewKey")) };
    const ModKey::Ptr m_keybind_recenter_horizon{ ModKey::create(generate_name("RecenterHorizonKey")) };
    const ModKey::Ptr m_keybind_set_standing_origin{ ModKey::create(generate_name("ResetStandingOriginKey")) };

    const ModKey::Ptr m_keybind_load_camera_0{ ModKey::create(generate_name("LoadCamera0Key")) };
    const ModKey::Ptr m_keybind_load_camera_1{ ModKey::create(generate_name("LoadCamera1Key")) };
    const ModKey::Ptr m_keybind_load_camera_2{ ModKey::create(generate_name("LoadCamera2Key")) };

    const ModKey::Ptr m_keybind_toggle_2d_screen{ ModKey::create(generate_name("Toggle2DScreenKey")) };
    const ModKey::Ptr m_keybind_disable_vr{ ModKey::create(generate_name("DisableVRKey")) };
    bool m_disable_vr{false}; // definitely should not be persistent
    bool m_diag_force_afr_off{false}; // definitely should not be persistent
    bool m_diag_disable_forced_second_draw{false}; // definitely should not be persistent
    bool m_diag_verbose_logging{false}; // gates high-frequency [DIAG]/[diag] debug logs added while investigating Synced Sequential (AFR) issues; not useful for Native Stereo Fix debugging; definitely should not be persistent
    // DIAG: logs every update_hmd_state() call with a monotonic counter and requested frame count, to
    // correlate against the matching [DIAG] NSF-POSE-REFRESH-RACE log in calculate_stereo_view_offset_
    // and confirm whether a pose refresh can land in between a same-frame left/right eye call pair.
    // Not persistent.
    bool m_diag_log_pose_refresh_timing{false};
    uint64_t m_diag_pose_refresh_call_count{0};
    // Default 0xFB: F2 (a 0/1 "secondary view" flag @0x2ec in WuWa) is NOT flipped. Flipping it made Pass 2 build far
    // shadow cascades/caster lists with left-eye bounds while rendering the right eye -> far shadows flickering between eyes.
    int m_diag_nsf_pass2_eye_field_mask{0xFB}; // bisect mask over the runtime-discovered eye identity fields flipped by the right-eye shadow fix; not persistent
    int m_diag_nsf_pass2_frame_count_mode{0};
    bool m_diag_nsf_frame_diff_logger{false}; // one-shot: logs per-frame-changing dwords in FSceneView/FSceneViewFamily to locate the foliage wind update Pass2 misses; not persistent

    const ModKey::Ptr m_keybind_toggle_gui{ ModKey::create(generate_name("ToggleSlateGUIKey")) };
    
    const ModString::Ptr m_requested_runtime_name{ ModString::create("Frontend_RequestedRuntime", "unset") };

    const ModToggle::Ptr m_lerp_camera_pitch{ ModToggle::create(generate_name("LerpCameraPitch"), false) };
    const ModToggle::Ptr m_lerp_camera_yaw{ ModToggle::create(generate_name("LerpCameraYaw"), false) };
    const ModToggle::Ptr m_lerp_camera_roll{ ModToggle::create(generate_name("LerpCameraRoll"), false) };
    const ModSlider::Ptr m_lerp_camera_speed{ ModSlider::create(generate_name("LerpCameraSpeed"), 0.01f, 10.0f, 1.0f) };

    std::chrono::high_resolution_clock::time_point m_last_lerp_update{};

    struct DecoupledPitchData {
        mutable std::shared_mutex mtx{};
        glm::quat pre_flattened_rotation{};
    } m_decoupled_pitch_data{};

    struct CameraFreeze {
        glm::vec3 position{};
        glm::vec3 rotation{}; // euler
        bool position_frozen{false};
        bool rotation_frozen{false};

        bool position_wants_freeze{false};
        bool rotation_wants_freeze{false};
    } m_camera_freeze{};

    struct CameraLerp {
        glm::vec3 last_position{};
        glm::vec3 last_rotation{};
    } m_camera_lerp{};

    struct CameraData {
        glm::vec3 offset{};
        float world_scale{1.0f};
        bool decoupled_pitch{false};
        bool decoupled_pitch_ui_adjust{true};
    };
    std::array<CameraData, 3> m_camera_datas{};
    void save_cameras();
    void load_cameras();
    void load_camera(int index);
    void save_camera(int index);

public:
    VR() {
        m_options = {
            *m_rendering_method,
            *m_synced_afr_method,
            *m_extreme_compat_mode,
            *m_uncap_framerate,
            *m_disable_hdr_compositing,
            *m_disable_hzbocclusion,
            *m_disable_instance_culling,
            *m_disable_motion_blur_nsf,
            *m_disable_skin_cache_nsf,
            *m_diag_log_final_eye_pose,
            *m_diag_suppress_extra_view,
            *m_diag_suppress_view_index,
            *m_diag_log_true_index_alias,
            *m_diag_log_world_to_meters,
            *m_diag_log_raw_view_index,
            *m_diag_exclude_view_index_from_sync_cache,
            *m_diag_exclude_view_index_from_sync_cache_index,
            *m_diag_apply_synced_pose_to_excluded_view_index,
            *m_diag_gradual_hard_cut_convergence,
            *m_diag_gradual_hard_cut_convergence_duration_ms,
            *m_diag_post_hard_cut_sensitivity_boost,
            *m_diag_post_hard_cut_sensitivity_boost_window_ms,
            *m_diag_post_hard_cut_sensitivity_boost_multiplier,
            *m_diag_rotation_gated_position_sync,
            *m_diag_rotation_gated_position_sync_threshold_deg,
            *m_diag_position_sync_suppression_pos_delta_ceiling,
            *m_diag_sustained_motion_position_sync_suppression,
            *m_diag_sustained_motion_position_sync_suppression_frames,
            *m_desktop_fix,
            *m_enable_gui,
            *m_enable_depth,
            *m_decoupled_pitch,
            *m_decoupled_pitch_ui_adjust,
            *m_load_blueprint_code,
            *m_2d_screen_mode,
            *m_roomscale_movement,
            *m_roomscale_sweep,
            *m_swap_controllers,
            *m_horizontal_projection_override,
            *m_vertical_projection_override,
            *m_grow_rectangle_for_projection_cropping,
            *m_snapturn,
            *m_snapturn_joystick_deadzone,
            *m_snapturn_angle,
            *m_controller_pitch_offset,
            *m_aim_method,
            *m_movement_orientation,
            *m_aim_use_pawn_control_rotation,
            *m_aim_modify_player_control_rotation,
            *m_aim_multiplayer_support,
            *m_aim_speed,
            *m_aim_interp,
            *m_dpad_shifting,
            *m_dpad_shifting_method,
            *m_motion_controls_inactivity_timer,
            *m_joystick_deadzone,
            *m_camera_forward_offset,
            *m_camera_right_offset,
            *m_camera_up_offset,
            *m_world_scale,
            *m_depth_scale,
            *m_custom_z_near,
            *m_custom_z_near_enabled,
            *m_ghosting_fix,
            *m_disable_lgui_ui_redirect,
            *m_disable_lgui_uaf_store_recovery,
            *m_disable_depth_submission,
            *m_native_stereo_fix,
            *m_native_stereo_fix_same_pass,
            *m_native_stereo_fix_same_pass_force_primary,
            *m_native_stereo_fix_right_eye_shadows,
            *m_native_stereo_fix_auto_suspend,
            *m_native_stereo_fix_null_pass2_view_state,
            *m_native_stereo_fix_sync_pose,
            *m_native_stereo_fix_sync_pose_position,
            *m_native_stereo_fix_sync_pose_blend_alpha,
            *m_native_stereo_fix_sync_pose_force_full,
            *m_diag_double_vision_fix_master,
            *m_diag_sync_pose_verbose_logging,
            *m_native_stereo_fix_mirror,
            *m_native_stereo_fix_auto_mirror_on_cinematic,
            *m_native_stereo_fix_auto_mirror_on_ui_blank,
            *m_native_stereo_fix_auto_mirror_on_motion,
            *m_native_stereo_fix_tall_ui,
            *m_native_stereo_fix_allow_with_afr,
            *m_unify_afr_frame_parity,
            *m_disable_loading_guards,
            *m_splitscreen_compatibility_mode,
            *m_splitscreen_view_index,
            *m_compatibility_skip_pip,
            *m_compatibility_skip_uobjectarray_init,
            *m_compatibility_ahud,
            *m_sceneview_compatibility_mode,
            *m_keybind_recenter,
            *m_keybind_recenter_horizon,
            *m_keybind_set_standing_origin,
            *m_keybind_load_camera_0,
            *m_keybind_load_camera_1,
            *m_keybind_load_camera_2,
            *m_keybind_toggle_2d_screen,
            *m_keybind_disable_vr,
            *m_keybind_toggle_gui,
            *m_requested_runtime_name,
            *m_show_fps,
            *m_show_statistics,
            *m_controllers_allowed,
            *m_lerp_camera_pitch,
            *m_lerp_camera_yaw,
            *m_lerp_camera_roll,
            *m_lerp_camera_speed,
            *m_sync_mode,
        };

        add_components_vr();
    }

private:
    bool m_stereo_emulation_mode{false}; // not a good config option, just for debugging
    bool m_wait_for_present{true};
    const ModToggle::Ptr m_controllers_allowed{ ModToggle::create(generate_name("ControllersAllowed"), true) };
    bool m_controller_test_mode{false};
    
    const ModToggle::Ptr m_show_fps{ ModToggle::create(generate_name("ShowFPSOverlay"), false) };
    bool m_show_fps_state{false};

    const ModToggle::Ptr m_show_statistics{ ModToggle::create(generate_name("ShowStatsOverlay"), false) };
    bool m_show_statistics_state{false};

    void update_statistics_overlay(sdk::UGameEngine* engine);

    int m_game_frame_count{};
    int m_frame_count{};
    int m_render_frame_count{};
    int m_last_frame_count{-1};
    int m_left_eye_frame_count{0};
    int m_right_eye_frame_count{0};

    bool m_submitted{false};

    // == 1 or == 0
    uint8_t m_left_eye_interval{0};
    uint8_t m_right_eye_interval{1};

    bool m_first_config_load{true};
    bool m_first_submit{true};
    bool m_is_d3d12{false};
    bool m_backbuffer_inconsistency{false};
    bool m_init_finished{false};
    bool m_has_hw_scheduling{false}; // hardware accelerated GPU scheduling
    bool m_spoofed_gamepad_connection{false};
    bool m_aim_temp_disabled{false};

    struct {
        bool draw{false};
        bool was_moving_left{false};
        bool was_moving_right{false};
        uint8_t page{0};
        uint8_t num_pages{3};
    } m_rt_modifier{};

    bool m_disable_projection_matrix_override{ false };
    bool m_disable_view_matrix_override{false};
    bool m_disable_backbuffer_size_override{false};

    uint32_t m_present_thread_id{};

    struct XInputContext {
        struct PadContext {
            using Func = std::function<void(const XINPUT_STATE&, bool is_vr_controller)>;
            std::optional<Func> update{};
            XINPUT_STATE state{};
        };

        PadContext gamepad{};
        PadContext vr_controller{};
        
        TracyLockable(std::recursive_mutex, mtx);

        struct VRState {
            class StickState {
            public:
                bool was_pressed(bool current_state) {
                    if (!current_state) {
                        is_pressed = false;
                        return false;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (is_pressed && now - initial_press > std::chrono::milliseconds(500)) {
                        return true;
                    }

                    if (!is_pressed) {
                        initial_press = now;
                        is_pressed = true;
                        return true;
                    }

                    return false;
                } 
            
            private:
                std::chrono::steady_clock::time_point initial_press{};
                bool is_pressed{false};
            };

            StickState left_stick_up{};
            StickState left_stick_down{};
            StickState left_stick_left{};
            StickState left_stick_right{};
        } vr;

        void enqueue(bool is_vr_controller, const XINPUT_STATE& in_state, PadContext::Func func) {
            ZoneScopedN(__FUNCTION__);

            std::scoped_lock _{mtx};
            if (is_vr_controller) {
                vr_controller.update = func;
                vr_controller.state = in_state;
            } else {
                gamepad.update = func;
                gamepad.state = in_state;
            }
        }

        void update() {
            ZoneScopedN(__FUNCTION__);

            std::scoped_lock _{mtx};

            if (vr_controller.update) {
                (*vr_controller.update)(vr_controller.state, true);
                vr_controller.update.reset();
            }

            if (gamepad.update) {
                (*gamepad.update)(gamepad.state, false);
                gamepad.update.reset();
            }
        }

        bool headlocked_begin_held{false};
        bool menu_longpress_begin_held{false};
        std::chrono::steady_clock::time_point headlocked_begin{};
        std::chrono::steady_clock::time_point menu_longpress_begin{};
    } m_xinput_context{};

    static std::string actions_json;
    static std::string binding_rift_json;
    static std::string bindings_oculus_touch_json;
    static std::string binding_vive;
    static std::string bindings_vive_controller;
    static std::string bindings_knuckles;

    const std::unordered_map<std::string, std::string> m_binding_files {
        { "actions.json", actions_json },
        { "binding_rift.json", binding_rift_json },
        { "bindings_oculus_touch.json", bindings_oculus_touch_json },
        { "binding_vive.json", binding_vive },
        { "bindings_vive_controller.json", bindings_vive_controller },
        { "bindings_knuckles.json", bindings_knuckles }
    };

    friend class vrmod::D3D11Component;
    friend class vrmod::D3D12Component;
    friend class vrmod::OverlayComponent;
    friend class FFakeStereoRenderingHook;
};
