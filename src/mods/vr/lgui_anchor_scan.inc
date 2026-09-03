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
				L"LGUIRenderer", L"LGUI_Render", L"RenderLGUI", L"LGUIHudRender", L"LGUIScreenSpace",
				L"LGUICanvas", L"ScreenSpaceOverlay", L"LGUI_ScreenSpace", L"LGUIRenderTarget", L"LGUIWorldSpaceRender",
				L"LGUIPostProcess", L"LGUIHelperLine", L"LGUIViewExtension",
			};
			const std::vector<std::string> anchors = {
				"LGUIRenderer", "LGUI_Render", "RenderLGUI", "LGUIHudRender", "LGUIScreenSpace",
				"ScreenSpaceOverlay", "LGUIRenderTarget", "FLGUIRenderer", "LGUIPostProcess", "LGUIViewExtension",
				"LGUIRenderPass", "LGUIMeshRender", "LGUIRenderThread",
			};

			auto report = [&](const std::string& tag, uintptr_t str_addr) {
				const auto refs = utility::scan_displacement_references(exe, str_addr);
				std::string fns{};
				size_t shown = 0;

				for (const auto ref : refs) {
					const auto fn = utility::find_function_start_with_call(ref);
					fns += fmt::format("ref={:x}", ref - base);
					if (fn) fns += fmt::format("(fn={:x})", *fn - base);
					fns += ",";
					if (++shown >= 8) { fns += "..."; break; }
				}

				SPDLOG_INFO("[LGUI_ANCHOR] {} @rva {:x} refs={} [{}]", tag, str_addr - base, refs.size(), fns);
			};

			for (const auto& a : wanchors) {
				const auto hits = utility::scan_strings(exe, a, false);
				SPDLOG_INFO("[LGUI_ANCHOR] L\"{}\" hits={}", utility::narrow(a), hits.size());
				size_t n = 0;
				for (const auto h : hits) {
					std::wstring full{};
					for (auto p = (const wchar_t*)h; !IsBadReadPtr(p, 2) && *p != 0 && full.size() < 96; ++p) full += *p;
					report(fmt::format("  L\"{}\"", utility::narrow(full)), h);
					if (++n >= 6) break;
				}
			}

			for (const auto& a : anchors) {
				const auto hits = utility::scan_strings(exe, a, false);
				SPDLOG_INFO("[LGUI_ANCHOR] \"{}\" hits={}", a, hits.size());
				size_t n = 0;
				for (const auto h : hits) {
					std::string full{};
					for (auto p = (const char*)h; !IsBadReadPtr(p, 1) && *p != 0 && full.size() < 96; ++p) full += *p;
					report(fmt::format("  \"{}\"", full), h);
					if (++n >= 6) break;
				}
			}

			SPDLOG_INFO("[LGUI_ANCHOR] scan complete");
		} catch (...) {
			SPDLOG_ERROR("[LGUI_ANCHOR] exception during scan");
		}
	}).detach();
}
