// AnnotateUEVRTrace.java
//
// Java port of annotate_uevr_trace.py, for environments where Ghidra's Jython scripting isn't
// available/configured (no pyghidra). Functionally identical: parses the UEVR offline trace
// (trace.bin + manifest.json, produced by src/mods/vr/TraceWriter.cpp while the game runs with
// "DIAG: Enable Offline Trace Writer" enabled in the UEVR debug UI) and annotates the currently
// open Ghidra program with bookmarks + plate comments at each resolved RVA.
//
// SETUP:
//   1. Copy this file into a folder Ghidra's Script Manager knows about (Window > Script Manager
//      > Script Directories, add this repo's tools/ghidra folder), OR copy it directly into your
//      user Ghidra scripts folder (typically %USERPROFILE%\ghidra_scripts).
//   2. Copy trace.bin and manifest.json (from <game>/uevr_trace/) next to this .java file, or
//      edit TRACE_DIR_OVERRIDE below to point at wherever you copied them.
//   3. Open the game's main executable in Ghidra and let auto-analysis finish.
//   4. Select this script in the Script Manager and click Run. Check the Console for progress.
//   5. Open Window > Bookmarks, filter by category "UEVR Trace" to jump to each annotated address.
//
// See tools/ghidra/README.md for the full trace format description and the EventKind table - this
// file mirrors that format exactly and must be kept in sync with
// src/mods/vr/TraceWriter.hpp if the record layout or EventKind enum ever changes.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Bookmark;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class AnnotateUEVRTrace extends GhidraScript {

	// Leave null to default to the directory this script is running from. Set to an absolute path
	// string (e.g. "C:\\Users\\aaron\\Desktop\\UEVR\\tools\\ghidra") to override.
	private static final String TRACE_DIR_OVERRIDE = null;

	// Must match sizeof(uevr_trace::TraceRecord) in src/mods/vr/TraceWriter.hpp:
	//   uint64 timestamp_qpc, uint32 frame_count, uint32 event_kind, uint32 module_index,
	//   uint32 _pad, uint64 rva, uint64 aux_a, uint64 aux_b
	private static final int RECORD_SIZE = 48;

	private static final String BOOKMARK_CATEGORY = "UEVR Trace";

	// These event kinds are recorded with the module base address (not a real call-site/RVA) on
	// the C++ side (see FFakeStereoRenderingHook.cpp: uevr_trace::TraceWriter::get().record(...,
	// (uintptr_t)utility::get_executable(), ...)) - they carry discovered struct-offset VALUES, not
	// a meaningful code location. Bookmarking them would just point at the PE/DOS header, so treat
	// them as data-only: print the decoded payload to the console instead of annotating the listing.
	private static final Set<Integer> DATA_ONLY_EVENT_KINDS = new HashSet<>();
	static {
		DATA_ONLY_EVENT_KINDS.add(4); // StereoPassOffsetResolved
		DATA_ONLY_EVENT_KINDS.add(5); // InitOptionsOffsetResolved
	}

	private static final Map<Integer, String> EVENT_KIND_NAMES = new HashMap<>();
	static {
		EVENT_KIND_NAMES.put(0, "Unknown");
		EVENT_KIND_NAMES.put(1, "SceneViewCtorCallSite");
		EVENT_KIND_NAMES.put(2, "LguiVtableSwap");
		EVENT_KIND_NAMES.put(3, "NsfEyeClassification");
		EVENT_KIND_NAMES.put(4, "StereoPassOffsetResolved");
		EVENT_KIND_NAMES.put(5, "InitOptionsOffsetResolved");
	}

	private static final class TraceRecord {
		long timestampQpc;
		int frameCount;
		int eventKind;
		int moduleIndex;
		long rva;
		long auxA;
		long auxB;
	}

	private static final class ModuleInfo {
		String name = "<unknown>";
		long base;
		long size;
	}

	@Override
	protected void run() throws Exception {
		final generic.jar.ResourceFile scriptSourceFile = getSourceFile();
		final File scriptDir = (scriptSourceFile != null) ? scriptSourceFile.getParentFile().getFile(false) : new File(".");
		final File traceDir = (TRACE_DIR_OVERRIDE != null) ? new File(TRACE_DIR_OVERRIDE) : scriptDir;
		final File traceBinFile = new File(traceDir, "trace.bin");
		final File manifestFile = new File(traceDir, "manifest.json");

		if (!traceBinFile.isFile() || !manifestFile.isFile()) {
			println("[AnnotateUEVRTrace] trace.bin/manifest.json not found in: " + traceDir.getAbsolutePath());
			println("[AnnotateUEVRTrace] Copy the uevr_trace output folder's contents here, or set TRACE_DIR_OVERRIDE.");
			return;
		}

		final List<ModuleInfo> modules = loadManifest(manifestFile);
		final List<TraceRecord> records = loadRecords(traceBinFile);

		println("[AnnotateUEVRTrace] Loaded " + modules.size() + " modules and " + records.size() + " trace records.");

		final Address imageBase = currentProgram.getImageBase();

		int annotated = 0;
		int skipped = 0;
		int dataOnly = 0;
		final Set<String> seen = new HashSet<>();

		for (TraceRecord record : records) {
			if (record.moduleIndex < 0 || record.moduleIndex >= modules.size()) {
				skipped++;
				continue;
			}

			final String dedupeKey = record.moduleIndex + ":" + record.rva + ":" + record.eventKind;
			if (!seen.add(dedupeKey)) {
				continue;
			}

			if (DATA_ONLY_EVENT_KINDS.contains(record.eventKind)) {
				println("[AnnotateUEVRTrace] (data) " + describeEvent(record));
				dataOnly++;
				continue;
			}

			Address address;
			try {
				address = imageBase.add(record.rva);
			} catch (Exception e) {
				println("[AnnotateUEVRTrace] Failed to resolve address for rva=0x" + Long.toHexString(record.rva) + ": " + e);
				skipped++;
				continue;
			}

			final String description = describeEvent(record);

			try {
				createBookmark(address, BOOKMARK_CATEGORY, description);
			} catch (Exception e) {
				println("[AnnotateUEVRTrace] Failed to bookmark " + address + ": " + e);
			}

			try {
				final String existing = getPlateComment(address);
				final String newComment = (existing == null || existing.isEmpty())
					? description
					: existing + "\n" + description;
				setPlateComment(address, newComment);
			} catch (Exception e) {
				println("[AnnotateUEVRTrace] Failed to set plate comment at " + address + ": " + e);
			}

			annotated++;
		}

		println("[AnnotateUEVRTrace] Done. Annotated " + annotated + " unique addresses, printed " + dataOnly
			+ " data-only records, skipped " + skipped + " records (module=" + (modules.isEmpty() ? "<none>" : modules.get(0).name) + ").");
	}

	private String describeEvent(TraceRecord record) {
		final String name = EVENT_KIND_NAMES.getOrDefault(record.eventKind, "Unknown(" + record.eventKind + ")");

		if (record.eventKind == 4) { // StereoPassOffsetResolved
			final long left = (record.auxB >>> 32) & 0xFFFFFFFFL;
			final long right = record.auxB & 0xFFFFFFFFL;
			return String.format("%s: live_stereo_pass_offset=0x%x left=%d right=%d", name, record.auxA, left, right);
		}

		if (record.eventKind == 5) { // InitOptionsOffsetResolved
			final long familyOff = (record.auxA >>> 32) & 0xFFFFFFFFL;
			final long stateOff = record.auxA & 0xFFFFFFFFL;
			final long stereoOff = record.auxB;
			return String.format("%s: family_off=0x%x state_off=0x%x stereo_pass_off=0x%x", name, familyOff, stateOff, stereoOff);
		}

		return String.format("%s: aux_a=0x%x aux_b=0x%x", name, record.auxA, record.auxB);
	}

	private List<TraceRecord> loadRecords(File path) throws IOException {
		final List<TraceRecord> records = new ArrayList<>();

		try (RandomAccessFile raf = new RandomAccessFile(path, "r");
			 FileChannel channel = raf.getChannel()) {
			final ByteBuffer buffer = ByteBuffer.allocate((int) channel.size());
			buffer.order(ByteOrder.LITTLE_ENDIAN);
			channel.read(buffer);
			buffer.flip();

			while (buffer.remaining() >= RECORD_SIZE) {
				final TraceRecord rec = new TraceRecord();
				rec.timestampQpc = buffer.getLong();
				rec.frameCount = buffer.getInt();
				rec.eventKind = buffer.getInt();
				rec.moduleIndex = buffer.getInt();
				buffer.getInt(); // _pad
				rec.rva = buffer.getLong();
				rec.auxA = buffer.getLong();
				rec.auxB = buffer.getLong();
				records.add(rec);
			}
		}

		return records;
	}

	// Minimal hand-rolled JSON parsing for the one known manifest shape (format_version,
	// record_size_bytes, modules[{name,base,size}]) - avoids pulling in an external JSON library
	// just for this. Deliberately tolerant of whitespace/ordering but not a general-purpose parser.
	private List<ModuleInfo> loadManifest(File path) throws IOException {
		final List<ModuleInfo> modules = new ArrayList<>();
		final StringBuilder sb = new StringBuilder();

		try (FileInputStream fis = new FileInputStream(path)) {
			final byte[] buf = new byte[4096];
			int n;
			while ((n = fis.read(buf)) > 0) {
				sb.append(new String(buf, 0, n, "UTF-8"));
			}
		}

		final String json = sb.toString();
		final int modulesIdx = json.indexOf("\"modules\"");
		if (modulesIdx < 0) {
			return modules;
		}

		final int arrStart = json.indexOf('[', modulesIdx);
		final int arrEnd = findMatchingBracket(json, arrStart);
		if (arrStart < 0 || arrEnd < 0) {
			return modules;
		}

		int i = arrStart + 1;
		while (i < arrEnd) {
			final int objStart = json.indexOf('{', i);
			if (objStart < 0 || objStart > arrEnd) {
				break;
			}
			final int objEnd = findMatchingBrace(json, objStart);
			if (objEnd < 0) {
				break;
			}

			final String obj = json.substring(objStart, objEnd + 1);
			final ModuleInfo info = new ModuleInfo();
			info.name = extractJsonString(obj, "name");
			info.base = extractJsonLong(obj, "base");
			info.size = extractJsonLong(obj, "size");
			modules.add(info);

			i = objEnd + 1;
		}

		return modules;
	}

	private int findMatchingBracket(String s, int openIdx) {
		return findMatching(s, openIdx, '[', ']');
	}

	private int findMatchingBrace(String s, int openIdx) {
		return findMatching(s, openIdx, '{', '}');
	}

	private int findMatching(String s, int openIdx, char open, char close) {
		if (openIdx < 0) {
			return -1;
		}
		int depth = 0;
		for (int i = openIdx; i < s.length(); i++) {
			final char c = s.charAt(i);
			if (c == open) {
				depth++;
			} else if (c == close) {
				depth--;
				if (depth == 0) {
					return i;
				}
			}
		}
		return -1;
	}

	private String extractJsonString(String obj, String key) {
		final String needle = "\"" + key + "\"";
		final int keyIdx = obj.indexOf(needle);
		if (keyIdx < 0) {
			return "<unknown>";
		}
		final int colonIdx = obj.indexOf(':', keyIdx);
		final int firstQuote = obj.indexOf('"', colonIdx + 1);
		if (firstQuote < 0) {
			return "<unknown>";
		}
		final StringBuilder out = new StringBuilder();
		int i = firstQuote + 1;
		while (i < obj.length() && obj.charAt(i) != '"') {
			char c = obj.charAt(i);
			if (c == '\\' && i + 1 < obj.length()) {
				i++;
				c = obj.charAt(i);
			}
			out.append(c);
			i++;
		}
		return out.toString();
	}

	private long extractJsonLong(String obj, String key) {
		final String needle = "\"" + key + "\"";
		final int keyIdx = obj.indexOf(needle);
		if (keyIdx < 0) {
			return 0;
		}
		final int colonIdx = obj.indexOf(':', keyIdx);
		int i = colonIdx + 1;
		while (i < obj.length() && Character.isWhitespace(obj.charAt(i))) {
			i++;
		}
		final int start = i;
		while (i < obj.length() && (Character.isDigit(obj.charAt(i)) || obj.charAt(i) == '-')) {
			i++;
		}
		if (start == i) {
			return 0;
		}
		try {
			return Long.parseLong(obj.substring(start, i));
		} catch (NumberFormatException e) {
			return 0;
		}
	}
}
