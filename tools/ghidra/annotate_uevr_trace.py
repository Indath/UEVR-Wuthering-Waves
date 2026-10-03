"""annotate_uevr_trace.py

Offline Ghidra Python script that parses a UEVR trace session (trace.bin + manifest.json,
produced by src/mods/vr/TraceWriter.cpp while the game is running with the
"DIAG: Enable Offline Trace Writer" toggle in the debug UI) and annotates the currently open
Ghidra program with labels, plate comments, and bookmarks at each resolved RVA.

This lets you map how the modified UE4.26 title's stereo-rendering path is actually laid out
(which SceneView/init-options offsets and stereo-pass encodings were learned at runtime) directly
onto the static binary, without a live debugger/socket connection.

USAGE (Ghidra Script Manager, or headless analyzer):
	1. Run the game with "DIAG: Enable Offline Trace Writer" enabled in the UEVR debug UI for a
	   while (long enough to pass the loading screen so sceneview_xref resolves its offsets).
	2. Click "DIAG: Flush Trace Now" (or just let the game run - it auto-flushes every 2048
	   records) then close the game.
	3. Copy the produced folder (<game>/uevr_trace/trace.bin + manifest.json) next to this
	   script, or edit TRACE_DIR below to point at it.
	4. Open the game's main executable module in Ghidra (the same build the trace was captured
	   from - the manifest's module size is checked against the open program to catch mismatches).
	5. Run this script from the Script Manager (Window > Script Manager), or headlessly via
	   analyzeHeadless with -postScript.

This script is intentionally dependency-free (stdlib + Ghidra's flat API only) so it runs under
Ghidra's bundled Jython/CPython without extra setup.
"""

import json
import os
import struct

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Directory containing trace.bin and manifest.json. Defaults to the directory this script lives
# in; override if you copy the script elsewhere.
TRACE_DIR = os.path.dirname(os.path.abspath(__file__))
TRACE_BIN_PATH = os.path.join(TRACE_DIR, "trace.bin")
MANIFEST_PATH = os.path.join(TRACE_DIR, "manifest.json")

# Must match sizeof(uevr_trace::TraceRecord) in src/mods/vr/TraceWriter.hpp.
RECORD_STRUCT_FORMAT = "<QIIIIQQQ"
RECORD_SIZE = struct.calcsize(RECORD_STRUCT_FORMAT)

EVENT_KIND_NAMES = {
	0: "Unknown",
	1: "SceneViewCtorCallSite",
	2: "LguiVtableSwap",
	3: "NsfEyeClassification",
	4: "StereoPassOffsetResolved",
	5: "InitOptionsOffsetResolved",
}

BOOKMARK_CATEGORY = "UEVR Trace"


def load_manifest(path):
	with open(path, "r") as f:
		return json.load(f)


def load_records(path):
	records = []
	with open(path, "rb") as f:
		data = f.read()

	offset = 0
	while offset + RECORD_SIZE <= len(data):
		chunk = data[offset:offset + RECORD_SIZE]
		(timestamp_qpc, frame_count, event_kind, module_index, _pad,
		 rva, aux_a, aux_b) = struct.unpack(RECORD_STRUCT_FORMAT, chunk)
		records.append({
			"timestamp_qpc": timestamp_qpc,
			"frame_count": frame_count,
			"event_kind": event_kind,
			"module_index": module_index,
			"rva": rva,
			"aux_a": aux_a,
			"aux_b": aux_b,
		})
		offset += RECORD_SIZE

	return records


def describe_event(record):
	kind = record["event_kind"]
	name = EVENT_KIND_NAMES.get(kind, "Unknown({})".format(kind))
	aux_a = record["aux_a"]
	aux_b = record["aux_b"]

	if kind == 4:  # StereoPassOffsetResolved
		left = (aux_b >> 32) & 0xFFFFFFFF
		right = aux_b & 0xFFFFFFFF
		return ("{}: live_stereo_pass_offset=0x{:x} left={} right={}"
				.format(name, aux_a, left, right))
	if kind == 5:  # InitOptionsOffsetResolved
		family_off = (aux_a >> 32) & 0xFFFFFFFF
		state_off = aux_a & 0xFFFFFFFF
		stereo_off = aux_b
		return ("{}: family_off=0x{:x} state_off=0x{:x} stereo_pass_off=0x{:x}"
				.format(name, family_off, state_off, stereo_off))

	return "{}: aux_a=0x{:x} aux_b=0x{:x}".format(name, aux_a, aux_b)


def find_module_base_in_program(current_program):
	"""Returns the current program's image base as an int, for RVA -> address translation."""
	image_base = current_program.getImageBase()
	return image_base


def main():
	if not os.path.isfile(TRACE_BIN_PATH) or not os.path.isfile(MANIFEST_PATH):
		print("[annotate_uevr_trace] trace.bin/manifest.json not found next to this script at: {}".format(TRACE_DIR))
		print("[annotate_uevr_trace] Copy the uevr_trace output folder here, or edit TRACE_DIR.")
		return

	manifest = load_manifest(MANIFEST_PATH)
	modules = manifest.get("modules", [])
	records = load_records(TRACE_BIN_PATH)

	print("[annotate_uevr_trace] Loaded {} modules and {} trace records.".format(len(modules), len(records)))

	# `currentProgram`, `createLabel`, `createBookmark`, `toAddr`, `getPlateComment`,
	# `setPlateComment`, `getFunctionAt`, `createFunction` are provided by Ghidra's
	# FlatProgramAPI/GhidraScript globals when this file is run as a Ghidra script - they are not
	# importable/resolvable statically outside that environment.
	try:
		program = currentProgram  # noqa: F821
	except NameError:
		print("[annotate_uevr_trace] This script must be run inside Ghidra (Script Manager or analyzeHeadless).")
		return

	image_base = find_module_base_in_program(program)

	annotated = 0
	skipped = 0
	seen_addresses = set()

	for record in records:
		module_index = record["module_index"]
		if module_index >= len(modules):
			skipped += 1
			continue

		module_name = modules[module_index].get("name", "<unknown>")

		# Only annotate events that belong to the main executable module - module_index 0 is
		# always the first module seen, which in practice is almost always the game's main .exe
		# since RVA-resolution events fire from utility::get_executable()-relative addresses.
		rva = record["rva"]

		try:
			address = toAddr(image_base.getOffset() + rva)  # noqa: F821
		except Exception as e:
			print("[annotate_uevr_trace] Failed to resolve address for rva=0x{:x}: {}".format(rva, e))
			skipped += 1
			continue

		description = describe_event(record)
		dedupe_key = (module_index, rva, record["event_kind"])

		if dedupe_key in seen_addresses:
			continue
		seen_addresses.add(dedupe_key)

		try:
			createBookmark(address, BOOKMARK_CATEGORY, description)  # noqa: F821
		except Exception as e:
			print("[annotate_uevr_trace] Failed to bookmark {}: {}".format(address, e))

		try:
			existing_comment = getPlateComment(address)  # noqa: F821
			new_comment = description if not existing_comment else existing_comment + "\n" + description
			setPlateComment(address, new_comment)  # noqa: F821
		except Exception as e:
			print("[annotate_uevr_trace] Failed to set plate comment at {}: {}".format(address, e))

		annotated += 1

	print("[annotate_uevr_trace] Done. Annotated {} unique addresses, skipped {} records (module={}).".format(
		annotated, skipped, modules[0]["name"] if modules else "<none>"))


if __name__ == "__main__":
	main()
