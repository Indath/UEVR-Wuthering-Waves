#include <d3dcompiler.h>

#include <openvr.h>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>

#include "Framework.hpp"
#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"

#include "d3d12/DirectXTK.hpp"

#include "D3D12Component.hpp"
#include <uevr/API.hpp>

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

namespace {
// DIAG: synchronous GPU->CPU pixel readback used to definitively determine whether a given
// D3D12 texture actually contains non-black pixel data at the moment it's sampled. This is
// intentionally heavyweight (stalls the GPU via a fence wait) so it must only be invoked from
// throttled call sites. It is used to bisect the SS black-screen path: if the *source* backbuffer
// sampled here is non-black but the *destination* OpenXR swapchain texture sampled right after
// the copy is black, the bug is in our copy/compositor path. If the source itself is already
// black, the bug is upstream in UE's own scene rendering (before it ever reaches our hook).
struct DiagPixelSample {
    bool succeeded{false};
    double avg_luminance{0.0};
    uint32_t corner_pixel{0}; // top-left pixel, packed as read (assumed BGRA8-ish layout)
    uint32_t center_pixel{0};
    uint32_t sampled_pixels{0};
    uint32_t nonzero_pixels{0};
};

// DIAG: sample_x_offset/sample_y_offset let callers pick WHERE in the texture the 32x32 sample
// window is taken from, instead of always the top-left corner. This matters because a fixed
// top-left-corner sample of the full double-wide backbuffer only ever lands inside the left eye's
// half - it can never prove anything about the right eye's content, and if the sampled corner
// happens to be a black letterbox/border pixel even within the left eye's own content, it would
// misleadingly look like "the whole eye is black" when only that specific corner is. Sampling from
// the center of each eye's actual region gives a much stronger, less ambiguous signal.
static DiagPixelSample diag_sample_texture(ID3D12Device* device, ID3D12CommandQueue* command_queue, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, uint32_t sample_x_offset = 0, uint32_t sample_y_offset = 0) {
    DiagPixelSample result{};

    if (device == nullptr || command_queue == nullptr || src == nullptr) {
        return result;
    }

    const auto desc = src->GetDesc();

    if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN) {
        return result;
    }

    // Only sample a small region so this stays cheap even though it's synchronous.
    constexpr uint32_t sample_dim = 32;
    sample_x_offset = (std::min)(sample_x_offset, desc.Width > sample_dim ? (uint32_t)desc.Width - sample_dim : 0);
    sample_y_offset = (std::min)(sample_y_offset, desc.Height > sample_dim ? (uint32_t)desc.Height - sample_dim : 0);
    const uint32_t sample_w = (std::min)((uint32_t)desc.Width, sample_dim);
    const uint32_t sample_h = (std::min)((uint32_t)desc.Height, sample_dim);

    D3D12_RESOURCE_DESC staging_desc{};
    staging_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    staging_desc.Width = sample_w;
    staging_desc.Height = sample_h;
    staging_desc.DepthOrArraySize = 1;
    staging_desc.MipLevels = 1;
    staging_desc.Format = desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    staging_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows{};
    UINT64 row_size{};
    UINT64 total_bytes{};
    device->GetCopyableFootprints(&staging_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);

