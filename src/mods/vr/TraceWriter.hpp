#pragma once

// TraceWriter: lightweight offline trace recorder.
//
// Purpose: record RVA-based runtime events (call sites, vtable resolutions, stereo-pass
// classifications, view constructions, etc.) to a binary trace file on disk during gameplay,
// alongside a one-time JSON module manifest. The trace is later parsed entirely offline by a
// Ghidra Python script (see tools/ghidra/annotate_uevr_trace.py) that annotates the static binary
// (labels/comments/bookmarks) so the modified game's stereo/UI functions can be mapped without
// live debugging or sockets.
//
// Design notes:
//  - Records are fixed-width binary (not JSON) to keep the hot-path cost low: this is called from
//    inside render-thread hooks that already run hundreds/thousands of times per second.
//  - All addresses are stored as module-relative RVAs, never raw VAs, so the trace remains valid
//    across ASLR'd runs as long as the underlying binary/module build is unchanged.
//  - Callers are expected to dedupe at the call site (e.g. an std::unordered_set of "already seen"
//    keys) before calling record(), mirroring the existing first_time_from_this_retaddr /
//    s_seen_raw_stereo_pass_values patterns already used elsewhere in this codebase. TraceWriter
//    itself does not dedupe, it just appends.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace uevr_trace {

// Every event kind that can be recorded. Keep this append-only (never renumber existing values)
// so old trace files stay parseable by the Ghidra script after this enum grows.
enum class EventKind : uint32_t {
	Unknown = 0,
	SceneViewCtorCallSite = 1,   // a3 = stereo_pass (raw), a4 = view_rect width<<32|height
	LguiVtableSwap = 2,          // a3 = vtable_rva, a4 = first_vfunc_rva
	NsfEyeClassification = 3,    // a3 = true_index, a4 = is_confirmed_non_eye_pass (0/1)
	StereoPassOffsetResolved = 4,// a3 = live_stereo_pass_offset, a4 = (left<<32|right)
	InitOptionsOffsetResolved = 5, // a3 = family_off<<32|state_off, a4 = stereo_pass_off
};

// One fixed-width trace record. Keep this packed/stable - the Ghidra script parses this layout
// directly via struct.unpack. Never reorder or resize existing fields; only append new ones at
// the end along with a format_version bump in the manifest if that ever becomes necessary.
#pragma pack(push, 1)
struct TraceRecord {
	uint64_t timestamp_qpc{0};   // QueryPerformanceCounter ticks, for ordering/correlation only
	uint32_t frame_count{0};
	uint32_t event_kind{0};      // EventKind
	uint32_t module_index{0};    // index into the manifest's module list
	uint32_t _pad{0};            // keep 8-byte alignment for the following fields
	uint64_t rva{0};             // module-relative RVA this event concerns
	uint64_t aux_a{0};           // event-specific payload
	uint64_t aux_b{0};           // event-specific payload
};
#pragma pack(pop)

static_assert(sizeof(TraceRecord) == 48, "TraceRecord layout changed - update the Ghidra parser format string too");

// Describes one resolved module for the manifest (written once as JSON alongside the binary
// trace), so the offline Ghidra script can confirm it's analyzing the same build these RVAs were
// captured from.
struct ModuleInfo {
	std::string name;
	uintptr_t base{0};
	size_t size{0};
};

// Singleton trace writer. Thread-safe: record() may be called from the render thread or any hook
// thread; an internal mutex guards the buffer and the module table.
class TraceWriter {
public:
	static TraceWriter& get();

	// Enables/disables recording. While disabled, record() is a no-op (cheap atomic check).
	void set_enabled(bool enabled);
	bool is_enabled() const { return m_enabled.load(std::memory_order_relaxed); }

	// Registers (or looks up) a module by the address it contains, returning a manifest index to
	// pass as TraceRecord::module_index. Safe to call every time - internally cached.
	uint32_t register_module_for_address(uintptr_t address);

	// Appends one record to the in-memory buffer. Flushes automatically once the buffer grows
	// past a size threshold, or immediately if force_flush is true. No-op if tracing is disabled.
	// force_flush defaults to true: most record() call sites in this codebase are one-shot
	// "RESOLVED ..." events on rarely-hit code paths (not hot-path per-frame telemetry), so
	// waiting for FLUSH_THRESHOLD_RECORDS to fill up could mean a short play session (or one
	// where tracing is enabled mid-session) never gets flushed to disk at all. Pass false
	// explicitly at any future hot-path call site to keep this cheap/buffered there instead.
	void record(EventKind kind, uintptr_t address, uint64_t aux_a = 0, uint64_t aux_b = 0, bool force_flush = true);

	// Forces a flush of any buffered records to disk, and (re)writes the module manifest JSON.
	// Safe to call manually (e.g. from a "Flush Trace Now" debug UI button) or automatically.
	void flush();

	// Flushes remaining data and closes the trace file. Call on unload/shutdown if possible;
	// flush() alone is sufficient for crash-safety since it's append-only.
	void finalize();

	std::filesystem::path get_trace_file_path() const { return m_trace_path; }
	std::filesystem::path get_manifest_file_path() const { return m_manifest_path; }

private:
	TraceWriter();
	~TraceWriter();

	void ensure_paths_initialized();
	void write_manifest_locked();

	mutable std::mutex m_mutex{};
	std::atomic<bool> m_enabled{false};
	std::vector<TraceRecord> m_buffer{};
	std::vector<ModuleInfo> m_modules{};
	std::unordered_map<uintptr_t, uint32_t> m_module_base_to_index{};

	std::filesystem::path m_trace_path{};
	std::filesystem::path m_manifest_path{};
	bool m_paths_initialized{false};

	static constexpr size_t FLUSH_THRESHOLD_RECORDS = 2048;
};

} // namespace uevr_trace
