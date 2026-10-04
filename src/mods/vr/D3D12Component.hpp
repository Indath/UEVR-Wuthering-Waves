#pragma once

#include <array>
#include <chrono>
#include <span>
#include <vector>

#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <wrl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <../../directxtk12-src/Inc/GraphicsMemory.h>
#include <../../directxtk12-src/Inc/SpriteBatch.h>
#include <../../directxtk12-src/Inc/DescriptorHeap.h>

#include "d3d12/CommandContext.hpp"
#include "d3d12/TextureContext.hpp"

class VR;

namespace vrmod {
class D3D12Component {
public:
    D3D12Component() 
        : m_openvr{this}
        , m_openxr{this}
    {

    }

    vr::EVRCompositorError on_frame(VR* vr);
    void on_post_present(VR* vr);
    void on_reset(VR* vr);

    void force_reset() { m_force_reset = true; }

    const auto& get_backbuffer_size() const { return m_backbuffer_size; }

    auto is_initialized() const { return m_openvr.left_eye_tex[0].texture != nullptr; }

    auto& openxr() { return m_openxr; }
    auto& get_openvr_ui_tex() { return m_openvr.ui_tex; }

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    // Exposed so callers tearing down a scene-capture actor/resource (e.g.
    // VRRenderTargetManager_Base::destroy_scene_capture) can flush outstanding GPU work against it
    // first, avoiding a release-while-in-flight race with the game's own D3D12 command lists.
    void wait_for_scene_capture_copies();

    // A resize/reallocation (e.g. NSF turning off at a menu, which changes the view-target size)
    // tears down and recreates our own D3D12 textures and OpenXR swapchains. wait_for_all_copies()/
    // wait_for_scene_capture_copies() only fence UEVR's own D3D12 copy queue - they have no
    // visibility into the game engine's own RHIThread, which pipelines several frames ahead of the
    // game thread in Unreal. If RHIThread still has commands recorded against a resource we just
    // released in place, it can later issue a ResourceBarrier/etc. against freed memory, causing an
    // access violation deep in the driver/D3D12Core (observed: ResourceBarrier reading offset 0x88).
    // Since there is no public hook to fence the engine's own queue, defer actual release of these
    // resources instead, giving any in-flight RHIThread work recorded against them time to retire
    // naturally before we drop the last reference.
    //
    // NOTE: A fixed small frame-count delay (e.g. "wait 5 presents") is NOT sufficient. During
    // menu/level-load transitions RHIThread/the game thread can stall for multiple seconds (observed
    // present stalls of 5+ seconds in crash logs) while our own present-driven drain() keeps ticking
    // the ring forward, which can free a resource well before the engine's backlogged commands against
    // it actually execute. Retention is therefore time-based (wall clock), not frame-count-based.
    struct DeferredResourceReleaser {
        // Conservative minimum retention: long enough to cover multi-second RHIThread/game-thread
        // stalls observed during level/menu transitions, not just a handful of normal frames.
        static constexpr std::chrono::milliseconds min_retention{3000};

        struct Entry {
            ComPtr<ID3D12Resource> resource{};
            std::chrono::steady_clock::time_point queued_at{};
        };

        std::vector<Entry> entries{};
        std::mutex mtx{};

        // Queue a resource for release no sooner than min_retention from now.
        void defer(ComPtr<ID3D12Resource> resource) {
            if (resource == nullptr) {
                return;
            }

            std::scoped_lock _{this->mtx};
            this->entries.push_back(Entry{std::move(resource), std::chrono::steady_clock::now()});
        }

        // Call once per present/frame. Releases only entries that have aged past min_retention.
        void drain() {
            std::scoped_lock _{this->mtx};

            const auto now = std::chrono::steady_clock::now();

            std::erase_if(this->entries, [&](const Entry& entry) {
                return (now - entry.queued_at) >= min_retention;
            });
        }
    };

    DeferredResourceReleaser m_deferred_resource_releaser{};

private:
    bool setup();
    std::unique_ptr<DirectX::DX12::SpriteBatch> setup_sprite_batch_pso(
        DXGI_FORMAT output_format, 
        std::span<const uint8_t> vs = {}, std::span<const uint8_t> ps = {},
        std::optional<DirectX::SpriteBatchPipelineStateDescription> pd = std::nullopt
    );

    void draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame);
    void clear_backbuffer();

    ComPtr<ID3D12Resource> m_prev_backbuffer{};
    std::array<d3d12::CommandContext, 3> m_generic_commands{};

    d3d12::TextureContext m_backbuffer_copy{};

    // RTV-only wrapper around the OpenXR double-wide swapchain render target, used so the
    // native-stereo-fix right eye (whose resolution can differ from the destination eye region)
    // can be scaled into place via a shader blit instead of a raw CopyTextureRegion.
    d3d12::TextureContext m_stereo_dst_tex{};

    d3d12::TextureContext m_game_ui_tex{};
    d3d12::TextureContext m_game_tex{};
    d3d12::TextureContext m_scene_capture_tex{};

