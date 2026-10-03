#pragma once

#include <iostream>
#include <functional>
#include <chrono>
#include <atomic>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi")

#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/PointerHook.hpp"
#include "utility/VtableHook.hpp"

class D3D12Hook
{
public:
	typedef std::function<void(D3D12Hook&)> OnPresentFn;
	typedef std::function<void(D3D12Hook&, uint32_t w, uint32_t h)> OnResizeBuffersFn;
    typedef std::function<void(D3D12Hook&, uint32_t w, uint32_t h)> OnResizeTargetFn;
    typedef std::function<void(D3D12Hook&)> OnCreateSwapChainFn;

	D3D12Hook() = default;
	virtual ~D3D12Hook();

	bool hook();
	bool unhook();

    bool is_hooked() {
        return m_hooked;
    }

    void on_present(OnPresentFn fn) {
        m_on_present = fn;
    }

    void on_post_present(OnPresentFn fn) {
        m_on_post_present = fn;
    }

    void on_resize_buffers(OnResizeBuffersFn fn) {
        m_on_resize_buffers = fn;
    }

    void on_resize_target(OnResizeTargetFn fn) {
        m_on_resize_target = fn;
    }

    /*void on_create_swap_chain(OnCreateSwapChainFn fn) {
        m_on_create_swap_chain = fn;
    }*/

    ID3D12Device4* get_device() const {
        return m_device;
    }

    IDXGISwapChain3* get_swap_chain() const {
        return m_swap_chain;
    }

    auto get_swapchain_0() { return m_swapchain_0; }
    auto get_swapchain_1() { return m_swapchain_1; }

    ID3D12CommandQueue* get_command_queue() const {
        return m_command_queue;
    }

    UINT get_display_width() const {
        return m_display_width;
    }

    UINT get_display_height() const {
        return m_display_height;
    }

    UINT get_render_width() const {
        return m_render_width;
    }

    UINT get_render_height() const {
        return m_render_height;
    }

    bool is_inside_present() const {
        return m_inside_present;
    }

    std::chrono::steady_clock::time_point get_present_enter_time() const {
        return m_present_enter_time;
    }

    bool is_proton_swapchain() const {
        return m_using_proton_swapchain;
    }

    bool is_framegen_swapchain() const {
        return m_using_frame_generation_swapchain;
    }

    void ignore_next_present() {
        m_ignore_next_present = true;
    }

    void set_next_present_interval(uint32_t interval) {
        m_next_present_interval = interval;
    }

    // DIAG accessors: let external watchdogs (e.g. Framework::hook_monitor) detect the case where
    // the hook was installed successfully but Present was never actually called through it before
    // the process terminates, which points at the patched vtable slots not being the ones the live
    // swapchain uses (or something reverting the patch) rather than a hang in our own code.
    bool has_seen_first_present() const {
        return m_first_present_seen;
    }

    // Unlike has_seen_first_present(), this only becomes true once a Present call has actually
    // made it all the way to invoking the registered on_present callback (i.e. Framework::on_frame_*),
    // which is what actually flips m_d3d_hook_ever_succeeded. A call can be "seen" (enter
    // present_internal) but never be "handled" if it gets bounced out early by WindowFilter or the
    // phase-1/og-instance swapchain-identity checks - which is exactly the gap that let the hook
    // monitor think the hook was still dead and churn through destructive rehooks.
    bool has_handled_first_present() const {
        return m_first_present_handled;
    }

    std::chrono::steady_clock::time_point get_hook_installed_time() const {
        return m_hook_installed_time;
    }

protected:
    ID3D12Device4* m_device{ nullptr };
    IDXGISwapChain3* m_swap_chain{ nullptr };
    IDXGISwapChain3* m_swapchain_0{};
    IDXGISwapChain3* m_swapchain_1{};
    ID3D12CommandQueue* m_command_queue{ nullptr };
    UINT m_display_width{ NULL };
    UINT m_display_height{ NULL };
    UINT m_render_width{ NULL };
    UINT m_render_height{ NULL };

    uint32_t m_command_queue_offset{};
    uint32_t m_proton_swapchain_offset{};

    std::optional<uint32_t> m_next_present_interval{};

    bool m_using_proton_swapchain{ false };
    bool m_using_frame_generation_swapchain{ false };
    bool m_hooked{ false };
    bool m_is_phase_1{ true };
    bool m_inside_present{false};
    std::chrono::steady_clock::time_point m_present_enter_time{};
    bool m_ignore_next_present{false};

    // DIAG/perf: once the dummy-swapchain RTTI type-info probe in hook() has thrown once this
    // process, skip it on subsequent rehook attempts (see hook() for rationale) instead of paying
    // its sometimes multi-second cost on every single rehook. Static because Framework::hook_d3d12()
    // constructs a brand new D3D12Hook instance on every rehook attempt, so a plain member would
    // reset back to false each time and never actually suppress repeat attempts.
    static inline bool m_skip_type_info_probe{false};

    // DIAG: track when the Present vtable hooks were installed and whether we've ever actually
    // observed a call come through them. If hook() logs success but the game process terminates
    // shortly afterward without m_first_present_seen ever flipping true, that proves Present was
    // never actually invoked through our patched pointers (e.g. anti-cheat reverted the patch, or
    // the patched vtable slots aren't the ones the live swapchain actually uses), rather than a
    // crash/hang happening deeper inside our own Present handling logic.
    std::chrono::steady_clock::time_point m_hook_installed_time{};
    std::atomic<bool> m_first_present_seen{false};
    std::atomic<bool> m_first_present_handled{false};
    std::atomic<bool> m_first_present_filtered_logged{false};
    void* m_hooked_present_fn_addr{nullptr};
    void* m_hooked_present1_fn_addr{nullptr};

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<PointerHook> m_present1_hook{};
    std::unique_ptr<VtableHook> m_swapchain_hook{};
    //std::unique_ptr<FunctionHook> m_create_swap_chain_hook{};

    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    OnResizeTargetFn m_on_resize_target{ nullptr };
    //OnCreateSwapChainFn m_on_create_swap_chain{ nullptr };
    
    static HRESULT present_internal(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params, bool present1 = false);

    static HRESULT WINAPI present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags);
    static HRESULT WINAPI present1(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static HRESULT WINAPI resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters);
    //static HRESULT WINAPI create_swap_chain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain);
};

