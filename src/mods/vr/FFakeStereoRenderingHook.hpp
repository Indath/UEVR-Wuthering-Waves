#pragma once

#include <memory>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>

#include <SafetyHook.hpp>

#include <utility/PointerHook.hpp>

#include <sdk/StereoStuff.hpp>
#include <sdk/FViewportInfo.hpp>
#include <sdk/threading/ThreadWorker.hpp>
#include <sdk/RHICommandList.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/UObjectReference.hpp>
#include <sdk/AActor.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/UTexture.hpp>

#include "IXRTrackingSystemHook.hpp"

#include "Mod.hpp"

struct FRHICommandListImmediate;
struct VRRenderTargetManager_418;
struct UCanvas;
struct IStereoLayers;

namespace sdk {
struct FSceneViewStateInterface;
class FViewport;
class FCanvas;
class UGameViewportClient;
class AActor;
class UObject;
class USceneCaptureComponent2D;
class UTexture;
class FSceneViewFamily;
class FSceneView;
}

// Injector-specific structure for VRRenderTargetManager that they will all secondarily inherit from
// because different engine versions can have a different IStereoRenderTargetManager virtual table
// so we need a unified way of storing data that can be used for all versions
struct VRRenderTargetManager_Base {
public:
    bool allocate_render_target_texture(uintptr_t return_address, FTexture2DRHIRef* tex, FTexture2DRHIRef* shader_resource);

    uint32_t get_number_of_buffered_frames() const { return 1; }

    bool should_use_separate_render_target() const { return true; }

    void update_viewport(bool use_separate_rt, const sdk::FViewport& vp, class SViewport* vp_widget = nullptr);

    void calculate_render_target_size(const sdk::FViewport& viewport, uint32_t& x, uint32_t& y);
    bool need_reallocate_view_target(const sdk::FViewport& Viewport);
    bool need_reallocate_depth_texture(const void* DepthTarget);

    // True while need_reallocate_view_target() is still debouncing a candidate size change
    // (i.e. has seen a new size but hasn't observed it stable for enough consecutive checks yet).
    // Callers that are deciding whether it's safe to (re)create the scene capture texture must
    // treat this the same as "loading" and defer, otherwise they can create a scene capture sized
    // for the OLD resolution just before the reallocation lands, leaving the compositor with a
    // scene-capture texture and destination eye texture of mismatched size for one or more frames.
    // That mismatch has been observed to fail SRV descriptor heap creation and take down the D3D12
    // device entirely (DXGI_ERROR_DEVICE_REMOVED).
    bool is_view_target_reallocation_pending() const {
        return m_pending_size_stable_count > 0;
    }

public:
    FRHITexture2D*& get_ui_target() { return ui_target; }
    FRHITexture2D* get_render_target() {
        return render_target; 
    }

    FRHITexture2D* get_scene_capture_render_target();
    void set_render_target(FRHITexture2D* rt) { render_target = rt; }

    bool is_ue_5_0_3() const { return is_version_5_0_3; }

    const std::optional<size_t>& get_viewport_force_separate_rt_offset() const { 
        return m_viewport_force_separate_rt_offset; 
    }

    bool create_scene_capture();
    void destroy_scene_capture();

    // True if a scene capture exists but its owning UWorld is no longer the engine's current
    // world (a full level transition happened, e.g. character-select -> game world) or that
    // world is currently tearing down. Used to proactively destroy_scene_capture() ourselves the
    // instant this is detected, instead of relying on GC (previously "fixed" by rooting the actor,
    // which defeated the engine's own world-teardown handshake and stalled the loading screen).
    bool is_scene_capture_world_stale() const;

    sdk::UTexture* get_scene_capture_utexture();

    // True until a short grace period has elapsed since the scene capture render target became
    // usable. Callers doing heavier per-frame work (e.g. the native-stereo-fix "same pass"
    // secondary-view/depth rendering) should treat this as still-loading and fall back to the
    // cheaper single-view path, since committing to that work right as streaming finishes can
    // starve the render thread of the cycles it needs to actually finish loading.
    bool is_scene_capture_in_grace_period() const {
        if (scene_capture_ready_time.time_since_epoch().count() == 0) {
            return true;
        }

        static constexpr auto grace_period = std::chrono::milliseconds(1500);
        return (std::chrono::steady_clock::now() - scene_capture_ready_time) < grace_period;
    }

    sdk::FViewport* get_viewport() const {
        return last_viewport;
    }

    void set_viewport(sdk::FViewport* vp) {
        last_viewport = vp;
    }

protected:
    struct VerifiedFTexture2D {
        VerifiedFTexture2D() = default;
        VerifiedFTexture2D(FRHITexture2D* tex) 
            : texture{tex}
        {
            if (tex != nullptr) {
                original_vtable = *(void**)tex;
            } else {
                original_vtable = nullptr;
            }
        }

        VerifiedFTexture2D& operator=(FRHITexture2D* tex) {
            texture = tex;
            if (tex != nullptr) {
                original_vtable = *(void**)tex;
            } else {
                original_vtable = nullptr;
            }

            return *this;
        }

        operator FRHITexture2D*&() try {
            if (texture == nullptr) {
                return texture;
            }

            // First line of defense against catching an exception
            if (original_vtable != *(void**)texture) {
                texture = nullptr;
                original_vtable = nullptr;
            }

            return texture;
        } catch (...) {
            // welp
            texture = nullptr;
            original_vtable = nullptr;
            return texture;
        }

        FRHITexture2D* texture{nullptr};
        void* original_vtable{nullptr};
    };

