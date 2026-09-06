#include <spdlog/spdlog.h>
#include <utility/String.hpp>

#include "Framework.hpp"

#include "TextureContext.hpp"
#include "CommandContext.hpp"

namespace d3d12 {
bool CommandContext::setup(const wchar_t* name) {
    std::scoped_lock _{this->mtx};

    this->internal_name = name;

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    this->cmd_allocator.Reset();
    this->cmd_list.Reset();
    this->fence.Reset();

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&this->cmd_allocator)))) {
        spdlog::error("[VR] Failed to create command allocator for {}", utility::narrow(name));
        return false;
    }

    this->cmd_allocator->SetName(name);

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, this->cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&this->cmd_list)))) {
        spdlog::error("[VR] Failed to create command list for {}", utility::narrow(name));
        return false;
    }
    
    this->cmd_list->SetName(name);

    if (FAILED(device->CreateFence(this->fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&this->fence)))) {
        spdlog::error("[VR] Failed to create fence for {}", utility::narrow(name));
        return false;
    }

    this->fence->SetName(name);
    this->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

void CommandContext::reset() {
    std::scoped_lock _{this->mtx};
    this->wait(2000);
    //this->on_post_present(VR::get().get());

    this->cmd_allocator.Reset();
    this->cmd_list.Reset();
    this->fence.Reset();
    this->fence_value = 0;
    CloseHandle(this->fence_event);
    this->fence_event = 0;
    this->waiting_for_fence = false;
}

void CommandContext::wait(uint32_t ms) {
    std::scoped_lock _{this->mtx};

	if (this->fence_event && this->waiting_for_fence) {
		const auto wait_result = WaitForSingleObject(this->fence_event, ms);

		if (wait_result != WAIT_OBJECT_0) {
			// GPU has not actually finished with this command allocator/list yet (timeout or wait failure).
			// Resetting them now would be undefined behavior while the GPU may still be executing commands
			// from them, which can corrupt driver state / leak GPU memory and produce garbage output.
			// Leave waiting_for_fence set so we retry safely on the next call instead.
			spdlog::error("[VR] Timed out (or failed) waiting for fence for {} (result={}), deferring reset", utility::narrow(this->internal_name), (uint32_t)wait_result);
			return;
		}

		ResetEvent(this->fence_event);
		this->waiting_for_fence = false;
		if (FAILED(this->cmd_allocator->Reset())) {
			spdlog::error("[VR] Failed to reset command allocator for {}", utility::narrow(this->internal_name));
		}

		if (FAILED(this->cmd_list->Reset(this->cmd_allocator.Get(), nullptr))) {
			spdlog::error("[VR] Failed to reset command list for {}", utility::narrow(this->internal_name));
		}
		this->has_commands = false;
	}
}

void CommandContext::copy(ID3D12Resource* src, ID3D12Resource* dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy");
        return;
    }

    // Switch src into copy source.
    D3D12_RESOURCE_BARRIER src_barrier{};

    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    this->cmd_list->CopyResource(dst, src);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

void CommandContext::copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region");
        return;
    }

    // Switch src into copy source.
    D3D12_RESOURCE_BARRIER src_barrier{};

    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = dst;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    this->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, src_box);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

void CommandContext::copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, UINT dst_x, UINT dst_y, UINT dst_z, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (src == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region");
        return;
    }

    // Switch src into copy source.
    D3D12_RESOURCE_BARRIER src_barrier{};

    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    src_barrier.Transition.pResource = src;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = src_state;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    // Copy the resource.
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = dst;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    this->cmd_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &src_loc, src_box);

    // Switch back to present.
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    src_barrier.Transition.StateAfter = src_state;
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    dst_barrier.Transition.StateAfter = dst_state;

    {
        D3D12_RESOURCE_BARRIER barriers[2]{src_barrier, dst_barrier};
        this->cmd_list->ResourceBarrier(2, barriers);
    }

    this->has_commands = true;
}

