#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <utility/Module.hpp>

#include "../../Framework.hpp"

#include "TraceWriter.hpp"

namespace uevr_trace {

TraceWriter& TraceWriter::get() {
	static TraceWriter instance{};
	return instance;
}

TraceWriter::TraceWriter() = default;

TraceWriter::~TraceWriter() {
	try {
		finalize();
	} catch (...) {
		// Never let a destructor throw during process teardown.
	}
}

void TraceWriter::ensure_paths_initialized() {
	if (m_paths_initialized) {
		return;
	}

	const auto dir = Framework::get_persistent_dir("uevr_trace");

	std::error_code ec{};
	std::filesystem::create_directories(dir, ec);

	m_trace_path = dir / "trace.bin";
	m_manifest_path = dir / "manifest.json";
	m_paths_initialized = true;

	SPDLOG_INFO("[TraceWriter] trace_path={} manifest_path={}", m_trace_path.string(), m_manifest_path.string());
}

void TraceWriter::set_enabled(bool enabled) {
	const auto was_enabled = m_enabled.exchange(enabled, std::memory_order_relaxed);

	if (enabled && !was_enabled) {
		std::scoped_lock _{m_mutex};
		ensure_paths_initialized();

		// Starting a fresh session: truncate any previous trace so the Ghidra script doesn't mix
		// records from different game sessions/builds together.
		std::ofstream truncate{m_trace_path, std::ios::binary | std::ios::trunc};
		truncate.close();

		SPDLOG_INFO("[TraceWriter] Tracing ENABLED - writing to {}", m_trace_path.string());
	} else if (!enabled && was_enabled) {
		SPDLOG_INFO("[TraceWriter] Tracing DISABLED, flushing remaining records");
		flush();
	}
}

uint32_t TraceWriter::register_module_for_address(uintptr_t address) {
	std::scoped_lock _{m_mutex};

	const auto module_opt = utility::get_module_within(address);
	const uintptr_t base = module_opt.has_value() ? (uintptr_t)*module_opt : 0;

	if (auto it = m_module_base_to_index.find(base); it != m_module_base_to_index.end()) {
		return it->second;
	}

	ModuleInfo info{};
	info.base = base;

	if (module_opt.has_value()) {
		if (const auto path = utility::get_module_path(*module_opt); path.has_value()) {
			info.name = *path;
		}

		if (const auto size = utility::get_module_size(*module_opt); size.has_value()) {
			info.size = *size;
		}
	}

	if (info.name.empty()) {
		info.name = "<unknown>";
	}

	const auto index = (uint32_t)m_modules.size();
	m_modules.push_back(std::move(info));
	m_module_base_to_index[base] = index;

	return index;
}

void TraceWriter::record(EventKind kind, uintptr_t address, uint64_t aux_a, uint64_t aux_b, bool force_flush) {
	if (!is_enabled()) {
		return;
	}

	const auto module_index = register_module_for_address(address);

	uintptr_t rva = address;

	{
		std::scoped_lock _{m_mutex};
		if (module_index < m_modules.size() && m_modules[module_index].base != 0) {
			rva = address - m_modules[module_index].base;
		}
	}

	LARGE_INTEGER qpc{};
	QueryPerformanceCounter(&qpc);

	TraceRecord rec{};
	rec.timestamp_qpc = (uint64_t)qpc.QuadPart;
	rec.event_kind = (uint32_t)kind;
	rec.module_index = module_index;
	rec.rva = (uint64_t)rva;
	rec.aux_a = aux_a;
	rec.aux_b = aux_b;

	bool should_flush = false;

	{
		std::scoped_lock _{m_mutex};
		m_buffer.push_back(rec);
		should_flush = force_flush || m_buffer.size() >= FLUSH_THRESHOLD_RECORDS;
	}

	if (should_flush) {
		flush();
	}
}

void TraceWriter::write_manifest_locked() {
	nlohmann::json manifest{};
	manifest["format_version"] = 1;
	manifest["record_size_bytes"] = sizeof(TraceRecord);
	manifest["modules"] = nlohmann::json::array();

	for (const auto& mod : m_modules) {
		manifest["modules"].push_back({
			{"name", mod.name},
			{"base", mod.base},
			{"size", mod.size},
		});
	}

	std::ofstream out{m_manifest_path, std::ios::trunc};
	if (out.is_open()) {
		out << manifest.dump(2);
	}
}

void TraceWriter::flush() {
	std::vector<TraceRecord> to_write{};

	{
		std::scoped_lock _{m_mutex};
		ensure_paths_initialized();

		if (m_buffer.empty()) {
			write_manifest_locked();
			return;
		}

		to_write.swap(m_buffer);
	}

	std::ofstream out{m_trace_path, std::ios::binary | std::ios::app};

	if (out.is_open()) {
		out.write((const char*)to_write.data(), (std::streamsize)(to_write.size() * sizeof(TraceRecord)));
	} else {
		SPDLOG_WARN("[TraceWriter] Failed to open trace file for append: {}", m_trace_path.string());
	}

	{
		std::scoped_lock _{m_mutex};
		write_manifest_locked();
	}
}

void TraceWriter::finalize() {
	flush();
}

} // namespace uevr_trace
