/*
This file (Plugin.cpp) is licensed under the MIT license and is separate from the rest of the UEVR codebase.

Copyright (c) 2023 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// GamepadCursorPlugin
//
// Purpose: Replace the closed-source `gamepad_to_mouse.dll` with a native UEVR plugin
// that drives real OS mouse input from a standard XInput gamepad (no positional/6DoF
// tracking assumed). Most Unreal Engine games (including this one) read RELATIVE mouse
// deltas via raw input, not absolute cursor position, so this plugin injects relative
// MOUSEEVENTF_MOVE deltas via SendInput rather than calling SetCursorPos (which only
// repositions the desktop cursor and is ignored by the game).
//
// Controls:
//  - Left stick / D-Pad: move the cursor (relative deltas, NOT tied to HMD orientation)
//  - A (XINPUT_GAMEPAD_A): left mouse click
//  - B (XINPUT_GAMEPAD_B): sends Escape key (common UI "back"/"close" action)
//  - Back/Select (XINPUT_GAMEPAD_BACK): toggles cursor emulation on/off
//
// An in-game ImGui window ("Gamepad Cursor Plugin") is rendered (both in the VR view
// and on the desktop mirror) allowing live tuning of sensitivity/deadzone without
// having to hand-edit any config file.

#include <sstream>
#include <mutex>
#include <memory>
#include <optional>

#include <Windows.h>
#include <XInput.h>

#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"

using namespace uevr;

// The game's native UI (Wuthering Waves) is NOT a fixed-distance screen-space
// overlay. WutheringWaves_UIFix.lua repositions/rescales the actual in-world
// UI plane every frame via an LGUIScreenSpaceInteraction actor:
//   ScreenSpaceActor = ScreenSpace:get_outer()
//   UI_Location_Target = camera_pos + (forward * UIWorldPlaneDistance) + (up * UIHeightOffset)
//   ScreenSpaceActor:K2_SetActorLocation(UI_Location_Target, ...)
// A fixed relative-mouse sensitivity has no idea how far away/how large that
// plane currently is, so as the Lua script changes UIWorldPlaneDistance /
// UIWorldScale on the fly the emulated cursor drifts increasingly offset from
// the panel and stops lining up with anything. To compensate, each frame we
// resolve the live actor, read its actual world position via K2_GetActorLocation,
// and derive a distance-based sensitivity scale plus continuous re-centering
// bias so the cursor tracks the panel instead of a static assumption.
namespace ui_tracking {
	struct Vec3 { float x{}, y{}, z{}; };

	// Cached class/actor lookups. Resolved lazily since UObjects may not exist
	// yet at plugin init (level not loaded, UI not spawned, etc.).
	inline API::UClass* g_screenspace_class{nullptr};
	inline API::UObject* g_screenspace_component{nullptr};
	inline API::UObject* g_screenspace_actor{nullptr};

	inline API::UObject* find_screenspace_actor() {
		if (g_screenspace_class == nullptr) {
			g_screenspace_class = API::get()->find_uobject<API::UClass>(L"Class /Script/LGUI.LGUIScreenSpaceInteraction");
		}

		if (g_screenspace_class == nullptr) {
			return nullptr;
		}

		// The component instance can be respawned when the UI is rebuilt, so
		// don't permanently cache a stale pointer; re-resolve if the cached
		// object no longer resolves to something valid.
		auto component = API::UObjectHook::get_first_object_by_class(g_screenspace_class, false);

		if (component == nullptr) {
			component = API::UObjectHook::get_first_object_by_class(g_screenspace_class, true);
		}

		if (component == nullptr) {
			g_screenspace_component = nullptr;
			g_screenspace_actor = nullptr;
			return nullptr;
		}

		g_screenspace_component = component;
		g_screenspace_actor = component->get_outer();
		return g_screenspace_actor;
	}

	// Calls the actor's K2_GetActorLocation UFunction and returns the world
	// position, mirroring how the Lua script reads/writes this same actor's
	// transform every frame.
	inline std::optional<Vec3> get_actor_location(API::UObject* actor) {
		if (actor == nullptr) {
			return std::nullopt;
		}

		struct {
			Vec3 return_value{};
		} params{};

		actor->call_function(L"K2_GetActorLocation", &params);
		return params.return_value;
	}
}

class GamepadCursorPlugin : public uevr::Plugin {
public:
	GamepadCursorPlugin() = default;

	void on_dllmain() override {}

	void on_initialize() override {
		API::get()->log_info("[GamepadCursorPlugin] Initializing native gamepad UI-nav key emulation.");

		// Force this thread to be per-monitor-DPI-aware. Without this,
		// GetClientRect/ClientToScreen/GetSystemMetrics(SM_C*VIRTUALSCREEN) can
		// return DPI-virtualized (scaled) coordinates while the swapchain's
		// actual backbuffer and Interception's absolute HID coordinate space
		// are always in real physical pixels. That mismatch alone produces a
		// constant proportional cursor offset, which matches what was observed
		// in-game (cursor consistently offset down/right from where it should be).
		SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	}

	// Re-resolves the live LGUIScreenSpaceInteraction actor and its current
	// world-space distance from the local player camera every frame. The
	// distance is used to scale cursor sensitivity so that when the Lua
	// UI-fix script pushes the panel further away (or pulls it closer) via
	// UIWorldPlaneDistance, the cursor speed/offset compensates instead of
	// staying fixed and drifting off the panel.
	void update_ui_plane_tracking() {
		using namespace ui_tracking;

		auto actor = find_screenspace_actor();
		m_ui_actor_found = actor != nullptr;

		if (actor == nullptr) {
			m_ui_plane_distance_scale = 1.0f;
			return;
		}

		const auto actor_pos = get_actor_location(actor);

		if (!actor_pos.has_value()) {
			m_ui_plane_distance_scale = 1.0f;
			return;
		}

		m_ui_actor_location = *actor_pos;

		const auto pawn = API::get()->get_local_pawn(0);
		const auto pawn_pos = get_actor_location(pawn);

		if (!pawn_pos.has_value()) {
			m_ui_plane_distance_scale = 1.0f;
			return;
		}

		const auto dx = m_ui_actor_location.x - pawn_pos->x;
		const auto dy = m_ui_actor_location.y - pawn_pos->y;
		const auto dz = m_ui_actor_location.z - pawn_pos->z;
		const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);

		m_ui_plane_distance = distance;

		if (distance <= 1.0f) {
			m_ui_plane_distance_scale = 1.0f;
			return;
		}

		// Reference distance chosen to match the Lua script's default
		// UIWorldPlaneDistance (100 units) used when the panel is at "normal"
		// readable range. Sensitivity is scaled proportionally to how far the
		// panel currently sits relative to that baseline so a fixed physical
		// stick deflection continues to traverse the same fraction of the
		// panel regardless of where the Lua script has currently placed it.
		constexpr float kReferenceDistance = 100.0f;
		m_ui_plane_distance_scale = std::clamp(distance / kReferenceDistance, 0.1f, 10.0f);
	}

	~GamepadCursorPlugin() override = default;

	void on_present() override {
		std::scoped_lock _{m_imgui_mutex};

		if (!m_initialized) {
			if (!initialize_imgui()) {
				return;
			}
		}

		// UEVR's own gamepad polling (src/mods/VR.cpp) unconditionally re-enables
		// ImGuiConfigFlags_NavEnableGamepad every time it sees controller input, which
		// causes ImGui to flip-flop between "gamepad selected" widget highlighting and
		// "mouse hovered" highlighting each frame (the flicker between gamepad/mouse
		// mode). Since we drive a real emulated mouse cursor here, force gamepad nav
		// off every frame the UEVR menu is open so mouse hover/click is the sole
		// interaction model. Leave it alone otherwise so normal gameplay gamepad nav
		// (if any) isn't affected.
		if (m_enabled && API::get()->param()->functions->is_drawing_ui()) {
			ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
		}

		const auto renderer_data = API::get()->param()->renderer;

		if (!API::get()->param()->vr->is_hmd_active()) {
			if (!m_was_rendering_desktop) {
				m_was_rendering_desktop = true;
				on_device_reset();
				return;
			}

			m_was_rendering_desktop = true;

			if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
				ImGui_ImplWin32_NewFrame();
				ImGui_ImplDX11_NewFrame();
				ImGui::NewFrame();
				internal_frame();
				ImGui::EndFrame();
				ImGui::Render();
				g_d3d11.render_imgui();
			} else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
				auto command_queue = (ID3D12CommandQueue*)renderer_data->command_queue;

				if (command_queue == nullptr) {
					return;
				}

				ImGui_ImplWin32_NewFrame();
				ImGui_ImplDX12_NewFrame();
				ImGui::NewFrame();
				internal_frame();
				ImGui::EndFrame();
				ImGui::Render();
				g_d3d12.render_imgui();
			}
		}
	}

	void on_device_reset() override {
		std::scoped_lock _{m_imgui_mutex};

		const auto renderer_data = API::get()->param()->renderer;

		if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
			ImGui_ImplDX11_Shutdown();
			g_d3d11 = {};
		}

		if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
			g_d3d12.reset();
			ImGui_ImplDX12_Shutdown();
			g_d3d12 = {};
		}

		m_initialized = false;
	}

	void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {
		const auto vr_active = API::get()->param()->vr->is_hmd_active();

		if (!m_initialized || !vr_active) {
			return;
		}

		if (m_was_rendering_desktop) {
			m_was_rendering_desktop = false;
			on_device_reset();
			return;
		}

		std::scoped_lock _{m_imgui_mutex};

		ImGui_ImplWin32_NewFrame();
		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		internal_frame();
		ImGui::EndFrame();
		ImGui::Render();
		g_d3d11.render_imgui_vr(context, rtv);
	}

	void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
		const auto vr_active = API::get()->param()->vr->is_hmd_active();

		if (!m_initialized || !vr_active) {
			return;
		}

		if (m_was_rendering_desktop) {
			m_was_rendering_desktop = false;
			on_device_reset();
			return;
		}

		std::scoped_lock _{m_imgui_mutex};

		ImGui_ImplWin32_NewFrame();
		ImGui_ImplDX12_NewFrame();
		ImGui::NewFrame();
		internal_frame();
		ImGui::EndFrame();
		ImGui::Render();
		g_d3d12.render_imgui_vr(command_list, rtv);
	}

	bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override {
		ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

		return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
	}

	// This plugin deliberately does NOT emulate a mouse cursor. The game's
	// native UI already highlights/navigates purely from camera/HMD
	// orientation (confirmed: rotating the HMD alone moves the highlighted
	// item, with no cursor involved at all), which means its selection state
	// is driven by the engine's own Slate gamepad-navigation system, not a
	// 2D mouse position. Injecting a mouse cursor on top of that only adds a
	// second, disconnected interaction layer that never lines up with the
	// VR-space panel, and continuous absolute-mouse injection was also
	// fighting the right stick's camera-look input. So we leave XInput
	// state completely untouched (native stick/dpad nav keeps working) and
	// only inject native key presses for confirm/back, since that's what
	// Slate gamepad-nav-driven UIs actually listen for.
	void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override {
		if (*retval != ERROR_SUCCESS || state == nullptr) {
			return;
		}

		// Only drive input emulation from the lowest/first active user index to
		// avoid multiple controllers fighting over the emulated keys.
		if (m_locked_user_index == -1) {
			m_locked_user_index = (int)user_index;
		}

		if ((int)user_index != m_locked_user_index) {
			return;
		}

		handle_toggle(state->Gamepad.wButtons);

		update_ui_plane_tracking();

		// NOTE: is_drawing_ui() only reflects whether UEVR's OWN overlay menu
		// is open, not the game's native UI (e.g. the in-game Terminal/character
		// menu). Gating on it meant this never worked while only the game's own
		// menus were open, which is the actual target for this plugin. Rely
		// solely on the explicit Back/Select toggle (m_enabled) instead.
		if (!m_enabled) {
			return;
		}

		handle_buttons(state->Gamepad.wButtons);
	}

private:
	// The game appears to filter/ignore mouse motion and clicks injected via
	// SendInput (they are rejected as "synthetic" input at the raw-input/HID
	// level), even though it happily reads the real physical mouse and its
	// own native XInput-based nav-highlight system. The Interception driver
	// injects strokes at the kernel/HID-filter level, so the game sees them
	bool initialize_imgui() {
		if (m_initialized) {
			return true;
		}

		std::scoped_lock _{m_imgui_mutex};

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		static const auto imgui_ini = API::get()->get_persistent_dir(L"imgui_gamepad_cursor_plugin.ini").string();
		ImGui::GetIO().IniFilename = imgui_ini.c_str();

		const auto renderer_data = API::get()->param()->renderer;

		DXGI_SWAP_CHAIN_DESC swap_desc{};
		auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;
		swapchain->GetDesc(&swap_desc);

		m_wnd = swap_desc.OutputWindow;
		m_backbuffer_w = (int)swap_desc.BufferDesc.Width;
		m_backbuffer_h = (int)swap_desc.BufferDesc.Height;

		if (!ImGui_ImplWin32_Init(m_wnd)) {
			return false;
		}

		if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
			if (!g_d3d11.initialize()) {
				return false;
			}
		} else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
			if (!g_d3d12.initialize()) {
				return false;
			}
		}

		m_initialized = true;
		return true;
	}

	void internal_frame() {
		if (ImGui::Begin("Gamepad Cursor Plugin")) {
			ImGui::TextWrapped(
				"Cursor emulation has been removed: this game's VR UI panel position is purely "
				"cosmetic (repositioned by the Lua UI-fix script), while widget focus/hit-testing "
				"is still computed against the flat 2D projection. Enabling this now automatically "
				"forces VR_2DScreenMode ON while active so gamepad nav lines up correctly, and "
				"restores true VR view the moment it's disabled.");
			ImGui::Separator();

			ImGui::Checkbox("Enabled (Back/Select toggles in-game)", &m_enabled);
			ImGui::TextWrapped("When enabled: VR_2DScreenMode is forced on (menu navigation now matches 2D-mode behavior) and A/B/X/Y are additionally sent as native key presses. The gamepad stick/dpad input itself is left completely untouched.");
			ImGui::Separator();

			ImGui::Text("Face button -> VK code (0 = disabled)");
			int confirm_vk = m_confirm_vk;
			int back_vk = m_back_vk;
			int x_vk = m_x_button_vk;
			int y_vk = m_y_button_vk;
			if (ImGui::InputInt("A (confirm) -> VK code", &confirm_vk)) {
				m_confirm_vk = (WORD)std::clamp(confirm_vk, 0, 0xFF);
			}
			if (ImGui::InputInt("B (back) -> VK code", &back_vk)) {
				m_back_vk = (WORD)std::clamp(back_vk, 0, 0xFF);
			}
			if (ImGui::InputInt("X button -> VK code", &x_vk)) {
				m_x_button_vk = (WORD)std::clamp(x_vk, 0, 0xFF);
			}
			if (ImGui::InputInt("Y button -> VK code", &y_vk)) {
				m_y_button_vk = (WORD)std::clamp(y_vk, 0, 0xFF);
			}
			int start_vk = m_start_button_vk;
			if (ImGui::InputInt("Start (three lines) -> VK code", &start_vk)) {
				m_start_button_vk = (WORD)std::clamp(start_vk, 0, 0xFF);
			}
			ImGui::TextWrapped(
				"Enter the decimal Windows virtual-key code the UI expects for that button "
				"(e.g. 13 = Enter, 27 = Escape, 70 = 'F', 9 = Tab).");

			ImGui::Text("Locked user index: %d", m_locked_user_index);
			ImGui::Separator();

			if (m_ui_actor_found) {
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "UI plane actor (LGUIScreenSpaceInteraction): FOUND");
				ImGui::Text("Actor location: (%.1f, %.1f, %.1f)", m_ui_actor_location.x, m_ui_actor_location.y, m_ui_actor_location.z);
				ImGui::Text("Distance from camera: %.1f", m_ui_plane_distance);
			} else {
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "UI plane actor (LGUIScreenSpaceInteraction): NOT FOUND");
			}
			ImGui::Separator();
			ImGui::TextWrapped(
				"Left stick / D-Pad drive the game's own native UI navigation directly (untouched). "
				"A/B/X/Y additionally send configurable key presses. Back/Select toggles this on/off.");
		}

		ImGui::End();
	}

	static float apply_deadzone(float value, float deadzone) {
		if (std::abs(value) < deadzone) {
			return 0.0f;
		}

		const auto sign = value > 0.0f ? 1.0f : -1.0f;
		return sign * ((std::abs(value) - deadzone) / (1.0f - deadzone));
	}

	// The VR-space UI panel (repositioned by WutheringWaves_UIFix.lua) is
	// purely cosmetic: the game's actual widget focus/hit-testing math is
	// still computed against the flat, un-rotated 2D projection basis that
	// VR_2DScreenMode enables (see is_using_2d_screen() usage in
	// FFakeStereoRenderingHook.cpp / D3D11Component.cpp / D3D12Component.cpp).
	// That's why 2D-screen-mode navigation lines up perfectly while VR-mode
	// navigation drifts (D-pad highlight moves a purely visual cursor while
	// A/B act on a stale/different real focus target). Since flipping
	// VR_2DScreenMode is just a rendering/projection toggle (no game-state
	// side effects), we reconcile the two by automatically switching into
	// 2D screen mode for the duration that UI-nav emulation is enabled, and
	// restoring true VR head-tracking the moment it's toggled back off.
	static constexpr auto kTwoDScreenModeKey = "VR_2DScreenMode";

	void handle_toggle(WORD buttons) {
		const auto back_down = (buttons & XINPUT_GAMEPAD_BACK) != 0;

		if (back_down && !m_prev_back_down) {
			m_enabled = !m_enabled;
			API::get()->log_info("[GamepadCursorPlugin] Native UI-nav key emulation %s", m_enabled ? "ENABLED" : "DISABLED");

			if (m_enabled) {
				m_prev_2d_screen_mode = API::VR::get_mod_value<bool>(kTwoDScreenModeKey);
				API::VR::set_mod_value<bool>(kTwoDScreenModeKey, true);
				API::get()->log_info("[GamepadCursorPlugin] Forcing VR_2DScreenMode ON for menu navigation.");
			} else {
				API::VR::set_mod_value<bool>(kTwoDScreenModeKey, m_prev_2d_screen_mode);
				API::get()->log_info("[GamepadCursorPlugin] Restoring VR_2DScreenMode to %s.", m_prev_2d_screen_mode ? "true" : "false");
			}
		}

		m_prev_back_down = back_down;
	}

	// Sends A/B as native keyboard confirm/back input for the game's own
	// gamepad-driven UI navigation (which already highlights items purely
	// from camera/HMD orientation, with no cursor involved at all). We do
	// NOT emulate a mouse cursor or clicks: that was a second, disconnected
	// interaction layer that never lined up with the VR-space UI panel, and
	// continuously injecting absolute mouse strokes was also fighting the
	// right stick's camera-look input. X/Y are kept as configurable direct
	// key presses for secondary UI actions.
	void handle_buttons(WORD buttons) {
		const auto a_down = (buttons & XINPUT_GAMEPAD_A) != 0;

		if (a_down && !m_prev_a_down && m_confirm_vk != 0) {
			send_key(m_confirm_vk);
		}

		m_prev_a_down = a_down;

		const auto b_down = (buttons & XINPUT_GAMEPAD_B) != 0;

		if (b_down && !m_prev_b_down && m_back_vk != 0) {
			send_key(m_back_vk);
		}

		m_prev_b_down = b_down;

		const auto x_down = (buttons & XINPUT_GAMEPAD_X) != 0;

		if (x_down && !m_prev_x_down && m_x_button_vk != 0) {
			send_key(m_x_button_vk);
		}

		m_prev_x_down = x_down;

		const auto y_down = (buttons & XINPUT_GAMEPAD_Y) != 0;

		if (y_down && !m_prev_y_down && m_y_button_vk != 0) {
			send_key(m_y_button_vk);
		}

		m_prev_y_down = y_down;

		// Start (the "three lines"/hamburger button, opposite Back/Select) is
		// read directly by the game in 2D mode but appears to do nothing while
		// VR_2DScreenMode is forced on for menu nav, so forward it as a key
		// press too (defaults to Escape, a common pause/menu-open binding).
		const auto start_down = (buttons & XINPUT_GAMEPAD_START) != 0;

		if (start_down && !m_prev_start_down && m_start_button_vk != 0) {
			send_key(m_start_button_vk);
		}

		m_prev_start_down = start_down;
	}

	void send_key(WORD vk) {
		INPUT inputs[2]{};

		inputs[0].type = INPUT_KEYBOARD;
		inputs[0].ki.wVk = vk;

		inputs[1].type = INPUT_KEYBOARD;
		inputs[1].ki.wVk = vk;
		inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

		SendInput(2, inputs, sizeof(INPUT));
	}

	enum class BackMode {
		EscapeKey,
		ClickCloseButton
	};

	HWND m_wnd{};
	int m_backbuffer_w{0};
	int m_backbuffer_h{0};
	bool m_initialized{false};
	bool m_was_rendering_desktop{false};
	std::recursive_mutex m_imgui_mutex{};

	bool m_enabled{true};
	bool m_prev_2d_screen_mode{false};
	bool m_prev_back_down{false};
	bool m_prev_a_down{false};
	bool m_prev_b_down{false};
	bool m_prev_x_down{false};
	bool m_prev_y_down{false};
	bool m_prev_start_down{false};

	int m_locked_user_index{-1};

	std::chrono::steady_clock::time_point m_last_tick{std::chrono::steady_clock::now()};

	// Native gamepad-nav key emulation: the game's own Slate/UI navigation
	// already listens for these, so we just forward them as key presses
	// rather than emulating a mouse cursor/clicks.
	WORD m_confirm_vk{VK_RETURN};
	WORD m_back_vk{VK_ESCAPE};

	// X/Y button emulation: many UI elements require these face buttons
	// directly (e.g. secondary actions); default to common keys but these
	// are user-tunable since the correct key is UI/context dependent.
	WORD m_x_button_vk{'F'};
	WORD m_y_button_vk{VK_TAB};
	WORD m_start_button_vk{VK_ESCAPE};

	// Live UI-plane tracking (see update_ui_plane_tracking()). Reflects the
	// LGUIScreenSpaceInteraction actor that WutheringWaves_UIFix.lua
	// dynamically repositions/rescales at runtime.
	bool m_ui_actor_found{false};
	ui_tracking::Vec3 m_ui_actor_location{};
	float m_ui_plane_distance{0.0f};
	float m_ui_plane_distance_scale{1.0f};
};

std::unique_ptr<GamepadCursorPlugin> g_plugin{new GamepadCursorPlugin()};