    VerifiedFTexture2D ui_target{};
    VerifiedFTexture2D render_target{};
    static void pre_texture_hook_callback(safetyhook::Context& ctx, bool from_second = false); // only used if pixel format cvar is missing
    static void texture_hook_callback(safetyhook::Context& ctx, bool from_second = false);

    FTexture2DRHIRef* texture_hook_ref{nullptr};
    FTexture2DRHIRef* shader_resource_hook_ref{nullptr};
    safetyhook::MidHook pre_texture_hook{}; // only used if pixel format cvar is missing
    safetyhook::MidHook pre_texture_hook2{}; // only used if pixel format cvar is missing
    safetyhook::MidHook texture_hook{};
    safetyhook::MidHook texture_hook2{};
    uint32_t last_texture_index{0};
    bool allocated_views{false};
    bool set_up_texture_hook{false};
    bool is_pre_texture_call_e8{false};
    bool is_using_texture_desc{false};
    bool is_version_greq_5_1{false};
    bool is_version_5_0_3{false};
    bool wants_depth_reallocate{false};
    bool allocate_texture_called{false}; // used to determine if the pretexture hook should go ahead

    uint32_t last_width{0};
    uint32_t last_height{0};

    // Debounce state for need_reallocate_view_target() - tracks a candidate new size until it has
    // been observed stable for several consecutive checks, to avoid reacting to transient/noisy
    // resolution fluctuations (e.g. during loading screens) with an expensive swapchain recreate.
    uint32_t m_pending_width{0};
    uint32_t m_pending_height{0};
    uint32_t m_pending_size_stable_count{0};

    std::vector<uint8_t> texture_create_insn_bytes{};
    std::vector<uint8_t> texture_create_insn_bytes2{};

    std::optional<size_t> m_viewport_force_separate_rt_offset{};
    bool m_attempted_find_force_separate_rt{false};

    sdk::UObjectReference<sdk::AActor> scene_capture_actor{nullptr};
    sdk::UObjectReference<sdk::USceneCaptureComponent2D> scene_capture_component{nullptr};
    sdk::UObjectReference<sdk::UTexture> scene_capture_target{nullptr}; // For custom compatibility rendering
    sdk::UObjectReference<sdk::UTexture> scene_capture_target_rhi_thread{nullptr}; // For custom compatibility rendering
    sdk::UTexture* in_flight_target{nullptr}; // Not a reference because this is basically a barrier against creating a new scene capture target
    sdk::FViewport* last_viewport{nullptr};

    // Throttle for create_scene_capture(). During a level transition the engine tick can flicker
    // (resume for a frame or two, then stall again), which was previously enough to let the loading
    // guards open briefly and re-trigger a full actor-spawn + FRenderTarget rehook cascade. That
    // cascade itself eats enough game-thread time to cause the next stall, creating a self-sustaining
    // loop that starves level streaming of the cycles it needs to finish. Enforce a minimum interval
    // between actual (re)creations here so bursts of flickering ticks can't retrigger it repeatedly.
    std::chrono::steady_clock::time_point last_scene_capture_create_time{};

    // Timestamp of when the scene capture render target most recently became fully usable
    // (i.e. scene_capture_target was assigned on the game thread). The secondary-view/depth
    // "same pass" rendering path is heavier than a simple eye-copy, and turning it on the instant
    // the target becomes valid can still land inside the tail end of level streaming, starving the
    // render thread of the cycles streaming needs to finish (observed as a hard stall on the loading
    // screen). A short grace period after readiness lets streaming settle before we commit to the
    // full stereo/depth path. Reset to epoch whenever the scene capture is destroyed/invalidated.
    std::chrono::steady_clock::time_point scene_capture_ready_time{};

    // The UWorld our scene_capture_actor was spawned into. Used to proactively detect a full
    // level transition (as opposed to sub-level streaming within the same persistent world) so we
    // can tear down the scene capture ourselves the instant the world changes/tears down, instead
    // of relying on Unreal's own GC pass or (previously) rooting the actor - rooting defeats the
    // engine's teardown handshake for the old world and stalls the loading screen. nullptr means
    // no scene capture actor currently exists.
    void* scene_capture_world{nullptr};
};

struct VRRenderTargetManager : IStereoRenderTargetManager, VRRenderTargetManager_Base {
public:
    uint32_t GetNumberOfBufferedFrames() const override { return VRRenderTargetManager_Base::get_number_of_buffered_frames(); }
    virtual bool ShouldUseSeparateRenderTarget() const override { return VRRenderTargetManager_Base::should_use_separate_render_target(); }

    virtual void UpdateViewport(
        bool bUseSeparateRenderTarget, const sdk::FViewport& Viewport, class SViewport* ViewportWidget = nullptr) override 
    {
        VRRenderTargetManager_Base::update_viewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);
    }

    virtual void CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override;
    virtual bool NeedReAllocateDepthTexture(const void* DepthTarget) override; // Not actually used, we are just checking the return address
    virtual bool NeedReAllocateShadingRateTexture(const void* ShadingRateTarget) override; // Not actually used, we are just checking the return address

    virtual bool NeedReAllocateViewportRenderTarget(const sdk::FViewport& Viewport) override {
        return VRRenderTargetManager_Base::need_reallocate_view_target(Viewport);
    }

    // We will use this to keep track of the game-allocated render targets.
    bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
        ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
        FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples = 1) override;

public:
    uintptr_t m_last_calculate_render_size_return_address{0};
    uintptr_t m_last_needs_reallocate_depth_texture_return_address{0};
    uintptr_t m_last_allocate_render_target_return_address{0};

    // Allows signaling to the engine that depth texture reallocation is needed if return address analysis passed.
    bool depth_analysis_passed{false};
};