    if (total_bytes == 0) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer{};

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buffer)))) {
        spdlog::error("[DIAG] Pixel sample: failed to create readback buffer.");
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_allocator{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence{};

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&cmd_list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        spdlog::error("[DIAG] Pixel sample: failed to create throwaway command objects.");
        return result;
    }

    D3D12_RESOURCE_BARRIER src_barrier{};
    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    const bool needs_transition = src_state != D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (needs_transition) {
        cmd_list->ResourceBarrier(1, &src_barrier);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback_buffer.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_BOX src_box{};
    src_box.left = sample_x_offset;
    src_box.top = sample_y_offset;
    src_box.front = 0;
    src_box.right = sample_x_offset + sample_w;
    src_box.bottom = sample_y_offset + sample_h;
    src_box.back = 1;

    cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

    if (needs_transition) {
        src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        src_barrier.Transition.StateAfter = src_state;
        cmd_list->ResourceBarrier(1, &src_barrier);
    }

    if (FAILED(cmd_list->Close())) {
        spdlog::error("[DIAG] Pixel sample: failed to close command list.");
        return result;
    }

    ID3D12CommandList* const cmd_lists[] = {cmd_list.Get()};
    command_queue->ExecuteCommandLists(1, cmd_lists);

    HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (fence_event == nullptr) {
        spdlog::error("[DIAG] Pixel sample: failed to create fence event.");
        return result;
    }

    command_queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    const auto wait_result = WaitForSingleObject(fence_event, 2000);
    CloseHandle(fence_event);

    if (wait_result != WAIT_OBJECT_0) {
        spdlog::error("[DIAG] Pixel sample: timed out waiting for GPU readback fence.");
        return result;
    }

    void* mapped{nullptr};
    D3D12_RANGE read_range{0, (SIZE_T)total_bytes};

    if (FAILED(readback_buffer->Map(0, &read_range, &mapped))) {
        spdlog::error("[DIAG] Pixel sample: failed to map readback buffer.");
        return result;
    }

    const auto* bytes = (const uint8_t*)mapped;
    // Assume 4 bytes-per-pixel formats (true for all backbuffer/swapchain formats used here, e.g. BGRA8/RGBA8).
    const uint32_t bytes_per_pixel = 4;
    uint64_t luminance_sum = 0;
    uint32_t sampled = 0;
    uint32_t nonzero = 0;

    for (uint32_t y = 0; y < sample_h; ++y) {
        const auto* row = bytes + (size_t)footprint.Footprint.RowPitch * y;

        for (uint32_t x = 0; x < sample_w; ++x) {
            const auto* pixel = row + (size_t)x * bytes_per_pixel;
            const uint32_t b = pixel[0];
            const uint32_t g = pixel[1];
            const uint32_t r = pixel[2];

            luminance_sum += (r + g + b);
            ++sampled;

            if (r != 0 || g != 0 || b != 0) {
                ++nonzero;
            }
        }
    }

    if (sample_w > 0 && sample_h > 0) {
        const auto* corner_row = bytes;
        result.corner_pixel = *(const uint32_t*)corner_row;

        const auto center_x = sample_w / 2;
        const auto center_y = sample_h / 2;
        const auto* center_row = bytes + (size_t)footprint.Footprint.RowPitch * center_y;
        result.center_pixel = *(const uint32_t*)(center_row + (size_t)center_x * bytes_per_pixel);
    }

    readback_buffer->Unmap(0, nullptr);

    result.succeeded = true;
    result.avg_luminance = sampled > 0 ? (double)luminance_sum / (sampled * 3.0) : 0.0;
    result.sampled_pixels = sampled;
    result.nonzero_pixels = nonzero;

    return result;
}

// DIAG: full readback of a texture to find the bounding box of every pixel that has any color or alpha.
// Used on the LGUI ui_target to measure exactly which sub-rectangle the redirected UI pass draws into,
// instead of inferring it from view rects. Synchronous and heavy: callers must throttle it.
struct DiagContentBounds {
    bool succeeded{false};
    uint32_t tex_w{0}, tex_h{0};
    int32_t min_x{-1}, min_y{-1}, max_x{-1}, max_y{-1};
    uint32_t nonzero_pixels{0};
};

static DiagContentBounds diag_content_bounds(ID3D12Device* device, ID3D12CommandQueue* command_queue, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state) {
    DiagContentBounds result{};

    if (device == nullptr || command_queue == nullptr || src == nullptr) {
        return result;
    }

    const auto desc = src->GetDesc();
    if (desc.Width == 0 || desc.Height == 0 || desc.Format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return result;
    }

    result.tex_w = (uint32_t)desc.Width;
    result.tex_h = desc.Height;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows{};
    UINT64 row_size{};
    UINT64 total_bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);

    if (total_bytes == 0) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer{};
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buffer)))) {
        spdlog::error("[DIAG] Content bounds: failed to create readback buffer.");
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_allocator{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_list{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence{};

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&cmd_list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        spdlog::error("[DIAG] Content bounds: failed to create command objects.");
        return result;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = src;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = src_state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    const bool needs_transition = src_state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (needs_transition) {
        cmd_list->ResourceBarrier(1, &barrier);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = readback_buffer.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    if (needs_transition) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = src_state;
        cmd_list->ResourceBarrier(1, &barrier);
    }

    if (FAILED(cmd_list->Close())) {
        return result;
    }

    ID3D12CommandList* const cmd_lists[] = {cmd_list.Get()};
    command_queue->ExecuteCommandLists(1, cmd_lists);

    HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr) {
        return result;
    }

    command_queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    const auto wait_result = WaitForSingleObject(fence_event, 2000);
    CloseHandle(fence_event);

    if (wait_result != WAIT_OBJECT_0) {
        spdlog::error("[DIAG] Content bounds: timed out waiting for GPU readback fence.");
        return result;
    }

    void* mapped{nullptr};
    D3D12_RANGE read_range{0, (SIZE_T)total_bytes};
    if (FAILED(readback_buffer->Map(0, &read_range, &mapped))) {
        return result;
    }

    const auto* bytes = (const uint8_t*)mapped;
    constexpr uint32_t stride = 4; // sample every 4th pixel in both axes; bounds are accurate to +-4px

    for (uint32_t y = 0; y < result.tex_h; y += stride) {
        const auto* row = (const uint32_t*)(bytes + (size_t)footprint.Footprint.RowPitch * y);
        for (uint32_t x = 0; x < result.tex_w; x += stride) {
            if (row[x] == 0) continue;
            ++result.nonzero_pixels;
            if (result.min_x < 0 || (int32_t)x < result.min_x) result.min_x = (int32_t)x;
            if (result.min_y < 0 || (int32_t)y < result.min_y) result.min_y = (int32_t)y;
            if ((int32_t)x > result.max_x) result.max_x = (int32_t)x;
            if ((int32_t)y > result.max_y) result.max_y = (int32_t)y;
        }
    }

    readback_buffer->Unmap(0, nullptr);
    result.succeeded = true;
    return result;
}
} // namespace

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[D3D12 VR] Could not set up, trying again next frame");
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d12_hook();

    hook->set_next_present_interval(0); // disable vsync for vr
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};
    auto ue4_texture = VR::get()->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    const auto ui_should_invert_alpha = vr->get_overlay_component().should_invert_ui_alpha();

    // Update the UI overlay.
    auto runtime = vr->get_runtime();

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // DIAG: unbiased visibility into the AFR/Synchronized-Sequential eye-selection state.
    // NOTE: this used to sample every 300th call ("++diag_afr_count % 300 == 1"). Since
    // diag_afr_count and vr->m_render_frame_count both increment by 1 every frame, a fixed
    // stride of 300 (an even number) always lands on the exact same frame-count parity, which
    // made it LOOK like is_left_eye_frame was permanently false when in fact only every other
    // frame's diagnostic was ever printed. We now log every frame for a short warm-up window
    // (so both parities are actually observed), and thereafter log every frame whenever the
    // eye selection doesn't alternate as expected (a real symptom of one eye going black),
    // plus a low-frequency heartbeat the rest of the time.
    // All three DIAG blocks below are gated behind "DIAG: Verbose Sync/Stall Logging" (Debug section).
    if (vr->is_diag_verbose_logging_enabled()) {
        static uint32_t diag_afr_count = 0;
        static bool diag_last_was_left = false;
        static bool diag_have_seen_left = false;
        static bool diag_have_seen_right = false;
        ++diag_afr_count;

        if (is_left_eye_frame) {
            diag_have_seen_left = true;
        }
        if (is_right_eye_frame) {
            diag_have_seen_right = true;
        }

        const bool warm_up = diag_afr_count <= 60;
        const bool stuck_on_one_eye = diag_afr_count > 120 && (!diag_have_seen_left || !diag_have_seen_right);

        if (warm_up || stuck_on_one_eye || diag_afr_count % 301 == 1) {
            SPDLOG_INFO("[DIAG] AFR eye state (#{}): is_actually_afr={} is_afr={} is_same_frame={} is_left_eye_frame={} is_right_eye_frame={} frame_count={} left_interval={} right_interval={} seen_left={} seen_right={}",
                diag_afr_count, is_actually_afr, is_afr, is_same_frame, is_left_eye_frame, is_right_eye_frame,
                vr->m_render_frame_count, vr->m_left_eye_interval, vr->m_right_eye_interval, diag_have_seen_left, diag_have_seen_right);
        }

        diag_last_was_left = is_left_eye_frame;
    }

    // DIAG: identity/binding check for the resource the engine actually handed us this frame.
    // If the engine is rendering the left eye into a DIFFERENT resource than what we're tracking
    // as "backbuffer" (e.g. a stale/alternate render target after an AFR transition or resize),
    // this would explain a permanently-black left eye that has nothing to do with our x-offset
    // cropping logic: we'd simply be sampling/copying the wrong (never-written-to) resource.
    // Also logs the ue4_texture wrapper pointer and whether backbuffer == real_backbuffer, since
    // a change in either between left-eye frames (while staying stable for right-eye frames)
    // would point directly at the engine's render-target binding as the root cause.
    if (vr->is_diag_verbose_logging_enabled()) {
        static uint32_t diag_rt_identity_count = 0;
        static void* diag_last_left_backbuffer = nullptr;
        static void* diag_last_right_backbuffer = nullptr;
        ++diag_rt_identity_count;

        void* const current_backbuffer_ptr = backbuffer.Get();
        void* const current_real_backbuffer_ptr = real_backbuffer.Get();
        void* const current_ue4_texture_ptr = (void*)ue4_texture;

        bool changed_for_this_eye = false;

        if (is_left_eye_frame) {
            changed_for_this_eye = diag_last_left_backbuffer != nullptr && diag_last_left_backbuffer != current_backbuffer_ptr;
            diag_last_left_backbuffer = current_backbuffer_ptr;
        } else if (is_right_eye_frame) {
            changed_for_this_eye = diag_last_right_backbuffer != nullptr && diag_last_right_backbuffer != current_backbuffer_ptr;
            diag_last_right_backbuffer = current_backbuffer_ptr;
        }

        if (diag_rt_identity_count <= 60 || changed_for_this_eye || diag_rt_identity_count % 301 == 1) {
            SPDLOG_INFO("[DIAG] RT identity (#{}): backbuffer={:p} real_backbuffer={:p} ue4_texture={:p} same_as_real={} is_left_eye_frame={} is_right_eye_frame={} changed_since_last_same_eye_frame={}",
                diag_rt_identity_count, current_backbuffer_ptr, current_real_backbuffer_ptr, current_ue4_texture_ptr,
                current_backbuffer_ptr == current_real_backbuffer_ptr, is_left_eye_frame, is_right_eye_frame, changed_for_this_eye);
        }
    }

    // DIAG: source-side pixel readback. Samples the actual backbuffer resource we're about to
    // copy from, BEFORE any of our copy/compositor logic runs. If this consistently reports
    // nonzero_pixels=0 (fully black), the black screen originates upstream in UE's own scene
    // render (or the wrong render target is being handed to us) and is NOT a bug in our AFR
    // copy/OpenXR submission path. If this reports real pixel data but the destination swapchain
    // sample (further below) is black, the break is in our copy/submission path instead.
    // Throttled hard because this stalls the GPU synchronously.
    if (vr->is_diag_verbose_logging_enabled()) {
        static uint32_t diag_src_sample_count = 0;
        ++diag_src_sample_count;

        // NOTE: throttle stride must be ODD. diag_src_sample_count increments once per on_frame call
        // (i.e. once per eye), so a fixed EVEN stride always lands on the same eye-parity frame,
        // making the throttled samples look permanently biased toward one eye when in fact both are
        // being processed - this previously caused the tail of a log to show only is_left_eye_frame=true
        // even though right-eye frames were also occurring in between (just never sampled).
        if (diag_src_sample_count <= 5 || diag_src_sample_count % 301 == 1) {
            const auto bb_desc = backbuffer.Get() != nullptr ? backbuffer->GetDesc() : D3D12_RESOURCE_DESC{};
            const auto bb_width = (uint32_t)bb_desc.Width;
            const auto bb_height = (uint32_t)bb_desc.Height;

            // Sample from the CENTER of each eye's half of the backbuffer (not the fixed top-left
            // corner), so we get a representative signal from inside actual rendered content rather
            // than a possibly-black letterbox/border pixel. Also sample BOTH halves every time,
            // independent of is_left_eye_frame/is_right_eye_frame, so we can directly see whether the
            // left half of the shared backbuffer ever contains real geometry regardless of which
            // eye's "turn" this frame nominally is.
            const auto half_w = bb_width / 2;
            const auto left_center_x = half_w > 32 ? half_w / 2 - 16 : 0;
            const auto right_center_x = half_w > 32 ? half_w + half_w / 2 - 16 : half_w;
            const auto center_y = bb_height > 32 ? bb_height / 2 - 16 : 0;

            const auto left_sample = diag_sample_texture(device, command_queue, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, left_center_x, center_y);
            const auto right_sample = diag_sample_texture(device, command_queue, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, right_center_x, center_y);

            if (left_sample.succeeded) {
                SPDLOG_INFO("[DIAG] SOURCE backbuffer LEFT-HALF-CENTER pixel sample (#{}): bb={}x{} sample_xy=({},{}) avg_luminance={:.2f} nonzero={}/{} corner=0x{:08X} center=0x{:08X} is_left_eye_frame={} is_right_eye_frame={}",
                    diag_src_sample_count, bb_width, bb_height, left_center_x, center_y, left_sample.avg_luminance, left_sample.nonzero_pixels, left_sample.sampled_pixels,
                    left_sample.corner_pixel, left_sample.center_pixel, is_left_eye_frame, is_right_eye_frame);
            } else {
                SPDLOG_INFO("[DIAG] SOURCE backbuffer LEFT-HALF-CENTER pixel sample (#{}): FAILED to sample.", diag_src_sample_count);
            }

            if (right_sample.succeeded) {
                SPDLOG_INFO("[DIAG] SOURCE backbuffer RIGHT-HALF-CENTER pixel sample (#{}): bb={}x{} sample_xy=({},{}) avg_luminance={:.2f} nonzero={}/{} corner=0x{:08X} center=0x{:08X} is_left_eye_frame={} is_right_eye_frame={}",
                    diag_src_sample_count, bb_width, bb_height, right_center_x, center_y, right_sample.avg_luminance, right_sample.nonzero_pixels, right_sample.sampled_pixels,
                    right_sample.corner_pixel, right_sample.center_pixel, is_left_eye_frame, is_right_eye_frame);
            } else {
                SPDLOG_INFO("[DIAG] SOURCE backbuffer RIGHT-HALF-CENTER pixel sample (#{}): FAILED to sample.", diag_src_sample_count);
            }
        }
    }

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;
    const auto ui_target = ffsr->get_render_target_manager()->get_ui_target();

    const auto frame_count = vr->m_render_frame_count;

    if (m_game_tex.texture.Get() == nullptr && backbuffer.Get() == real_backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as copy of backbuffer");
        
        ComPtr<ID3D12Resource> backbuffer_copy{};
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto desc = backbuffer->GetDesc();
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        m_backbuffer_copy.reset();

        ComPtr<ID3D12Resource> backbuffer_copy2{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy2)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy2.Get(), std::nullopt, std::nullopt, L"Backbuffer Copy")) {
            spdlog::error("[VR] Failed to fully setup backbuffer copy.");
            m_backbuffer_copy.reset();
        }

        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // UE backbuffer is not VR compatible, so we need to copy it to a new texture with this one.

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_game_tex.setup(device, backbuffer_copy.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        } else {
            for (auto& commands : m_game_tex_commands) {
                commands.setup(L"Game Texture Commands");
            }
        }
    } else if (backbuffer.Get() != real_backbuffer.Get() && m_game_tex.texture.Get() != backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as reference to original");

        if (!m_game_tex.setup(device, backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        }
    }

    // Check if local player exists via UEVR API
    bool is_world_loading = false;

    if (uevr::API::get() != nullptr) {
        // If the local player controller doesn't exist yet, the level is still streaming
        if (uevr::API::get()->get_player_controller(0) == nullptr) {
            is_world_loading = true;
        }
    }

    if (vr->is_native_stereo_fix_enabled() && !is_world_loading) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = scene_capture != nullptr ? (ID3D12Resource*)scene_capture->get_native_resource() : nullptr;

        if (scene_capture_rt != nullptr && m_scene_capture_tex.texture.Get() != scene_capture_rt) {
            spdlog::info("[VR] Setting up scene capture texture as reference to original");

            if (!m_scene_capture_tex.setup(
                    device, scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Scene Capture Texture")) {
                spdlog::error("[VR] Failed to fully setup scene capture texture.");
                m_scene_capture_tex.reset();
            }
        }

        if (scene_capture_rt == nullptr && m_scene_capture_tex.texture.Get() != nullptr) {
            spdlog::info("[VR] Resetting scene capture texture");
            m_scene_capture_tex.reset();
        }
    } else {
        m_scene_capture_tex.reset();
    }

    // We need to render the scene capture texture to the right side of the double wide texture
    auto pre_render = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (render_target == nullptr) {
            return;
        }

        const auto dst_eye_width = m_backbuffer_size[0] / 2;
        const auto dst_eye_height = m_backbuffer_size[1];

        // The left (game) texture was historically guaranteed to match the destination eye
        // dimensions, but the game's dynamic resolution/scene-capture resizing (introduced by a
        // performance update) means the game texture's actual size can now differ from the VR
        // per-eye render target size. A raw CopyTextureRegion can only crop, not scale, so when
        // the sizes mismatch it produces an out-of-bounds/black-looking left eye. When the sizes
        // differ, fall back to a shader blit (render_srv_to_rtv) so the game texture is scaled
        // into the destination region, mirroring the right eye's scene capture handling below.
        if (m_game_tex.texture != nullptr && m_game_tex.srv_heap != nullptr) {
            const auto game_tex_desc = m_game_tex.texture->GetDesc();

            // The game texture is crop-compatible (fast CopyTextureRegion, no scaling needed) whenever
            // its height matches the destination eye height and its width is at least the destination
            // eye width. This covers both the "single-eye-sized" texture case (Width == dst_eye_width)
            // and the "full double-wide backbuffer" case (Width == 2 * dst_eye_width), where the left
            // eye is simply the left-most dst_eye_width x dst_eye_height region. Only fall back to the
            // shader blit path when the game texture is genuinely smaller than the destination (true
            // upscaling required), since the blit path allocates a new SRV/RTV heap that can fail and
            // has been observed to crash the D3D12 device (DXGI_ERROR_DEVICE_REMOVED) when misused.
            const auto left_sizes_match = game_tex_desc.Width >= dst_eye_width && game_tex_desc.Height == dst_eye_height;

            // Diagnostic: correlates the composited frame/backbuffer identity with the copy mode so we
            // can determine whether a partial-eye flicker lines up with backbuffer index reuse, AFR
            // state, or same-frame duplication rather than a scale mismatch.
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] left eye composite: game_tex={}x{} dst_eye={}x{} mode={} frame={} bb_idx={} is_afr={} is_same_frame={} native_stereo_fix={}",
                game_tex_desc.Width, game_tex_desc.Height,
                dst_eye_width, dst_eye_height,
                left_sizes_match ? "copy" : "blit",
                vr->m_render_frame_count,
                swapchain->GetCurrentBackBufferIndex(),
                is_afr,
                is_same_frame,
                vr->is_native_stereo_fix_enabled());

            if (left_sizes_match) {
                D3D12_BOX left_src_box{
                    .left = 0,
                    .top = 0,
                    .front = 0,
                    .right = dst_eye_width,
                    .bottom = dst_eye_height,
                    .back = 1
                };

                commands.copy_region(
                    m_game_tex.texture.Get(), render_target, &left_src_box,
                    0, 0, 0,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_RENDER_TARGET
                );
            } else {
                // Wrap the destination render target so render_srv_to_rtv can target it directly.
                if (m_stereo_dst_tex.texture.Get() != render_target) {
                    if (!m_stereo_dst_tex.setup(device, render_target, std::nullopt, std::nullopt, L"Stereo Dest Texture")) {
                        spdlog::error("[VR] Failed to setup stereo destination texture for left eye blit.");
                        m_stereo_dst_tex.reset();
                    }
                }

                if (m_stereo_dst_tex.texture.Get() != nullptr && m_stereo_dst_tex.rtv_heap != nullptr) {
                    const RECT left_dest_rect{
                        0, 0,
                        (LONG)dst_eye_width, (LONG)dst_eye_height
                    };

                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_tex,
                        m_stereo_dst_tex,
                        std::nullopt,
                        left_dest_rect,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET
                    );
                }
            }
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] left eye composite: game texture is NULL, left eye will not be copied this frame");
        }

        // The scene capture texture (right eye, native stereo fix) can be reallocated to the
        // HMD's native per-eye resolution independently of the game's backbuffer resolution, so
        // its actual dimensions can differ significantly from the destination eye region. A raw
        // CopyTextureRegion cannot scale between mismatched resolutions, it can only crop, which
        // produces a corrupted/black-looking right eye. When the sizes differ, use a shader blit
        // (render_srv_to_rtv) instead so the scene capture is scaled into the destination region.
        if (m_scene_capture_tex.texture != nullptr && m_scene_capture_tex.srv_heap != nullptr) {
            const auto scene_capture_desc = m_scene_capture_tex.texture->GetDesc();

            const auto sizes_match = scene_capture_desc.Width == dst_eye_width && scene_capture_desc.Height == dst_eye_height;

            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] right eye composite: scene_capture={}x{} dst_eye={}x{} mode={} game_tex={}",
                scene_capture_desc.Width, scene_capture_desc.Height,
                dst_eye_width, dst_eye_height,
                sizes_match ? "copy" : "blit",
                (void*)m_game_tex.texture.Get());

            if (sizes_match) {
                D3D12_BOX right_src_box{
                    .left = 0, .top = 0, .front = 0,
                    .right = dst_eye_width, .bottom = dst_eye_height, .back = 1
                };

                commands.copy_region(
                    m_scene_capture_tex.texture.Get(), render_target, &right_src_box,
                    dst_eye_width, 0, 0,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_RENDER_TARGET
                );
            } else {
                // Wrap the destination render target so render_srv_to_rtv can target it directly.
                if (m_stereo_dst_tex.texture.Get() != render_target) {
                    if (!m_stereo_dst_tex.setup(device, render_target, std::nullopt, std::nullopt, L"Stereo Dest Texture")) {
                        spdlog::error("[VR] Failed to setup stereo destination texture for right eye blit.");
                        m_stereo_dst_tex.reset();
                    }
                }

                if (m_stereo_dst_tex.texture.Get() != nullptr && m_stereo_dst_tex.rtv_heap != nullptr) {
                    const RECT dest_rect{
                        (LONG)dst_eye_width, 0,
                        (LONG)m_backbuffer_size[0], (LONG)dst_eye_height
                    };

                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_stereo_dst_tex,
                        std::nullopt,
                        dest_rect,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET
                    );
                }
            }
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] right eye composite: scene capture texture is NULL, right eye will not be copied this frame");
        }
    };

    // For copying the real backbuffer if we need to
    if (m_game_tex.texture.Get() != nullptr && backbuffer == real_backbuffer) {
        const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
        auto& command_ctx = m_game_tex_commands[idx];
        if (command_ctx.cmd_list != nullptr) {
            command_ctx.wait(INFINITE);
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            command_ctx.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_ctx.copy(real_backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            //m_game_tex_commands[idx].copy(backbuffer.Get(), m_game_tex.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, ENGINE_SRC_COLOR);
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                command_ctx.cmd_list.Get(),
                m_backbuffer_copy,
                m_game_tex,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            command_ctx.execute();
        }

        backbuffer = m_game_tex.texture;
    }

    if (ui_target != nullptr) {
        if (m_game_ui_tex.texture.Get() != ui_target->get_native_resource()) {
            if (!m_game_ui_tex.setup(device, 
                (ID3D12Resource*)ui_target->get_native_resource(), 
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                L"Game UI Texture"))
            {
                spdlog::error("[VR] Failed to fully setup game UI texture.");
                m_game_ui_tex.reset();
            }
        }

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            // Force a UI target reallocation whenever the tall-UI mode changes (NSF on/off or the
            // "Fit UI To Full Canvas" toggle), so the redirected UI target is re-sized to match the
            // new desired dimensions dynamically without requiring a game restart. Without this, the
            // engine only reallocates on its own (resolution/menu changes), leaving a stale UI target
            // that looks compressed/stretched until something else triggers a resize.
            static bool s_last_tall_ui = false;
            const bool tall_ui_now = vr->is_native_stereo_fix_tall_ui_enabled();
            if (tall_ui_now != s_last_tall_ui) {
                SPDLOG_INFO("[VR] Tall-UI mode changed ({} -> {}), forcing UI target reallocation", s_last_tall_ui, tall_ui_now);
                s_last_tall_ui = tall_ui_now;
                ffsr->set_should_recreate_textures(true);
            }

            const auto native = (ID3D12Resource*)ui_target->get_native_resource();
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                const auto desc = native->GetDesc();

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                } else if (m_game_ui_tex.texture != nullptr) {
                    const auto ui_desc = m_game_ui_tex.texture->GetDesc();

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        ffsr->set_should_recreate_textures(true);
                    }
                }
            } else if (native == nullptr) {
                spdlog::error("[VR] Recreating UI texture because native resource is null");
                ffsr->set_should_recreate_textures(true);
            }
        }
    } else {
        m_game_ui_tex.reset(); // Probably fixes non-resident errors.
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const auto is_2d_screen = vr->is_using_2d_screen();

    // LGUI (redirected into ui_target) only paints the top-left hmd_w x hmd_h region when native stereo fix is on
    // (measured via LGUI_BOUNDS). In 2D-screen mode the UI texture is composited over the 2D screen with a full-texture
    // blit, so that region ended up in the top-left corner. Blit just the painted region, stretched over the screen.
    std::optional<RECT> ui_src_rect{};
    {
        const auto ext = ffsr->get_ui_draw_extent();
        if (m_game_ui_tex.texture.Get() != nullptr && ext.width > 0 && ext.height > 0) {
            const auto ui_desc = m_game_ui_tex.texture->GetDesc();
            if (ext.width < (int32_t)ui_desc.Width || ext.height < (int32_t)ui_desc.Height) {
                ui_src_rect = RECT{0, 0, (LONG)ext.width, (LONG)ext.height};
            }
        }

        static std::optional<RECT> last_rect{};
        static bool have_last = false;
        const bool changed = !have_last || ui_src_rect.has_value() != last_rect.has_value() ||
            (ui_src_rect.has_value() && (ui_src_rect->right != last_rect->right || ui_src_rect->bottom != last_rect->bottom));
        if (changed) {
            have_last = true;
            last_rect = ui_src_rect;
            SPDLOG_INFO("[LGUI_BLIT] 2d-screen UI src rect = {} (2d_screen={} nsf={})",
                ui_src_rect ? fmt::format("{}x{}", ui_src_rect->right, ui_src_rect->bottom) : std::string{"full"}, is_2d_screen, vr->is_native_stereo_fix_enabled());
        }
    }

    auto draw_2d_view = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (ui_should_invert_alpha && m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
            d3d12::render_srv_to_rtv(m_ui_batch_alpha_invert.get(), commands.cmd_list.Get(), m_game_ui_tex, m_game_ui_tex, std::nullopt, ENGINE_SRC_COLOR, ENGINE_SRC_COLOR);
        }

        draw_spectator_view(commands.cmd_list.Get(), is_right_eye_frame);

        if (is_2d_screen && m_game_tex.texture.Get() != nullptr && m_game_tex.srv_heap != nullptr) {
            // Clear previous frame
            for (auto& screen : m_2d_screen_tex) {
                commands.clear_rtv(screen, clear_color, ENGINE_SRC_COLOR);
            }

            // Render left side to left screen tex
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                commands.cmd_list.Get(),
                m_game_tex,
                m_2d_screen_tex[0],
                RECT{0, 0, (LONG)((float)m_backbuffer_size[0] / 2.0f), (LONG)m_backbuffer_size[1]},
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR
            );

            if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_ui_tex,
                    m_2d_screen_tex[0],
                    ui_src_rect,
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );
            }

            if (!is_afr) {
                // Render right side to right screen tex
                if (m_scene_capture_tex.texture.Get() != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                } else {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_tex,
                        m_2d_screen_tex[1],
                        RECT{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]},
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ui_src_rect,
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            } else {
                // BUGFIX: previously the right 2D-mode screen
                // cleared to black and never drawn to when is_afr is true, leaving it permanently
                // black in 2D mode for AFR games (which is the common case). In AFR, m_game_tex is
                // the shared double-wide backbuffer containing BOTH eyes side-by-side (this is the
                // same texture/layout composite_afr_eye crops from for the actual VR eyes above), so
                // crop its right half here too instead of skipping this screen entirely.
                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_tex,
                    m_2d_screen_tex[1],
                    RECT{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]},
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ui_src_rect,
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            }

            // Clear the RT so the entire background is black when submitting to the compositor
            commands.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);

            if (m_scene_capture_tex.texture.Get() != nullptr) {
                commands.clear_rtv(m_scene_capture_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    };

    // Draws the spectator view
    auto clear_rt = [&](d3d12::CommandContext& commands) {
        if (m_game_ui_tex.texture.Get() == nullptr) {
            return;
        }

        const float ui_clear_color[] = { 0.0f, 0.0f, 0.0f, ui_should_invert_alpha ? 1.0f : 0.0f };
        commands.clear_rtv(m_game_ui_tex, (float*)&ui_clear_color, ENGINE_SRC_COLOR);
    };

    // NOTE: the actual LGUI redirect (forcing LGUI to draw into ui_target instead of its native RHI texture) is
    // gated at its source in FFakeStereoRenderingHook::game_viewport_client_draw_hook via
    // VR::is_lgui_ui_redirect_disabled(). When disabled there, ui_target is never populated by LGUI, so this
    // submission path naturally has nothing new to present each frame (falling back to whatever was last there,
    // typically blank/stale) rather than needing a separate skip here.
    if (runtime->is_openvr() && m_openvr.ui_tex.texture.Get() != nullptr) {
        m_openvr.ui_tex.commands.wait(INFINITE);

        draw_2d_view(m_openvr.ui_tex.commands, nullptr);

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            } else if (ui_target != nullptr) {
                m_openvr.ui_tex.commands.copy((ID3D12Resource*)ui_target->get_native_resource(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            }
        } else if (is_2d_screen) {
            m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
        }

        clear_rt(m_openvr.ui_tex.commands);
        m_openvr.ui_tex.commands.execute();
    } else if (runtime->is_openxr() && runtime->ready() && vr->m_openxr->frame_began) {
        // DIAG: measure the actual painted bounds of the LGUI ui_target every ~5s (or on mode change) so we know
        // exactly what sub-rect the redirected UI occupies in each NSF/2D/VR state, independent of any view rect guess.
        // NOTE: diag_content_bounds does a BLOCKING full-target GPU readback; with retries this stalls the pipeline on
        // many consecutive frames and causes noticeable lag. It has already established that the painted region tracks
        // the per-eye view rect, so it is disabled by default. Flip LGUI_BOUNDS_DIAG to true only when re-measuring.
        constexpr bool LGUI_BOUNDS_DIAG = false; // Option B validation: confirm paint height now fits within 2160; set false after.
        if (LGUI_BOUNDS_DIAG && ui_target != nullptr) {
            static uint32_t bounds_counter = 0;
            static bool last_2d = false, last_nsf = false;
            static uint32_t last_hmd_w = 0, last_hmd_h = 0;
            // A resolution change trigger often lands a frame or two before LGUI actually paints at the new size
            // (its first captured frame after a change is frequently empty: bbox=(-1,-1) nonzero_samples=0, wasting
            // the sample). Keep retrying on every frame for a short window after a change/tick until we get real
            // painted pixels, instead of giving up after a single empty capture.
            static uint32_t retries_left = 0;
            const bool nsf_now = vr->is_native_stereo_fix_enabled();
            const auto hmd_w_now = vr->get_hmd_width();
            const auto hmd_h_now = vr->get_hmd_height();
            // Also trigger a sample immediately when the HMD/eye render resolution changes (not just 2D/NSF mode),
            // since that's the value that actually varies when testing in-game resolution changes while in VR -
            // the previous 300-frame-only tick missed most resolution changes during a quick test pass.
            const bool mode_changed = is_2d_screen != last_2d || nsf_now != last_nsf || hmd_w_now != last_hmd_w || hmd_h_now != last_hmd_h;
            const bool tick = (++bounds_counter % 300) == 0;
            if (mode_changed) {
                retries_left = 30; // ~0.5s at 60fps worth of retry frames
            }
            if (mode_changed || tick || retries_left > 0) {
                last_2d = is_2d_screen;
                last_nsf = nsf_now;
                last_hmd_w = hmd_w_now;
                last_hmd_h = hmd_h_now;
                const auto native = (ID3D12Resource*)ui_target->get_native_resource();
                const auto b = diag_content_bounds(device, command_queue, native, ENGINE_SRC_COLOR);
                const auto ext = ffsr->get_ui_draw_extent();
                if (b.succeeded) {
                    if (b.nonzero_pixels > 0) {
                        retries_left = 0;
                    } else if (retries_left > 0) {
                        --retries_left;
                    }
                    SPDLOG_INFO("[LGUI_BOUNDS] ui_target {}x{} painted bbox=({},{})-({},{}) size={}x{} nonzero_samples={} | reported_extent={}x{} 2d_screen={} nsf={} hmd={}x{}",
                        b.tex_w, b.tex_h, b.min_x, b.min_y, b.max_x, b.max_y,
                        b.max_x >= 0 ? b.max_x - b.min_x + 4 : 0, b.max_y >= 0 ? b.max_y - b.min_y + 4 : 0, b.nonzero_pixels,
                        ext.width, ext.height, is_2d_screen, nsf_now, vr->get_hmd_width(), vr->get_hmd_height());
                } else {
                    if (retries_left > 0) --retries_left;
                    SPDLOG_INFO("[LGUI_BOUNDS] readback failed for ui_target {:x}", (uintptr_t)native);
                }
            }
        }

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                if (is_afr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, std::nullopt, ENGINE_SRC_COLOR);
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1].texture.Get(), std::nullopt, clear_rt, ENGINE_SRC_COLOR);
                }
            } else if (ui_target != nullptr) {
                // PERF (Option A): the redirected LGUI ui_target is sized to the full per-eye canvas (e.g.
                // 3557x4209 with NSF tall-UI), but LGUI only paints a top-left sub-rect (get_ui_draw_extent),
                // and the quad only presents that same sub-rect. Copying the WHOLE target every frame blits
                // tens of MB of dead pixels - a major steady-state cost. Crop the copy to just the painted
                // region via a src box. CopyTextureRegion writes to dst (0,0), matching the quad's top-left
                // crop, so this is visually identical while moving far fewer bytes.
                const auto ext = ffsr->get_ui_draw_extent();
                const auto native_ui = (ID3D12Resource*)ui_target->get_native_resource();

                D3D12_BOX ui_box{};
                D3D12_BOX* ui_box_ptr = nullptr;

                // Only apply the crop optimization if native_ui is valid; if it's null, fall back to full copy
                if (native_ui != nullptr) {
                    const auto ui_desc = native_ui->GetDesc();
                    if (ext.width > 0 && ext.height > 0 &&
                        (UINT)ext.width <= ui_desc.Width && (UINT)ext.height <= ui_desc.Height &&
                        ((UINT)ext.width < ui_desc.Width || (UINT)ext.height < ui_desc.Height)) {
                        ui_box.left = 0;
                        ui_box.top = 0;
                        ui_box.front = 0;
                        ui_box.right = (UINT)ext.width;
                        ui_box.bottom = (UINT)ext.height;
                        ui_box.back = 1;
                        ui_box_ptr = &ui_box;
                    }
                } else {
                    SPDLOG_WARN("[D3D12Component] LGUI ui_target native_ui is nullptr - falling back to full target copy. ui_target={:p}", (void*)ui_target);
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, native_ui, draw_2d_view, clear_rt, ENGINE_SRC_COLOR, ui_box_ptr);
            }

            auto fw_rt = g_framework->get_rendertarget_d3d12();

            // DIAG: measure what actually got composited for the UI layer this frame (post-blit), same cadence as LGUI_BOUNDS.
            // If this bbox is still hmd-sized top-left, our blit/crop is not what is being displayed.
            // NOTE: also a BLOCKING GPU readback - gated behind LGUI_BOUNDS_DIAG so it does not cause lag in normal use.
            if (LGUI_BOUNDS_DIAG) {
                static uint32_t out_counter = 0;
                static bool out_last_2d = false, out_last_nsf = false;
                const bool nsf_now = vr->is_native_stereo_fix_enabled();
                const bool changed = is_2d_screen != out_last_2d || nsf_now != out_last_nsf;
                if (changed || (++out_counter % 300) == 0) {
                    out_last_2d = is_2d_screen;
                    out_last_nsf = nsf_now;
                    ID3D12Resource* dst = nullptr;
                    const char* what = "none";
                    if (is_2d_screen && m_2d_screen_tex[0].texture.Get() != nullptr) {
                        dst = m_2d_screen_tex[0].texture.Get();
                        what = "2d_screen_tex[0]";
                    } else if (!is_2d_screen) {
                        if (auto it = m_openxr.contexts.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI); it != m_openxr.contexts.end() && !it->second.textures.empty()) {
                            const auto idx = std::min<size_t>(it->second.last_acquired_texture, it->second.textures.size() - 1);
                            dst = it->second.textures[idx].texture;
                            what = "ui_swapchain";
                        }
                    }
                    if (dst != nullptr) {
                        const auto b = diag_content_bounds(device, command_queue, dst, is_2d_screen ? ENGINE_SRC_COLOR : D3D12_RESOURCE_STATE_RENDER_TARGET);
                        SPDLOG_INFO("[LGUI_OUT] {} {}x{} painted bbox=({},{})-({},{}) nonzero_samples={} | src_rect={} 2d_screen={} nsf={}",
                            what, b.tex_w, b.tex_h, b.min_x, b.min_y, b.max_x, b.max_y, b.nonzero_pixels,
                            ui_src_rect ? fmt::format("{}x{}", ui_src_rect->right, ui_src_rect->bottom) : std::string{"full"}, is_2d_screen, nsf_now);
                    } else {
                        SPDLOG_INFO("[LGUI_OUT] no destination texture available (2d_screen={})", is_2d_screen);
                    }
                }
            }

            if (fw_rt && g_framework->is_drawing_anything()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, g_framework->get_rendertarget_d3d12().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } else if (is_2d_screen) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
        } else if (m_game_ui_tex.commands.ready()) {
            m_game_ui_tex.commands.wait(INFINITE);
            draw_2d_view(m_game_ui_tex.commands, nullptr);
            clear_rt(m_game_ui_tex.commands);
            m_game_ui_tex.commands.execute();
        }
    }

    /*else if (m_game_tex.texture.Get() != nullptr) {
        m_game_tex.commands.wait(INFINITE);
        draw_spectator_view(m_game_tex.commands.cmd_list.Get(), is_right_eye_frame);
        m_game_tex.commands.execute();
    }*/

    ComPtr<ID3D12Resource> scene_depth_tex{};

    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    // The AFR left/right eye swapchains are created once at setup time, sized to the OpenXR
    // runtime's fixed per-eye resolution (get_hmd_width()/get_hmd_height()), which is completely
    // independent of the game's own backbuffer resolution. When the game uses dynamic resolution
    // scaling, m_backbuffer_size can change frame-to-frame while the destination swapchain stays
    // fixed. A raw CopyTextureRegion (used below) can only crop, never scale, so whenever these
    // sizes diverge the copy silently produces a corrupted/black-looking eye instead of erroring -
    // this is the same class of bug already identified and fixed for the Native Stereo Fix path
    // (see the "left_sizes_match"/"sizes_match" handling in the pre_render lambda above). Fall back
    // to a shader blit (render_srv_to_rtv) via m_stereo_dst_tex whenever the destination swapchain's
    // actual size doesn't match the half of the game texture we intend to crop out for this eye.
    auto composite_afr_eye = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target, bool right_half, const char* debug_label) {
        if (render_target == nullptr) {
            return;
        }

        if (m_game_tex.texture.Get() == nullptr || m_game_tex.srv_heap == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] {} composite (AFR): game texture is NULL, eye will not be copied this frame", debug_label);
            return;
        }

        const auto game_tex_desc = m_game_tex.texture->GetDesc();
        const auto dst_desc = render_target->GetDesc();

        const auto dst_eye_width = (uint32_t)dst_desc.Width;
        const auto dst_eye_height = (uint32_t)dst_desc.Height;

        const auto extreme_compat = vr->is_extreme_compatibility_mode_enabled();
        const auto half_width = extreme_compat ? (uint32_t)game_tex_desc.Width : (uint32_t)game_tex_desc.Width / 2;

        // The engine's own AdjustViewRect call independently decides which physical x-offset half
        // of the double-wide backbuffer each eye's scene actually gets rendered into. That decision
        // is driven by AdjustViewRect's own local index/index_starts_from_one state, which is NOT
        // guaranteed to stay in phase with is_left_eye_frame/right_half here (derived from a separate
        // frame-parity counter, vr->m_render_frame_count). If the two fall out of phase, cropping a
        // hardcoded half (x=0 for left, x=half_width for right) silently grabs the WRONG eye's content,
        // producing a permanently-black eye despite the engine rendering both eyes correctly. Prefer
        // the real, engine-reported x-offset for this eye whenever it's available and sane.
        const auto& ffsr = VR::get()->m_fake_stereo_hook;
        auto src_x_offset = (right_half && !extreme_compat) ? (uint32_t)game_tex_desc.Width - half_width : 0;

        if (!extreme_compat && ffsr != nullptr && ffsr->has_seen_eye_x_offsets()) {
            const auto reported_x_offset = right_half ? ffsr->get_last_right_eye_x_offset() : ffsr->get_last_left_eye_x_offset();

            // Only trust the reported offset if it actually lands within the game texture bounds for
            // a half-width crop (guards against stale/uninitialized values or a mode where AdjustViewRect
            // isn't driving eye layout at all, e.g. Native Stereo Fix).
            if (reported_x_offset + half_width <= (uint32_t)game_tex_desc.Width) {
                if (reported_x_offset != src_x_offset) {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] {} composite (AFR): using engine-reported x_offset={} instead of assumed={}",
                        debug_label, reported_x_offset, src_x_offset);
                }

                src_x_offset = reported_x_offset;
            }
        }

        // The engine's true eye-index state machine (driven by AdjustViewRect) can become skewed
        // for several consecutive frames toward one eye (observed: right_calls climbing while
        // left_calls stalls), independent of our own frame-parity-based left/right classification.
        // When that happens, blindly copying here for an eye whose AdjustViewRect offset hasn't
        // actually been refreshed since our last copy just re-stamps a stale/black crop of the
        // shared backbuffer over the last good frame for that eye. Skip the copy in that case and
        // leave the destination swapchain image as-is (last known good frame) instead.
        if (!extreme_compat && ffsr != nullptr && ffsr->has_seen_eye_x_offsets()) {
            static uint64_t s_last_left_update_count = 0;
            static uint64_t s_last_right_update_count = 0;

            const auto current_update_count = right_half ? ffsr->get_right_eye_x_offset_update_count() : ffsr->get_left_eye_x_offset_update_count();
            auto& last_update_count = right_half ? s_last_right_update_count : s_last_left_update_count;

            if (current_update_count == last_update_count) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[VR] {} composite (AFR): skipping copy, engine has not refreshed this eye's view rect since last copy (update_count={})",
                    debug_label, current_update_count);
                return;
            }

            last_update_count = current_update_count;
        }

        const auto sizes_match = half_width == dst_eye_width && (uint32_t)game_tex_desc.Height == dst_eye_height;

        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] {} composite (AFR): game_tex={}x{} half={}x{} dst_eye={}x{} mode={} extreme_compat={}",
            debug_label, (uint32_t)game_tex_desc.Width, (uint32_t)game_tex_desc.Height, half_width, (uint32_t)game_tex_desc.Height,
            dst_eye_width, dst_eye_height, sizes_match ? "copy" : "blit", extreme_compat);

        if (sizes_match) {
            D3D12_BOX box{};
            box.left = src_x_offset;
            box.top = 0;
            box.front = 0;
            box.right = src_x_offset + half_width;
            box.bottom = (uint32_t)game_tex_desc.Height;
            box.back = 1;

            commands.copy_region(
                m_game_tex.texture.Get(), render_target, &box,
                0, 0, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
        } else {
            // Wrap the destination render target so render_srv_to_rtv can target it directly.
            if (m_stereo_dst_tex.texture.Get() != render_target) {
                if (!m_stereo_dst_tex.setup(device, render_target, std::nullopt, std::nullopt, L"Stereo Dest Texture (AFR)")) {
                    spdlog::error("[VR] Failed to setup stereo destination texture for AFR eye blit.");
                    m_stereo_dst_tex.reset();
                }
            }

            if (m_stereo_dst_tex.texture.Get() != nullptr && m_stereo_dst_tex.rtv_heap != nullptr) {
                const RECT src_rect{
                    (LONG)src_x_offset, 0,
                    (LONG)(src_x_offset + half_width), (LONG)game_tex_desc.Height
                };

                const RECT dest_rect{
                    0, 0,
                    (LONG)dst_eye_width, (LONG)dst_eye_height
                };

                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_tex,
                    m_stereo_dst_tex,
                    src_rect,
                    dest_rect,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_RENDER_TARGET
                );
            }
        }
    };

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            // DIAG: log of the actual copy performed into the AFR left-eye swapchain. Logs every
            // frame for a short warm-up window (unbiased by parity, unlike a fixed modulo stride),
            // then falls back to a low-frequency heartbeat.
            if (vr->is_diag_verbose_logging_enabled()) {
                static uint32_t diag_left_copy_count = 0;
                ++diag_left_copy_count;
                if (diag_left_copy_count <= 60 || diag_left_copy_count % 300 == 1) {
                    SPDLOG_INFO("[DIAG] AFR_LEFT_EYE copy (#{}) [is_left_eye_frame branch]: swapchain_idx={} backbuffer={}x{} extreme_compat={} frame_count={}",
                        diag_left_copy_count, (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, m_backbuffer_size[0], m_backbuffer_size[1],
                        vr->is_extreme_compatibility_mode_enabled(), vr->m_render_frame_count);
                }
            }

            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, nullptr,
                [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
                    composite_afr_eye(commands, render_target, false, "left eye");
                }, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);

            if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture
        if (runtime->is_openvr()) {
            m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::VRTextureWithPose_t left_eye{
                (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                // DIAG: throttled log of the actual copy performed into the AFR left-eye swapchain
                // from the "else" (right-eye-frame) branch's catch-up left-eye copy.
                if (vr->is_diag_verbose_logging_enabled()) {
                    static uint32_t diag_left_catchup_count = 0;
                    if (++diag_left_catchup_count % 300 == 1) {
                        SPDLOG_INFO("[DIAG] AFR_LEFT_EYE copy (#{}) [catch-up branch]: backbuffer={}x{} extreme_compat={}",
                            diag_left_catchup_count, m_backbuffer_size[0], m_backbuffer_size[1], vr->is_extreme_compatibility_mode_enabled());
                    }
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, nullptr,
                    [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
                        composite_afr_eye(commands, render_target, false, "left eye (catch-up)");
                    }, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }

            if (is_actually_afr) {
                // DIAG: log of the actual copy performed into the AFR right-eye swapchain. Logs every
                // frame for a short warm-up window, then falls back to a low-frequency heartbeat.
                if (vr->is_diag_verbose_logging_enabled()) {
                    static uint32_t diag_right_copy_count = 0;
                    ++diag_right_copy_count;
                    if (diag_right_copy_count <= 60 || diag_right_copy_count % 300 == 1) {
                        SPDLOG_INFO("[DIAG] AFR_RIGHT_EYE copy (#{}): swapchain_idx={} backbuffer={}x{} is_afr={} extreme_compat={} frame_count={}",
                            diag_right_copy_count, (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, m_backbuffer_size[0], m_backbuffer_size[1],
                            is_afr, vr->is_extreme_compatibility_mode_enabled(), vr->m_render_frame_count);
                    }
                }

                // NOTE: Regardless of is_afr, the right eye must always be sourced from the right half
                // of the double-wide backbuffer (or the whole backbuffer in extreme compat mode). See
                // composite_afr_eye for the size-mismatch-safe crop/blit logic.
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, nullptr,
                    [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
                        composite_afr_eye(commands, render_target, true, "right eye");
                    }, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            } else {
                // Copy over the entire double wide instead
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, pre_render, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                }

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left and right eye textures.
        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            if (!is_afr) {
                m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    //return e; // dont return because it will just completely stop us from even getting to the right eye which could be catastrophic
                }
            }

            if (!is_afr) {
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openvr.copy_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    m_openvr.copy_left_to_right(m_scene_capture_tex.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                m_openvr.copy_left_to_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{
                (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    if (is_right_eye_frame) {
        if ((runtime->ready() && vr->get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE) || !runtime->got_first_sync) {
            //vr->update_hmd_state();
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (!vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            std::vector<XrCompositionLayerBaseHeader*> quad_layers{};

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (vr->m_2d_screen_mode->value()) {
                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }
            
            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }

            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex.Get() != nullptr);

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame(quad_layers);
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        /*if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                m_generic_commands[frame_count % 3].wait(INFINITE);
                m_generic_commands[frame_count % 3].copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                m_generic_commands[frame_count % 3].execute();
            }
        }*/
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

std::unique_ptr<DirectX::DX12::SpriteBatch> D3D12Component::setup_sprite_batch_pso(
    DXGI_FORMAT output_format, 
    std::span<const uint8_t> ps, 
    std::span<const uint8_t> vs, 
    std::optional<DirectX::SpriteBatchPipelineStateDescription> pd) 
{
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    if (!pd) {
        pd = DirectX::SpriteBatchPipelineStateDescription{DirectX::RenderTargetState{output_format, DXGI_FORMAT_UNKNOWN}};
    }

    if (ps.size() > 0) {
        pd->customPixelShader = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
    }

    if (vs.size() > 0) {
        pd->customVertexShader = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
    }

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, *pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");

    return batch;
}

void D3D12Component::draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame) {
    if (command_list == nullptr || m_game_ui_tex.texture == nullptr) {
        return;
    }

    if (m_game_ui_tex.srv_heap == nullptr || m_game_ui_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    if (m_game_tex.texture == nullptr || m_game_tex.srv_heap == nullptr || m_game_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    const auto& vr = VR::get();

    if (!vr->is_hmd_active() || !vr->m_desktop_fix->value()) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    const auto desc = backbuffer->GetDesc();

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        spdlog::error("[VR] Backbuffer RTV heap is null (D3D12)");
        return;
    }

    // Copy the previous right eye frame to the left eye frame
    const auto prev_index = (index + m_backbuffer_textures.size() - 1) % m_backbuffer_textures.size();
    if (vr->is_using_afr() && !is_right_eye_frame && m_backbuffer_textures[prev_index]->texture != nullptr) {
        const auto& last_right_eye_buffer = m_backbuffer_textures[prev_index]->texture;

        if (backbuffer.Get() != last_right_eye_buffer.Get()) {
            m_generic_commands[index % 3].wait(INFINITE);
            m_generic_commands[index % 3].copy(last_right_eye_buffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
            m_generic_commands[index % 3].execute();

            return;
        }
    }

    auto& batch = m_backbuffer_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MaxDepth = 1.0f;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)desc.Width;
    scissor_rect.bottom = (LONG)desc.Height;

    // Transition backbuffer to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { backbuffer_ctx.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Clear backbuffer
    const float bb_clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    command_list->ClearRenderTargetView(backbuffer_ctx.get_rtv(), bb_clear_color, 0, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

    const auto aspect_ratio = (float)desc.Width / (float)desc.Height;

    const auto eye_width = ((float)m_backbuffer_size[0] / 2.0f);
    const auto eye_height = (float)m_backbuffer_size[1];
    const auto eye_aspect_ratio = eye_width / eye_height;

    const auto original_centerw = (float)eye_width / 2.0f;
    const auto original_centerh = (float)eye_height / 2.0f;

    ///////////////
    // Eye (game) texture
    ///////////////
    // only show one half of the double wide texture (right side)
    RECT source_rect{};

    // Show left side when using AFR or native stereo fix
    if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled()) {
        source_rect.left = 0;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0] / 2;
        source_rect.bottom = m_backbuffer_size[1];
    } else {
        source_rect.left = (LONG)m_backbuffer_size[0] / 2;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0];
        source_rect.bottom = m_backbuffer_size[1];
    }

    // Correct left/top/right/bottom to match the aspect ratio of the game
    if (eye_aspect_ratio > aspect_ratio) {
        const auto new_width = eye_height * aspect_ratio;
        const auto new_centerw = new_width / 2.0f;
        source_rect.left = (LONG)(original_centerw - new_centerw);
        source_rect.right = (LONG)(original_centerw + new_centerw);
    } else {
        const auto new_height = eye_width / aspect_ratio;
        const auto new_centerh = new_height / 2.0f;
        source_rect.top = (LONG)(original_centerh - new_centerh);
        source_rect.bottom = (LONG)(original_centerh + new_centerh);
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { m_game_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(m_game_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)m_backbuffer_size[0], (uint32_t)m_backbuffer_size[1] },
        dest_rect,
        &source_rect, 
        DirectX::Colors::White);

    //////
    // UI
    //////
    // Set descriptor heaps
    ID3D12DescriptorHeap* ui_heaps[] = { m_game_ui_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, ui_heaps);

    batch->Draw(m_game_ui_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height },
        dest_rect, 
        DirectX::Colors::White);

    batch->End();

    // Transition backbuffer to D3D12_RESOURCE_STATE_PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &barrier);
}

void D3D12Component::clear_backbuffer() {
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (backbuffer == nullptr) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    // oh well
    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        return;
    }

    // Clear the backbuffer
    backbuffer_ctx.commands.wait(0);
    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    backbuffer_ctx.commands.clear_rtv(backbuffer_ctx.texture.Get(), backbuffer_ctx.get_rtv(), clear_color, D3D12_RESOURCE_STATE_PRESENT);
    backbuffer_ctx.commands.execute();
}

void D3D12Component::on_post_present(VR* vr) {
    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();

        auto device = hook->get_device();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    if (vr->is_hmd_active()) {
        clear_backbuffer();
    }
}

void D3D12Component::on_reset(VR* vr) {
    m_force_reset = true;

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& commands : m_generic_commands) {
        commands.reset();
    }

    for (auto& commands : m_game_tex_commands) {
        commands.reset();
    }

    for (auto& backbuffer : m_backbuffer_textures) {
        backbuffer.reset();
    }

    for (auto & screen : m_2d_screen_tex) {
        screen.reset();
    }

    m_openvr.ui_tex.reset();
    m_game_ui_tex.reset();
    m_game_tex.reset();
    m_scene_capture_tex.reset();

    // m_stereo_dst_tex wraps a raw swapchain backbuffer pointer (see composite_afr_eye), so if we
    // don't release it here, it keeps an outstanding reference to the OLD swapchain buffer alive
    // across ResizeBuffers. DXGI requires ALL outstanding references to swapchain buffers to be
    // released before ResizeBuffers can succeed; holding onto a stale one here caused
    // DXGI_ERROR_INVALID_CALL / DXGI_ERROR_DEVICE_REMOVED on resize.
    m_stereo_dst_tex.reset();
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_batch_alpha_invert.reset();
    m_graphics_memory.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        m_openxr.wait_for_all_copies();

        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D12Resource> scene_depth_tex{rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }


        const auto ui_target_size = FFakeStereoRenderingHook::get_ui_target_size();

        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() ||
            vr->m_openxr->swapchains.empty() ||
            (uint32_t)ui_target_size.width != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].width ||
            (uint32_t)ui_target_size.height != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].height ||
            m_last_afr_state != vr->is_using_afr() ||
            needs_depth_resize)
        {
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_prev_backbuffer.Reset();
    m_openvr.texture_counter = 0;
}

bool D3D12Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up d3d12 textures...");

    auto vr = VR::get();
    on_reset(vr.get());
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer (D3D12).");
        return false;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D12).");
        return false;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    auto backbuffer_desc = backbuffer->GetDesc();

    spdlog::info("[VR] D3D12 Real backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, (uint32_t)real_backbuffer_desc.Format);

    backbuffer_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer_desc.Width /= 2; // The texture we get from UE is both eyes combined. we will copy the regions later.
    }

    spdlog::info("[VR] D3D12 RT width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format);

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    if (vr->is_using_2d_screen()) {
        auto screen_desc = backbuffer_desc;
        screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        for (auto& context : m_2d_screen_tex) {
            ComPtr<ID3D12Resource> screen_tex{};
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &screen_desc, ENGINE_SRC_COLOR, nullptr,
                    IID_PPV_ARGS(&screen_tex)))) {
                spdlog::error("[VR] Failed to create 2D screen texture.");
                continue;
            }

            screen_tex->SetName(L"2D Screen Texture");

            if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
                spdlog::error("[VR] Failed to setup 2D screen context.");
                continue;
            }
        }
    }

    if (vr->get_runtime()->is_openvr()) {
        for (auto& ctx : m_openvr.left_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Left Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Left Eye")) {
                spdlog::error("[VR] Failed to setup left eye context.");
                return false;
            }
        }

        for (auto& ctx : m_openvr.right_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Right Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Right Eye")) {
                spdlog::error("[VR] Failed to setup right eye context.");
                return false;
            }
        }

        // Set up the UI texture. It matches the (possibly taller) UI target so the full LGUI canvas is captured.
        auto ui_desc = backbuffer_desc;
        const auto ui_target_size = FFakeStereoRenderingHook::get_ui_target_size();
        ui_desc.Width = (uint32_t)ui_target_size.width;
        ui_desc.Height = (uint32_t)ui_target_size.height;

        ComPtr<ID3D12Resource> ui_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ui_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&ui_tex)))) {
            spdlog::error("[VR] Failed to create UI texture.");
            return false;
        }

        ui_tex->SetName(L"OpenVR UI Texture");

        if (!m_openvr.ui_tex.setup(device, ui_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"OpenVR UI")) {
            spdlog::error("[VR] Failed to setup OpenVR UI context.");
            return false;
        }
    }

    for (auto& commands : m_generic_commands) {
        if (!commands.setup(L"Generic commands")) {
            return false;
        }
    }

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        m_backbuffer_size[0] = backbuffer_desc.Width * 2;
    } else {
        m_backbuffer_size[0] = backbuffer_desc.Width;
    }

    m_backbuffer_size[1] = backbuffer_desc.Height;

    m_backbuffer_batch = setup_sprite_batch_pso(real_backbuffer_desc.Format);
    m_game_batch = setup_sprite_batch_pso(backbuffer_desc.Format);

    // Custom blend state to flip the alpha in-place of the UI texture without an intermediate render target
    {
        DirectX::SpriteBatchPipelineStateDescription invert_alpha_in_place_pd{DirectX::RenderTargetState{backbuffer_desc.Format, DXGI_FORMAT_UNKNOWN}};

        auto& bd = invert_alpha_in_place_pd.blendDesc;
        auto& bdrt = bd.RenderTarget[0];
        bdrt.BlendEnable = TRUE;

        bdrt.SrcBlend = D3D12_BLEND_ONE;
        bdrt.DestBlend = D3D12_BLEND_ZERO;
        bdrt.BlendOp = D3D12_BLEND_OP_ADD;

        bdrt.SrcBlendAlpha = D3D12_BLEND_ONE;
        bdrt.DestBlendAlpha = D3D12_BLEND_ZERO;
        bdrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bdrt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        m_ui_batch_alpha_invert = setup_sprite_batch_pso(
            backbuffer_desc.Format, 
            alpha_luminance_sprite_ps_SpritePixelShader, 
            alpha_luminance_sprite_ps_SpriteVertexShader, 
            invert_alpha_in_place_pd
        );
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;

    return true;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }

    // ... Rest of your original create_swapchains code continues as normal below ...
    
    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (backbuffer == nullptr && FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, const D3D12_RESOURCE_DESC& desc) -> std::optional<std::string> {
        // Create the swapchain.
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);
        
        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);

                auto& texture_ctx = ctx.texture_contexts[index];
                texture_ctx->texture = ctx.textures[index].texture;

                // Depth stencil textures don't need an RTV.
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                    if (ctx.texture_contexts[index]->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        texture_ctx->commands.clear_rtv(ctx.textures[index].texture, texture_ctx->get_rtv(), clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        texture_ctx->commands.execute();
                        texture_ctx->commands.wait(100);
                    } else {
                        spdlog::error("[VR] Failed to create RTV for swapchain image {}.", index);
                    }
                }

                texture_ctx->texture.Reset();
                texture_ctx->rtv_heap.reset();

                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    auto hmd_desc = backbuffer_desc;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();
    hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    hmd_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hmd_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;

    virtual_desktop_dummy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait

    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    // The UI swapchain must match the (possibly taller) UI target so LGUI's full per-eye canvas is captured.
    const auto ui_target_size = FFakeStereoRenderingHook::get_ui_target_size();
    desktop_rt_swapchain_create_info.width = (uint32_t)ui_target_size.width;
    desktop_rt_swapchain_create_info.height = (uint32_t)ui_target_size.height;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_desc.Width = (uint32_t)ui_target_size.width;
    desktop_rt_desc.Height = (uint32_t)ui_target_size.height;

    desktop_rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desktop_rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        //depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.DepthOrArraySize = 1;

        depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        depth_desc.Width = vr->get_hmd_width() * 2;
        depth_desc.Height = vr->get_hmd_height();

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_desc = depth_tex->GetDesc();

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);
            spdlog::info("[VR] Depth texture flags: {}", (uint32_t)depth_desc.Flags);

            if (depth_desc.Width > hmd_desc.Width || depth_desc.Height > hmd_desc.Height) {
                spdlog::info("[VR] Depth texture is larger than the HMD");
                //depth_desc.Width = hmd_desc.Width;
                //depth_desc.Height = hmd_desc.Height;
            }

            depth_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            this->made_depth_with_null_defaults = true;
            spdlog::error("[VR] Depth texture is null! Using default values");
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

    if (this->contexts.empty()) {
        return;
    }
    
    auto& vr = VR::get();
    std::scoped_lock __{vr->m_openxr->swapchain_mtx};

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        //ctx.texture_contexts.clear();
        for (auto& texture_context : ctx.texture_contexts) {
            if (texture_context != nullptr) {
                texture_context->reset();
            }
        }

        ctx.texture_contexts.clear();

        std::vector<ID3D12Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            const auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands, 
    std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands, 
    D3D12_RESOURCE_STATES src_state, 
    D3D12_BOX* src_box) 
{
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->get_synchronize_stage() != VR::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            // DIAG: GPU-time the submission recorded on each relevant swapchain's command context so we
            // can compare the redirected UI copy/clear/draw against the actual per-eye scene composite.
            // The UI path already measured ~0.14 ms (negligible); if the eye-composite path is also small
            // while FPS still drops in both NSF ON/OFF, the cost is the engine's per-eye scene render
            // (config-tunable), not our compositor.
            // PERF: this instrumentation forces CommandContext::end_gpu_timing() to set has_commands = true
            // every frame for every recognized swapchain (UI/DOUBLE_WIDE/AFR eyes), which in turn forces
            // execute() to always ExecuteCommandLists/Signal/SetEventOnCompletion even on frames that would
            // otherwise have nothing to submit here. Unlike the other [DIAG] readback helpers in this file,
            // this was not gated behind is_diag_verbose_logging_enabled(), so it silently ran unconditionally
            // in every build. Gate it the same way so it only costs anything while actively diagnosing.
            const wchar_t* timing_label = nullptr;
            if (vr->is_diag_verbose_logging_enabled()) {
                switch ((runtimes::OpenXR::SwapchainIndex)swapchain_idx) {
                case runtimes::OpenXR::SwapchainIndex::UI: timing_label = L"OpenXR UI copy+clear+draw"; break;
                case runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE: timing_label = L"OpenXR double-wide scene composite"; break;
                case runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE: timing_label = L"OpenXR AFR left-eye composite"; break;
                case runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE: timing_label = L"OpenXR AFR right-eye composite"; break;
                default: break;
                }
            }
            const bool time_ui = timing_label != nullptr;
            if (time_ui) {
                texture_ctx->commands.enable_gpu_timing(timing_label);
                texture_ctx->commands.begin_gpu_timing();
            }

            if (pre_commands) {
                (*pre_commands)(texture_ctx->commands, ctx.textures[texture_index].texture);
            }

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr) {
                if (src_box == nullptr) {
                    const auto is_depth = swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE;
                    const auto dst_state = is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

                    texture_ctx->commands.copy(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_state, 
                        dst_state);
                } else {
                    texture_ctx->commands.copy_region(
                        resource, 
                        ctx.textures[texture_index].texture, src_box,
                        src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }

            if (additional_commands) {
                (*additional_commands)(texture_ctx->commands);
            }

            if (time_ui) {
                texture_ctx->commands.end_gpu_timing();
            }

            texture_ctx->commands.execute();

            // DIAG: destination-side pixel readback for the AFR eye swapchains. Sampled AFTER
            // our copy has been recorded/executed, so this reflects exactly what will be
            // submitted to the OpenXR runtime for that eye. Compared against the SOURCE
            // backbuffer sample above, this tells us definitively whether the black image is:
            //   - already black in the source (upstream UE/engine rendering issue), or
            //   - fine in the source but black in the destination (bug in our copy/compositor).
            // Waited on the fence first so the copy has actually completed by the time we read.
            if (vr->is_diag_verbose_logging_enabled() &&
                (swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE ||
                 swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE)) {
                static uint32_t diag_dst_left_count = 0;
                static uint32_t diag_dst_right_count = 0;

                const bool is_left = swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE;
                auto& diag_count = is_left ? diag_dst_left_count : diag_dst_right_count;
                ++diag_count;

                if (diag_count <= 5 || diag_count % 300 == 1) {
                    texture_ctx->commands.wait(INFINITE);

                    auto& hook = g_framework->get_d3d12_hook();
                    auto device = hook->get_device();
                    auto command_queue = hook->get_command_queue();

                    const auto sample = diag_sample_texture(device, command_queue, ctx.textures[texture_index].texture, D3D12_RESOURCE_STATE_RENDER_TARGET);

                    if (sample.succeeded) {
                        SPDLOG_INFO("[DIAG] DEST {} swapchain pixel sample (#{}): swapchain_idx={} avg_luminance={:.2f} nonzero={}/{} corner=0x{:08X} center=0x{:08X}",
                            is_left ? "AFR_LEFT_EYE" : "AFR_RIGHT_EYE", diag_count, swapchain_idx,
                            sample.avg_luminance, sample.nonzero_pixels, sample.sampled_pixels, sample.corner_pixel, sample.center_pixel);
                    } else {
                        SPDLOG_INFO("[DIAG] DEST {} swapchain pixel sample (#{}): FAILED to sample.", is_left ? "AFR_LEFT_EYE" : "AFR_RIGHT_EYE", diag_count);
                    }
                }
            }

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod