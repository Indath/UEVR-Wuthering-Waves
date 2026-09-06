#pragma once

#include <chrono>
#include <mutex>
#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
struct TextureContext;

struct CommandContext {
    CommandContext() = default;
    virtual ~CommandContext() { this->reset(); }

    bool setup(const wchar_t* name = L"CommandContext object");
    void reset();
    void wait(uint32_t ms);
    void copy(ID3D12Resource* src, ID3D12Resource* dst, 
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void copy_region(ID3D12Resource* src, ID3D12Resource* dst, D3D12_BOX* src_box, 
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void copy_region(ID3D12Resource* src, ID3D12Resource* dst, 
        D3D12_BOX* src_box, UINT dst_x, UINT dst_y, UINT dst_z,
        D3D12_RESOURCE_STATES src_state, 
        D3D12_RESOURCE_STATES dst_state);
    void copy_region_stereo(ID3D12Resource* srcleft, ID3D12Resource* srcright, ID3D12Resource* dst, D3D12_BOX* srcleft_box, D3D12_BOX* srcright_box,
        UINT dstleft_x, UINT dstleft_y, UINT dstleft_z,
        UINT dstright_x, UINT dstright_y, UINT dstright_z,
        D3D12_RESOURCE_STATES src_state = D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void clear_rtv(ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float* color, 
        D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void clear_rtv(TextureContext& tex, const float* color, D3D12_RESOURCE_STATES dst_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void execute();

    // DIAG: optional GPU-side timing of everything recorded between begin_gpu_timing() and
    // end_gpu_timing() on this context's command list. Uses a timestamp query heap resolved into a
    // readback buffer; the previous frame's result is read back (lock-free, GPU already fenced) at
    // begin and logged via [GPU_TIMING]. Enable once with enable_gpu_timing(label).
    void enable_gpu_timing(const wchar_t* label);
    void begin_gpu_timing();
    void end_gpu_timing();

    bool ready() const {
        return this->cmd_list != nullptr && this->cmd_allocator != nullptr && this->fence != nullptr;
    }

    ComPtr<ID3D12CommandAllocator> cmd_allocator{};
    ComPtr<ID3D12GraphicsCommandList> cmd_list{};
    ComPtr<ID3D12Fence> fence{};
    UINT64 fence_value{};
    HANDLE fence_event{};

    std::recursive_mutex mtx{};

    bool waiting_for_fence{false};
    bool has_commands{false};

    std::wstring internal_name{L"CommandContext object"};

    // GPU timing (see enable_gpu_timing).
    ComPtr<ID3D12QueryHeap> timestamp_query_heap{};
    ComPtr<ID3D12Resource> timestamp_readback{};
    std::wstring gpu_timing_label{};
    bool gpu_timing_enabled{false};
    bool gpu_timing_active{false};   // begin recorded in the current command list recording
    bool gpu_timing_pending{false};  // resolve recorded, result readable after the next wait()
    double last_gpu_time_ms{0.0};
    std::chrono::steady_clock::time_point last_gpu_time_log{};  // per-instance log throttle
};
}