    // Tracks how many consecutive frames (at the same view-target generation) the scene capture
    // render target has been observed as null while m_scene_capture_tex was still bound, so we can
    // cap how long we keep compositing against the stale texture before forcing a reset. See the
    // usage site in draw_scene_capture_overlay (D3D12Component.cpp) for the full rationale.
    uint64_t m_scene_capture_stale_generation{};
    uint32_t m_scene_capture_consecutive_null_frames{};

    std::array<d3d12::CommandContext, 3> m_game_tex_commands{};
    std::array<d3d12::TextureContext, 2> m_2d_screen_tex{};
    std::vector<std::unique_ptr<d3d12::TextureContext>> m_backbuffer_textures{};

    std::unique_ptr<DirectX::DX12::GraphicsMemory> m_graphics_memory{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_backbuffer_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_game_batch{};
    std::unique_ptr<DirectX::DX12::SpriteBatch> m_ui_batch_alpha_invert{};

    ID3D12Resource* m_last_checked_native{nullptr};

    // Mimicking what OpenXR does.
    struct OpenVR {
        OpenVR(D3D12Component* p) : parent{p} {}
        
        d3d12::TextureContext& get_left() {
            auto& ctx = this->left_eye_tex[this->texture_counter % left_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& get_right() {
            auto& ctx = this->right_eye_tex[this->texture_counter % right_eye_tex.size()];

            return ctx;
        }

        d3d12::TextureContext& acquire_left() {
            auto& ctx = get_left();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        d3d12::TextureContext& acquire_right() {
            auto& ctx = get_right();
            ctx.commands.wait(INFINITE);

            return ctx;
        }

        void copy_left(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_left();
            //ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Copy the left half of the backbuffer to the left eye texture.
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.right = parent->m_backbuffer_size[0] / 2;
            src_box.bottom = parent->m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        void copy_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_right();
            //ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Copy the right half of the backbuffer to the right eye texture.
            D3D12_BOX src_box{};
            src_box.left = parent->m_backbuffer_size[0] / 2;
            src_box.top = 0;
            src_box.right = parent->m_backbuffer_size[0];
            src_box.bottom = parent->m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }
        
        // For AFR
        void copy_left_to_right(ID3D12Resource* src, D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT) {
            auto& ctx = this->acquire_right();
            //ctx.commands.copy(src, ctx.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Copy the right half of the backbuffer to the right eye texture.
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.right = parent->m_backbuffer_size[0] / 2;
            src_box.bottom = parent->m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;
            ctx.commands.copy_region(src, ctx.texture.Get(), &src_box, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ctx.commands.execute();
        }

        std::array<d3d12::TextureContext, 3> left_eye_tex{};
        std::array<d3d12::TextureContext, 3> right_eye_tex{};
        d3d12::TextureContext ui_tex{};
        uint32_t texture_counter{0};
        D3D12Component* parent{};

        friend class D3D12Component;
    } m_openvr;

    struct OpenXR {
        OpenXR(D3D12Component* p) : parent{p} {}

        void initialize(XrSessionCreateInfo& session_info);
        std::optional<std::string> create_swapchains();
        void destroy_swapchains();
        void copy(uint32_t swapchain_idx, ID3D12Resource* src,
            std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands = std::nullopt,
            std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands = std::nullopt,
            D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT, D3D12_BOX* src_box = nullptr);

        void copy(uint32_t swapchain_idx, ID3D12Resource* src,
            D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT, D3D12_BOX* src_box = nullptr)
        {
            this->copy(swapchain_idx, src, std::nullopt, std::nullopt, src_state, src_box);
        }
        void wait_for_all_copies() {
            std::scoped_lock _{this->mtx};

            for (auto& it : this->contexts) {
                for (auto& texture_ctx : it.second.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }
            }
        }

        bool ever_acquired(uint32_t swapchain_idx) {
            std::scoped_lock _{this->mtx};

            auto it = this->contexts.find(swapchain_idx);
            if (it == this->contexts.end()) {
                return false;
            }

            return it->second.ever_acquired;
        }

        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

        struct SwapchainContext {
            std::vector<XrSwapchainImageD3D12KHR> textures{};
            std::vector<std::unique_ptr<d3d12::TextureContext>> texture_contexts{};
            uint32_t num_textures_acquired{0};
            uint32_t last_acquired_texture{0};
            bool ever_acquired{false};
        };

        std::unordered_map<uint32_t, SwapchainContext> contexts{};
        std::recursive_mutex mtx{};
        std::array<uint32_t, 2> last_resolution{};
        bool made_depth_with_null_defaults{false};

        D3D12Component* parent{};

        friend class D3D12Component;
    } m_openxr;

    uint32_t m_backbuffer_size[2]{};

    uint32_t m_last_rendered_frame{0};
    bool m_force_reset{true};
    bool m_last_afr_state{false};
    bool m_submitted_left_eye{false};
};
} // namespace vrmod