// More optimal than two copy_region calls.
void CommandContext::copy_region_stereo(ID3D12Resource* srcleft, ID3D12Resource* srcright, ID3D12Resource* dst, D3D12_BOX* srcleft_box, D3D12_BOX* srcright_box,
    UINT dstleft_x, UINT dstleft_y, UINT dstleft_z,
    UINT dstright_x, UINT dstright_y, UINT dstright_z,
    D3D12_RESOURCE_STATES src_state,
    D3D12_RESOURCE_STATES dst_state)
{
    std::scoped_lock _{this->mtx};

    if (srcleft == nullptr || srcright == nullptr || dst == nullptr) {
        spdlog::error("[VR] nullptr passed to copy_region_stereo");
        return;
    }

#ifdef UEVR_TRACE_STEREO_COPY
    {
        // Throttled per-frame trace to confirm the right-eye source/dest are being copied with sane values
        // without flooding the log every frame.
        static uint64_t s_call_count = 0;
        if ((s_call_count++ % 300) == 0) {
            spdlog::debug("[VR] copy_region_stereo trace #{}: srcleft={} srcright={} dst={} "
                          "left_box=({},{},{})-({},{},{}) right_box=({},{},{})-({},{},{}) "
                          "dstleft=({},{},{}) dstright=({},{},{}) src_state={} dst_state={}",
                s_call_count, (void*)srcleft, (void*)srcright, (void*)dst,
                srcleft_box->left, srcleft_box->top, srcleft_box->front,
                srcleft_box->right, srcleft_box->bottom, srcleft_box->back,
                srcright_box->left, srcright_box->top, srcright_box->front,
                srcright_box->right, srcright_box->bottom, srcright_box->back,
                dstleft_x, dstleft_y, dstleft_z,
                dstright_x, dstright_y, dstright_z,
                (int)src_state, (int)dst_state);
        }
    }
#endif

    // Transition states to copy source / dest.
    D3D12_RESOURCE_BARRIER barriers[3]
    {
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcleft, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE} },
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcright, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE} },
        { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, dst_state, D3D12_RESOURCE_STATE_COPY_DEST} }
    };
    
    this->cmd_list->ResourceBarrier(3, barriers);

    // Copy left half
    D3D12_TEXTURE_COPY_LOCATION src_loc_left = { srcleft, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
    D3D12_TEXTURE_COPY_LOCATION dst_loc = { dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };

    this->cmd_list->CopyTextureRegion(&dst_loc, dstleft_x, dstleft_y, dstleft_z, &src_loc_left, srcleft_box);

    // Copy right half
    D3D12_TEXTURE_COPY_LOCATION src_loc_right = { srcright, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };

    this->cmd_list->CopyTextureRegion(&dst_loc, dstright_x, dstright_y, dstright_z, &src_loc_right, srcright_box);

    // Transition states back to original.
    barriers[0] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcleft, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state} };
    barriers[1] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {srcright, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state} };
    barriers[2] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, dst_state} };

    this->cmd_list->ResourceBarrier(3, barriers);

    this->has_commands = true;
}

void CommandContext::clear_rtv(ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float* color, D3D12_RESOURCE_STATES dst_state) {
    std::scoped_lock _{this->mtx};

    if (dst == nullptr) {
        spdlog::error("[VR] nullptr passed to clear_rtv");
        return;
    }

    // Switch dst into copy destination.
    D3D12_RESOURCE_BARRIER dst_barrier{};
    dst_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    dst_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    dst_barrier.Transition.pResource = dst;
    dst_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dst_barrier.Transition.StateBefore = dst_state;
    dst_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // No need to switch if we're already in the right state.
    if (dst_state != dst_barrier.Transition.StateAfter) {
        D3D12_RESOURCE_BARRIER barriers[1]{dst_barrier};
        this->cmd_list->ResourceBarrier(1, barriers);
    }

    // Clear the resource.
    this->cmd_list->ClearRenderTargetView(rtv, color, 0, nullptr);

    // Switch back to present.
    dst_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    dst_barrier.Transition.StateAfter = dst_state;

    if (dst_state != dst_barrier.Transition.StateBefore) {
        D3D12_RESOURCE_BARRIER barriers[1]{dst_barrier};
        this->cmd_list->ResourceBarrier(1, barriers);
    }

    this->has_commands = true;
}

void CommandContext::clear_rtv(d3d12::TextureContext& tex, const float* color, D3D12_RESOURCE_STATES dst_state) {
    if (tex.texture == nullptr || tex.rtv_heap == nullptr) {
        return;
    }

    this->clear_rtv(tex.texture.Get(), tex.get_rtv(), color, dst_state);
}