struct VRRenderTargetManager_418 : IStereoRenderTargetManager_418, VRRenderTargetManager_Base {
    uint32_t GetNumberOfBufferedFrames() const override { return VRRenderTargetManager_Base::get_number_of_buffered_frames(); }
    virtual bool ShouldUseSeparateRenderTarget() const override { return VRRenderTargetManager_Base::should_use_separate_render_target(); }

    virtual void UpdateViewport(bool bUseSeparateRenderTarget, const sdk::FViewport& Viewport, class SViewport* ViewportWidget = nullptr) override {
        VRRenderTargetManager_Base::update_viewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);
    }

    virtual void CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override {
        VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
    }

    virtual bool NeedReAllocateViewportRenderTarget(const sdk::FViewport& Viewport) override {
        return VRRenderTargetManager_Base::need_reallocate_view_target(Viewport);
    }

    virtual bool NeedReAllocateDepthTexture(const void* DepthTarget) override {
        return VRRenderTargetManager_Base::need_reallocate_depth_texture(&DepthTarget);
    }

    // We will use this to keep track of the game-allocated render targets.
    bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint32_t Flags,
        uint32_t TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture, FTexture2DRHIRef& OutShaderResourceTexture,
        uint32_t NumSamples = 1) override;
};

struct VRRenderTargetManager_Special : IStereoRenderTargetManager_Special, VRRenderTargetManager_Base {
    uint32_t GetNumberOfBufferedFrames() const override { return VRRenderTargetManager_Base::get_number_of_buffered_frames(); }
    virtual bool ShouldUseSeparateRenderTarget() const override { return VRRenderTargetManager_Base::should_use_separate_render_target(); }

    virtual void UpdateViewport(bool bUseSeparateRenderTarget, const sdk::FViewport& Viewport, class SViewport* ViewportWidget = nullptr) override {
        VRRenderTargetManager_Base::update_viewport(bUseSeparateRenderTarget, Viewport, ViewportWidget);
    }

    virtual void CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) override {
        VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
    }

    virtual bool NeedReAllocateViewportRenderTarget(const sdk::FViewport& Viewport) override {
        return VRRenderTargetManager_Base::need_reallocate_view_target(Viewport);
    }

    // We will use this to keep track of the game-allocated render targets.
    bool AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
        ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
        FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples = 1) override;
};

class FFakeStereoRenderingHook : public ModComponent {
public:
    FFakeStereoRenderingHook();

    VRRenderTargetManager_Base* get_render_target_manager() {
        if (m_uses_old_rendertarget_manager) {
            return static_cast<VRRenderTargetManager_Base*>(&m_rtm_418);
        }

        if (m_special_detected) {
            return static_cast<VRRenderTargetManager_Base*>(&m_rtm_special);
        }

        return static_cast<VRRenderTargetManager_Base*>(&m_rtm);
    }

    /*void switch_to_old_rendertarget_manager() {
        m_uses_old_rendertarget_manager = true;
    }*/
    
    bool has_pixel_format_cvar() const {
        return m_pixel_format_cvar_found;
    }

    // Region of ui_target (pixels) the redirected LGUI pass actually draws into. Zero when no redirect happened.
    struct UIDrawExtent { int32_t width{0}; int32_t height{0}; };
    UIDrawExtent get_ui_draw_extent() const { return m_ui_draw_extent; }
    void set_ui_draw_extent(int32_t w, int32_t h) { m_ui_draw_extent = {w, h}; }

    // Dynamic size (pixels) that the LGUI ui_target should be allocated at. Decoupled from the shared scene RT size
    // (get_d3d12_rt_size) so the UI target can be made tall enough to hold LGUI's full per-eye canvas (view_rect),
    // which is taller than the scene RT height when NSF is on. The UI is then cropped/stretched back to 16:9 at
    // presentation time. Returns integer width/height. Falls back to the scene RT size if VR is unavailable.
    static UIDrawExtent get_ui_target_size();

    // Game-thread viewport size (FViewport::GetSizeXY) sampled in UGameViewportClient::Draw. LGUI lays out its
    // screen-space canvas / ortho projection from this, so the redirected draw extent must match it.
    UIDrawExtent get_game_viewport_size() const { return m_game_viewport_size; }
    void set_game_viewport_size(int32_t w, int32_t h) { m_game_viewport_size = {w, h}; }

    void attempt_hooking();
    void attempt_hook_game_engine_tick(uintptr_t return_address = 0);
    void attempt_hook_slate_thread(uintptr_t return_address = 0, bool alternate = false);
    void attempt_hook_update_viewport_rhi(uintptr_t return_address);
    void attempt_hook_fsceneview_constructor();
    

    bool has_double_precision() const {
        return m_has_double_precision;
    }

    bool has_attempted_to_hook_engine() const {
        return m_attempted_hook_game_engine_tick;
    }

    bool has_attempted_to_hook_slate() const {
        return m_attempted_hook_slate_thread;
    }

    bool has_attempted_to_hook_fsceneview() const {
        return m_attempted_hook_fsceneview_constructor;
    }

    bool is_slate_hooked() const {
        return m_hooked_slate_thread;
    }

    bool should_recreate_textures() const {
        return m_wants_texture_recreation;
    }

    void set_should_recreate_textures(bool recreate) {
        m_wants_texture_recreation = recreate;
        m_skip_next_adjust_view_rect = true;
    }

    uint64_t get_view_target_generation() const {
        return m_view_target_generation;
    }

    // Called at the moment a view-target/scene-capture reallocation is actually committed (not merely
    // requested). Bumps the "epoch" counter so downstream consumers (scene capture bind, eye composite,
    // OpenXR end_frame submission) can log/verify which texture generation they are operating on, to
    // diagnose ghosting/double-image artifacts caused by stale texture content being composited/
    // submitted across a reallocation boundary.
    uint64_t bump_view_target_generation() {
        return ++m_view_target_generation;
    }

    // DIAG: tracks how long the scene capture render target has been continuously reporting null
    // while the compositor keeps presenting a stale last-known-good texture (see D3D12Component's
    // "keeping last-known-good scene capture texture" path). Used to drive both a manual log marker
    // and a real-time in-headset visual indicator so the user can correlate perceived blur/double-
    // image moments against this specific freeze condition instead of guessing at timestamps.
    void report_scene_capture_stall_frame(bool is_stalled) {
        if (is_stalled) {
            if (m_scene_capture_stall_start == std::chrono::steady_clock::time_point{}) {
                m_scene_capture_stall_start = std::chrono::steady_clock::now();
            }
        } else {
            m_scene_capture_stall_start = std::chrono::steady_clock::time_point{};
        }
    }

    // Returns how long (in ms) the stall has been ongoing, or 0 if not currently stalled.
    uint64_t get_scene_capture_stall_duration_ms() const {
        if (m_scene_capture_stall_start == std::chrono::steady_clock::time_point{}) {
            return 0;
        }

        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_scene_capture_stall_start).count();
    }

    bool is_scene_capture_stalled() const {
        return m_scene_capture_stall_start != std::chrono::steady_clock::time_point{};
    }

    // DIAG/TRIGGER: heartbeat for LGUI's per-frame draw hook (lgui_slot24_hook). Wuthering Waves blanks
    // its LGUI-driven UI/quad during certain skill/VFX animations (the user has observed the UI go blank
    // and return to stock UI once the animation ends) - LGUI's draw call simply stops firing for that
    // window, which is a much more direct signal for "is a skill/VFX animation currently playing" than
    // bCinematicMode (which only reflects Sequencer/Matinee cutscenes, not this kind of skill-driven UI
    // hide). Call this once per LGUI draw invocation to record the heartbeat.
    void report_lgui_draw_heartbeat() {
        m_lgui_last_draw_time = std::chrono::steady_clock::now();
        m_lgui_ever_drawn = true;
    }

    // Returns how long (in ms) it has been since LGUI last actually drew a frame. Returns 0 if LGUI has
    // never drawn yet (e.g. very early boot), so callers should treat that as "not stalled" rather than
    // "infinitely stalled".
    uint64_t get_lgui_draw_silence_duration_ms() const {
        if (!m_lgui_ever_drawn) {
            return 0;
        }

        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_lgui_last_draw_time).count();
    }

    // DIAG/TRIGGER: call whenever calculate_stereo_view_offset's NSF sync-pose branch measures a
    // meaningful Pass1/Pass2 rotation or position delta for the current frame - i.e. the live animated
    // camera pose itself changed between the two eyes' render calls this frame (skill/VFX/dash camera
    // motion), as opposed to being stationary. This is a direct per-frame measurement of the actual
    // "camera motion vector changing due to animation" condition, unlike bCinematicMode/UI-blank which
    // only correlate with unrelated engine states (loading screens, Sequencer cutscenes).
    void report_pose_divergence_event() {
        m_last_pose_divergence_time = std::chrono::steady_clock::now();
        m_had_pose_divergence = true;
    }

    // Returns how long (in ms) it has been since the last pose-divergence event. Returns UINT64_MAX if
    // no divergence has ever been observed yet, so callers can distinguish "never happened" from
    // "happened a long time ago" (both should be treated as "not currently diverging").
    uint64_t get_ms_since_last_pose_divergence() const {
        if (!m_had_pose_divergence) {
            return UINT64_MAX;
        }

        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_last_pose_divergence_time).count();
    }

    // The engine's own AdjustViewRect call decides, independently of D3D12Component/D3D11Component's
    // frame-parity-based is_left_eye_frame classification, which physical x-offset half of the
    // double-wide backbuffer each eye's scene gets rendered into. These two "which eye is this"
    // trackers are computed from unrelated state and can fall out of phase, silently causing the
    // compositor to crop the wrong half of the backbuffer for a given eye. Expose the last-known
    // x-offset actually assigned to each eye by AdjustViewRect so the compositor can crop from the
    // real location instead of assuming a fixed left=0/right=half layout.
    uint32_t get_last_left_eye_x_offset() const {
        return m_last_left_eye_x_offset;
    }

    uint32_t get_last_right_eye_x_offset() const {
        return m_last_right_eye_x_offset;
    }

    // See m_left_eye_x_offset_update_count / m_right_eye_x_offset_update_count above.
    uint64_t get_left_eye_x_offset_update_count() const {
        return m_left_eye_x_offset_update_count;
    }

    uint64_t get_right_eye_x_offset_update_count() const {
        return m_right_eye_x_offset_update_count;
    }

    bool has_seen_eye_x_offsets() const {
        return m_has_seen_eye_x_offsets;
    }

    void on_device_reset() override {
        if (m_recreate_textures_on_reset->value()) {
            m_wants_texture_recreation = true;
        }
    }

    void on_config_load(const utility::Config& cfg, bool set_defaults) {
        for (IModValue& option : m_options) {
            option.config_load(cfg, set_defaults);
        }
    }

    void on_config_save(utility::Config& cfg) {
        for (IModValue& option : m_options) {
            option.config_save(cfg);
        }
    }

    void on_frame() override;
    void on_draw_ui() override;

    auto get_frame_delay_compensation() const {
        return m_frame_delay_compensation->value();
    }

    auto& get_slate_thread_worker() {
        return m_slate_thread_worker;
    }

    bool has_slate_hook() {
        return (bool)m_slate_thread_hook;
    }

    bool has_engine_tick_hook() {
        return m_hooked_game_engine_tick;
    }

    auto& get_embedded_rtm() {
        return m_embedded_rtm;
    }

    bool m_inside_manual_view_offset{false};

    void calculate_stereo_view_offset_(const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        m_inside_manual_view_offset = true;
        calculate_stereo_view_offset(nullptr, view_index, view_rotation, world_to_meters, view_location);
        m_inside_manual_view_offset = false;
    }

    bool is_in_viewport_client_draw() const;

    bool is_ignoring_next_viewport_draw() const {
        return m_ignore_next_viewport_draw;
    }

    // True only while inside the UEVR-forced, manually re-invoked second FViewport::Draw call
    // used by Synchronized Sequential mode to draw the second eye within the same engine tick.
    // FViewport::Draw is also where the engine polls/pumps input devices, so this second forced
    // call causes a single physical button press to be seen as two separate input frames by the
    // engine's own input processing (e.g. UMG/Slate gamepad navigation), unlike Native Stereo
    // which only calls Draw once per tick. Consumers (e.g. XInputHook) can check this to avoid
    // re-dispatching the same input state as if it were a new frame during this forced redraw.
    bool is_in_synced_forced_viewport_draw() const {
        return m_in_synced_forced_viewport_draw;
    }

    auto& get_last_pre_rotation() {
        return m_last_pre_rotation;
    }

    auto& get_last_pre_rotation_double() {
        return m_last_pre_rotation_double;
    }

    // Do not call these directly
    static void setup_viewpoint(ISceneViewExtension* extension, void* player_controller, void* view_info);
    static void localplayer_setup_viewpoint(void* localplayer, void* view_info, void* pass);
    static void setup_view_family(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family);
    static void begin_render_viewfamily_real(void* render_module, sdk::FCanvas* canvas, sdk::FSceneViewFamily* view_family);
    static void begin_render_viewfamily(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family);
    static void pre_render_viewfamily_renderthread(ISceneViewExtension* extension, sdk::FRHICommandListBase* cmd_list, sdk::FSceneViewFamily& view_family);

private:
    bool hook();
    bool standard_fake_stereo_hook(uintptr_t vtable);
    bool nonstandard_create_stereo_device_hook();
    bool nonstandard_create_stereo_device_hook_4_27();
    bool nonstandard_create_stereo_device_hook_4_22();
    bool nonstandard_create_stereo_device_hook_4_18();
    
    bool hook_game_viewport_client();
    bool setup_view_extensions();

    static std::optional<uintptr_t> locate_fake_stereo_rendering_constructor();
    static std::optional<uintptr_t> locate_fake_stereo_rendering_vtable();
    static std::optional<uintptr_t> locate_active_stereo_rendering_device();
    static inline uintptr_t s_stereo_rendering_device_offset{0}; // GEngine

    std::optional<uint32_t> get_stereo_view_offset_index(uintptr_t vtable);

    bool patch_vtable_checks();
    bool attempt_runtime_inject_stereo();
    void post_init_properties(uintptr_t localplayer);

    // Hooks
    // UGameEngine
    static void* engine_tick_hook(sdk::UGameEngine* engine, float delta, bool idle);

    // FSceneView
    static sdk::FSceneView* sceneview_constructor(sdk::FSceneView* sceneview, sdk::FSceneViewInitOptions* init_options, void* a3, void* a4);
    
    // IStereoRendering
    static bool is_stereo_enabled(FFakeStereoRendering* stereo);
    static void adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h);
    static void calculate_stereo_view_offset(FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation,
        const float world_to_meters, Vector3f* view_location);
    static Matrix4x4f* calculate_stereo_projection_matrix(FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index);
    static void render_texture_render_thread(FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list,
        FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size);
    static void init_canvas(FFakeStereoRendering* stereo, sdk::FSceneView* view, UCanvas* canvas);
    static uint32_t get_desired_number_of_views_hook(FFakeStereoRendering* stereo, bool is_stereo_enabled);
    static EStereoscopicPass get_view_pass_for_index_hook(FFakeStereoRendering* stereo, bool stereo_requested, int32_t view_index);

    static IStereoRenderTargetManager* get_render_target_manager_hook(FFakeStereoRendering* stereo);
    static IStereoLayers* get_stereo_layers_hook(FFakeStereoRendering* stereo);

    // LocalPlayer
    static void post_calculate_stereo_projection_matrix(safetyhook::Context& ctx);
    static void pre_get_projection_data(safetyhook::Context& ctx);

    // Slate
    static void* slate_draw_window_render_thread(void* renderer, void* command_list, void* viewport_info, 
                                                 void* elements, void* params, void* unk1, void* unk2);

    // FViewport
    static void* viewport_destructor_hook(void* viewport, void* a2, void* a3, void* a4);
    static void viewport_draw_hook(void* viewport, bool should_present);
    static FRHITexture2D** viewport_get_render_target_texture_hook(sdk::FViewport* viewport);

    // UGameViewportClient
    static void game_viewport_client_draw_hook(sdk::UGameViewportClient*, sdk::FViewport*, sdk::FCanvas*, void*);

    // FSceneViewport
    static void update_viewport_rhi_hook(void* viewport, size_t destroyed, size_t new_size_x, size_t new_size_y, size_t new_window_mode, size_t preferred_pixel_format);

    std::unique_ptr<ThreadWorker<FRHICommandListImmediate*>> m_slate_thread_worker{std::make_unique<ThreadWorker<FRHICommandListImmediate*>>()};

    struct {
        std::recursive_mutex mtx{};
        safetyhook::InlineHook constructor_hook{};
        std::unordered_set<sdk::FSceneViewStateInterface*> known_scene_states;
        bool inside_post_init_properties{false};

        uint32_t last_frame_count{};
        uint32_t last_index{};

        // For keeping track of what the states were before our modifications.
        std::unordered_map<sdk::FSceneViewStateInterface*, sdk::FSceneViewInitOptionsUE4> view_init_options_ue4{};
        std::unordered_map<sdk::FSceneViewStateInterface*, sdk::FSceneViewInitOptionsUE5> view_init_options_ue5{};
        std::unordered_set<uintptr_t> seen_retaddrs{};
    } m_sceneview_data;

    safetyhook::InlineHook m_localplayer_get_viewpoint_hook{};
    safetyhook::InlineHook m_tick_hook{};
    safetyhook::InlineHook m_adjust_view_rect_hook{};
    safetyhook::InlineHook m_calculate_stereo_view_offset_hook_inline{};
    std::unique_ptr<PointerHook> m_calculate_stereo_view_offset_hook_ptr{}; // some games have a short jmp which isnt supported by safetyhook right now so we use pointerhook
    safetyhook::InlineHook m_calculate_stereo_projection_matrix_hook{};
    safetyhook::InlineHook m_render_texture_render_thread_hook{};
    safetyhook::InlineHook m_slate_thread_hook{};
    safetyhook::InlineHook m_gameviewportclient_draw_hook{};
    safetyhook::InlineHook m_viewport_draw_hook{}; // for AFR
    safetyhook::InlineHook m_render_module_begin_render_viewfamily_hook{};

    // both of these are used to figure out where the localplayer is, they aren't actively
    // used for anything else, the second one is an alternative hook if the first one
    // deems fruitless.
    safetyhook::MidHook m_calculate_stereo_projection_matrix_post_hook{};
    safetyhook::MidHook m_get_projection_data_pre_hook{};

    std::unique_ptr<PointerHook> m_is_stereo_enabled_hook{};
    std::unique_ptr<PointerHook> m_get_render_target_manager_hook{};
    std::unique_ptr<PointerHook> m_get_stereo_layers_hook{};
    std::unique_ptr<PointerHook> m_init_canvas_hook{};
    std::unique_ptr<PointerHook> m_get_desired_number_of_views_hook{};
    std::unique_ptr<PointerHook> m_get_view_pass_for_index_hook{};
    std::unique_ptr<PointerHook> m_update_viewport_rhi_hook{};
    std::unique_ptr<PointerHook> m_viewport_get_render_target_texture_hook{};
    std::unique_ptr<PointerHook> m_viewport_destructor_hook{};

    std::unique_ptr<IXRTrackingSystemHook> m_tracking_system_hook{};

    struct {
        std::unordered_set<uintptr_t> seen_retaddrs{};
        std::unordered_set<uintptr_t> call_original_retaddrs{};
        std::unordered_set<uintptr_t> redirected_retaddrs{};
        std::recursive_mutex retaddr_mutex{};
        bool has_view_family_tex{false};
        int32_t selected_retaddr{0};
    } m_viewport_rt_hook_data{};

    VRRenderTargetManager m_rtm{};
    VRRenderTargetManager_418 m_rtm_418{};
    VRRenderTargetManager_Special m_rtm_special{};

    Rotator<float> m_last_afr_rotation{};
    Rotator<double> m_last_afr_rotation_double{};

    // Monotonically increasing "epoch" for the current view-target/scene-capture texture identity.
    // Bumped every time set_should_recreate_textures(true) is called (i.e. whenever a reallocation
    // is requested). Logged alongside texture pointers at reallocation, scene-capture bind/retention,
    // eye composite copy/blit, and OpenXR end_frame submission so a captured log can reveal whether a
    // frame ever mixes content from two different generations (a race producing single-image ghosting/
    // double-image artifacts independent of stereo/eye desync).
    uint64_t m_view_target_generation{0};

    // DIAG: epoch (steady_clock time_point) when the scene capture RT most recently began
    // continuously reporting null while the compositor is retaining a stale last-known-good
    // texture. Epoch value (default-constructed) means "not currently stalled". See
    // report_scene_capture_stall_frame()/get_scene_capture_stall_duration_ms()/is_scene_capture_stalled().
    std::chrono::steady_clock::time_point m_scene_capture_stall_start{};

    // DIAG/TRIGGER: last time LGUI's render-thread draw hook (lgui_slot24_hook) actually fired, and
    // whether it has ever fired at all yet. See report_lgui_draw_heartbeat()/get_lgui_draw_silence_duration_ms().
    std::chrono::steady_clock::time_point m_lgui_last_draw_time{};
    bool m_lgui_ever_drawn{false};

    // DIAG/TRIGGER: last time the NSF sync-pose code (calculate_stereo_view_offset) observed a
    // meaningful rotation/position divergence between Pass1 (left) and Pass2 (right) within the SAME
    // engine frame - i.e. the live animated camera actually moved between the two eyes' render calls.
    // This is a direct measurement of "camera motion vectors changing due to animation/VFX", which is
    // exactly the condition the user identified as the real trigger for the disorienting right-eye lag
    // (as opposed to bCinematicMode or UI-blank, which don't correlate). See
    // report_pose_divergence_event()/get_ms_since_last_pose_divergence().
    std::chrono::steady_clock::time_point m_last_pose_divergence_time{};
    bool m_had_pose_divergence{false};

    // NSF (non-AFR) equivalent of the AFR rotation-cache above:
    // location for the current frame so Pass2 (right eye) can be forced to reuse it, eliminating a
    // momentary eye desync when the live animated camera pose changes between the two eyes' render
    // calls within the same frame (camera-transition animations). Gated behind
    // is_native_stereo_fix_sync_pose_enabled(); see calculate_stereo_view_offset().
    Rotator<float> m_nsf_sync_pose_rotation{};
    Rotator<double> m_nsf_sync_pose_rotation_double{};
    Vector3f m_nsf_sync_pose_location{};
    Vector3d m_nsf_sync_pose_location_double{};
    uint32_t m_nsf_sync_pose_frame_count{0};
    bool m_nsf_sync_pose_have_left{false};

    // HARD-CUT detection state for the NSF sync-pose blend (see calculate_stereo_view_offset). Plain
    // magnitude thresholds alone (mirroring the Lua cutscene-actor reset script's >10deg/>30unit test)
    // turned out to be too sensitive here because, unlike the Lua script (which only ever evaluates
    // that test while the view target is an actual CineCameraActor, i.e. never during normal player-
    // controlled input), our C++ check runs every frame regardless of what is driving the camera - so
    // a normal fast thumbstick turn can rack up >10 degrees of Pass1/Pass2 divergence in a single frame
    // and would get misclassified as a hard cut. To compensate we track a smoothed rolling baseline
    // (EMA) of recent per-frame deltas and only call it a hard cut if the current delta is both an
    // abrupt SPIKE relative to that recent baseline (a real discontinuity) AND above a raised absolute
    // floor (a safety net for cases where the whole preceding window was already elevated, e.g. very
    // fast continuous spinning).
    float m_nsf_pose_delta_rot_ema{0.0f};
    float m_nsf_pose_delta_pos_ema{0.0f};
    uint32_t m_nsf_pose_delta_sample_count{0};

    // Returns true if the given per-frame rotation/position delta should be treated as a hard camera
    // cut (full instant snap) rather than continuous motion (blended). Updates the rolling EMA
    // baseline as a side effect, so this must be called at most once per frame from the NSF sync-pose
    // branch.
    bool is_nsf_sync_pose_hard_cut(float rot_delta, float pos_delta) {
        constexpr float kEmaAlpha = 0.1f;
        constexpr float kSpikeMultiplier = 6.0f;
        constexpr float kMinBaselineRot = 2.0f;   // degrees, avoids div-by-near-zero baseline spikes
        constexpr float kMinBaselinePos = 5.0f;   // units
        constexpr float kHardCutRotFloor = 25.0f; // degrees - raised safety-net floor
        constexpr float kHardCutPosFloor = 80.0f; // units - raised safety-net floor
        constexpr uint32_t kMinSamplesForSpike = 10;

        const auto rot_baseline = (std::max)(m_nsf_pose_delta_rot_ema, kMinBaselineRot);
        const auto pos_baseline = (std::max)(m_nsf_pose_delta_pos_ema, kMinBaselinePos);

        const bool has_enough_samples = m_nsf_pose_delta_sample_count >= kMinSamplesForSpike;
        const bool is_spike = has_enough_samples &&
            (rot_delta > rot_baseline * kSpikeMultiplier || pos_delta > pos_baseline * kSpikeMultiplier);
        const bool is_over_floor = rot_delta > kHardCutRotFloor || pos_delta > kHardCutPosFloor;

        // Update the rolling baseline AFTER evaluating this frame so the spike itself doesn't get
        // absorbed into the baseline before we've judged it.
        m_nsf_pose_delta_rot_ema = (m_nsf_pose_delta_sample_count == 0)
            ? rot_delta
            : std::lerp(m_nsf_pose_delta_rot_ema, rot_delta, kEmaAlpha);
        m_nsf_pose_delta_pos_ema = (m_nsf_pose_delta_sample_count == 0)
            ? pos_delta
            : std::lerp(m_nsf_pose_delta_pos_ema, pos_delta, kEmaAlpha);
        ++m_nsf_pose_delta_sample_count;

        return is_spike && is_over_floor;
    }

    // DIAG: gradual hard-cut convergence state. Instead of instantly snapping Pass2's rotation/
    // position to Pass1's cached pose the moment a hard cut is detected (blend_alpha=1.0 applied on a
    // single frame), this ramps an effective blend_alpha from 0 up to 1 linearly over
    // duration_ms once a hard cut fires, so the two eyes ease back into agreement over roughly a
    // second instead of one eye teleporting into place. Purely diagnostic/toggle-gated - see
    // is_diag_gradual_hard_cut_convergence_enabled() in VR.hpp.
    std::chrono::steady_clock::time_point m_nsf_hard_cut_start_time{};
    bool m_nsf_hard_cut_active{false};

    // Must be called at most once per Pass2 (true_index==1) call per frame, mirroring
    // is_nsf_sync_pose_hard_cut() above. trigger_now should be (is_hard_cut || force_full) for the
    // current frame. Returns the ramped alpha to use in place of the instant 1.0 snap; returns 0.0f
    // once no hard cut is active/being converged (caller should fall back to the normal continuous
    // blend_alpha in that case).
    float get_nsf_gradual_convergence_alpha(bool trigger_now, float duration_ms) {
        const auto now = std::chrono::steady_clock::now();

        if (trigger_now) {
            m_nsf_hard_cut_start_time = now;
            m_nsf_hard_cut_active = true;
        }

        if (!m_nsf_hard_cut_active) {
            return 0.0f;
        }

        const auto elapsed_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(now - m_nsf_hard_cut_start_time).count();

        if (duration_ms <= 0.0f || elapsed_ms >= duration_ms) {
            m_nsf_hard_cut_active = false;
            return 1.0f;
        }

        return elapsed_ms / duration_ms;
    }

    Rotator<float> m_last_pre_rotation{};
    Rotator<double> m_last_pre_rotation_double{};

    std::atomic<Rotator<float>> m_last_rotation{};
    std::atomic<Rotator<double>> m_last_rotation_double{};

    std::vector<uintptr_t> m_projection_matrix_stack{};
    bool m_hooked_alternative_localplayer_scan{false};

    bool m_hooked{false};
    bool m_tried_hooking{false};
    bool m_finished_hooking{false};
    bool m_hooked_game_engine_tick{false};
    bool m_hooked_slate_thread{false};
    bool m_attempted_hook_game_engine_tick{false};
    bool m_attempted_hook_slate_thread{false};
    bool m_attempted_hook_slate_thread_alternate{false};
    bool m_attempted_hook_update_viewport_rhi{false};
    bool m_attempted_hook_fsceneview_constructor{false};
    bool m_uses_old_rendertarget_manager{false};
    bool m_rendertarget_manager_embedded_in_stereo_device{false}; // 4.17 and below...?
    bool m_special_detected{false};
    bool m_special_detected_4_18{false};
    bool m_special_detected_4_22{false};
    bool m_special_detected_4_27{false};
    bool m_manually_constructed{false};
    bool m_pixel_format_cvar_found{false};
    UIDrawExtent m_ui_draw_extent{};
    UIDrawExtent m_game_viewport_size{};
    bool m_injected_stereo_at_runtime{false};
    bool m_has_double_precision{false}; // for the projection matrix... AND the view offset... IS UE5 DOING THIS NOW???
    bool m_fixed_localplayer_view_count{false};
    bool m_wants_texture_recreation{false};
    bool m_has_view_extension_hook{false};
    bool m_has_game_viewport_client_draw_hook{false};
    bool m_skip_next_adjust_view_rect{true};
    bool m_inside_slate_draw_window{false};
    int32_t m_skip_next_adjust_view_rect_count{1};
    uint32_t m_slate_draw_window_thread_id{0};

    // Last-known x-offset assigned by AdjustViewRect to each eye. See get_last_left_eye_x_offset().
    std::atomic<uint32_t> m_last_left_eye_x_offset{0};
    std::atomic<uint32_t> m_last_right_eye_x_offset{0};
    std::atomic<bool> m_has_seen_eye_x_offsets{false};

    // Monotonically-increasing counter bumped every time AdjustViewRect records a new offset for
    // the given eye (see adjust_view_rect()). The compositor (D3D12Component::composite_afr_eye)
    // uses this to detect when the engine's own eye-index state machine has skewed toward one eye
    // for several consecutive frames (observed in the wild: the engine's true_index resolving to
    // "right" far more often than "left" in some AFR games), so it can avoid re-copying a stale/
    // never-updated half of the backbuffer over a good previous frame.
    std::atomic<uint64_t> m_left_eye_x_offset_update_count{0};
    std::atomic<uint64_t> m_right_eye_x_offset_update_count{0};

    // Synchronized AFR
    float m_ignored_engine_delta{0.0f};
    bool m_in_engine_tick{false};
    bool m_in_viewport_client_draw{false};
    bool m_was_in_viewport_client_draw{false}; // for IsStereoEnabled
    bool m_in_synced_forced_viewport_draw{false};
    bool m_ignore_next_viewport_draw{false};
    bool m_ignore_next_engine_tick{false};
    void* m_last_destroyed_viewport{nullptr}; // used to check if the viewport is destroyed when we call FViewport::Draw again
    void** m_last_viewport_vtable{nullptr};


    bool m_analyzing_view_extensions{false};
    bool m_has_view_extensions_installed{false};

    std::chrono::time_point<std::chrono::high_resolution_clock> m_analyze_view_extensions_start_time{};

    /*FFakeStereoRendering m_stereo_recreation {
        90.0f, 
        (int32_t)1920, 
        (int32_t)1080, 
        (int32_t)2
    };*/

    struct FallbackDevice {
        void* vtable;
        char padding[0x20]{};
    } m_fallback_device;
    std::vector<void*> m_fallback_vtable{};

    // Seems to be the case in <= 4.17
    struct EmbeddedRenderTargetManagerInfo {
        std::unique_ptr<PointerHook> should_use_separate_render_target_hook{};
        std::unique_ptr<PointerHook> calculate_render_target_size_hook{};
        std::unique_ptr<PointerHook> allocate_render_target_texture_hook{};
        std::unique_ptr<PointerHook> need_reallocate_viewport_render_target_hook{};
        std::chrono::steady_clock::time_point last_time_needed_hmd_reallocate{};
        bool should_use_separate_rt_called{true};
        bool need_reallocate_viewport_render_target_called{true};
    } m_embedded_rtm;

    const ModToggle::Ptr m_recreate_textures_on_reset{ ModToggle::create("VR_RecreateTexturesOnReset", true) };
    const ModInt32::Ptr m_frame_delay_compensation{ ModInt32::create("VR_FrameDelayCompensation", 0) };
    const ModToggle::Ptr m_asynchronous_scan{ ModToggle::create("VR_AsynchronousScan", true) };
    // Off by default because it can cause issues with some games
    const ModToggle::Ptr m_use_fmalloc_scene_view_extensions{ ModToggle::create("VR_UseFMallocSceneViewExtensions", false) };

    void setup_options() {
        m_options = {
            *m_recreate_textures_on_reset,
            *m_frame_delay_compensation,
            *m_asynchronous_scan,
            *m_use_fmalloc_scene_view_extensions
        };
    }

    friend class IXRTrackingSystemHook;
};