void CommandContext::execute() {
    std::scoped_lock _{this->mtx};

    if (this->has_commands) {
        // If a GPU timing region was recorded this frame, resolve the two timestamps into the
        // readback buffer before closing so the result is available after the next fence wait.
        if (this->gpu_timing_active && this->timestamp_query_heap != nullptr && this->timestamp_readback != nullptr) {
            this->cmd_list->ResolveQueryData(this->timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, this->timestamp_readback.Get(), 0);
            this->gpu_timing_active = false;
            this->gpu_timing_pending = true;
        }

        if (FAILED(this->cmd_list->Close())) {
            spdlog::error("[VR] Failed to close command list. ({})", utility::narrow(this->internal_name));
            return;
        }
        
        auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();
        ID3D12CommandList* const cmd_lists[] = {this->cmd_list.Get()};
        command_queue->ExecuteCommandLists(1, cmd_lists);
        command_queue->Signal(this->fence.Get(), ++this->fence_value);
        this->fence->SetEventOnCompletion(this->fence_value, this->fence_event);
        this->waiting_for_fence = true;
        this->has_commands = false;
    }
}

void CommandContext::enable_gpu_timing(const wchar_t* label) {
    std::scoped_lock _{this->mtx};

    this->gpu_timing_label = label;

    if (this->timestamp_query_heap != nullptr && this->timestamp_readback != nullptr) {
        this->gpu_timing_enabled = true;
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        return;
    }

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    qd.NodeMask = 0;

    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&this->timestamp_query_heap)))) {
        spdlog::error("[VR] Failed to create timestamp query heap for {}", utility::narrow(this->internal_name));
        return;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(uint64_t) * 2;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&this->timestamp_readback)))) {
        spdlog::error("[VR] Failed to create timestamp readback buffer for {}", utility::narrow(this->internal_name));
        this->timestamp_query_heap.Reset();
        return;
    }

    this->gpu_timing_enabled = true;
}

void CommandContext::begin_gpu_timing() {
    std::scoped_lock _{this->mtx};

    if (!this->gpu_timing_enabled || this->timestamp_query_heap == nullptr || this->cmd_list == nullptr) {
        return;
    }

    // The previous submission has completed (wait() was called before re-recording), so its
    // resolved timestamps are safe to read back now.
    if (this->gpu_timing_pending && this->timestamp_readback != nullptr) {
        this->gpu_timing_pending = false;

        uint64_t* mapped = nullptr;
        D3D12_RANGE read_range{0, sizeof(uint64_t) * 2};

        if (SUCCEEDED(this->timestamp_readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped))) && mapped != nullptr) {
            const uint64_t start = mapped[0];
            const uint64_t end = mapped[1];

            D3D12_RANGE write_range{0, 0};
            this->timestamp_readback->Unmap(0, &write_range);

            auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();
            uint64_t freq = 0;

            if (command_queue != nullptr && SUCCEEDED(command_queue->GetTimestampFrequency(&freq)) && freq != 0 && end >= start) {
                this->last_gpu_time_ms = (double)(end - start) * 1000.0 / (double)freq;

                // Per-instance throttle (NOT the shared-static SPDLOG_INFO_EVERY_N_SEC macro): that
                // macro's static timer lives at this call site and would be shared across every
                // CommandContext/label, starving out all but the first label to log each second.
                const auto now = std::chrono::steady_clock::now();
                if (now - this->last_gpu_time_log >= std::chrono::seconds(1)) {
                    this->last_gpu_time_log = now;
                    SPDLOG_INFO("[GPU_TIMING] {} = {:.3f} ms", utility::narrow(this->gpu_timing_label), this->last_gpu_time_ms);
                }
            }
        }
    }

    this->cmd_list->EndQuery(this->timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    this->gpu_timing_active = true;
}

void CommandContext::end_gpu_timing() {
    std::scoped_lock _{this->mtx};

    if (!this->gpu_timing_active || this->timestamp_query_heap == nullptr || this->cmd_list == nullptr) {
        return;
    }

    this->cmd_list->EndQuery(this->timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    // Ensure execute() runs (and resolves) even if no other commands were recorded.
    this->has_commands = true;
}
}