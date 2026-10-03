# UEVR Offline Trace -> Ghidra Annotation

This folder contains an offline companion to the in-game `TraceWriter` (see
`src/mods/vr/TraceWriter.hpp` / `.cpp`). Instead of streaming live data over a socket while
debugging, the game writes a compact binary trace to disk, and this script later annotates a
static Ghidra analysis of the game binary from that trace - no live connection required.

## How the trace is produced

1. Launch the game with UEVR injected as usual.
2. Open the UEVR debug UI and enable **"DIAG: Enable Offline Trace Writer"** (under the VR /
   Compatibility diagnostics section in `src/mods/VR.cpp`). Enabling it truncates any previous
   trace and starts a fresh recording session.
3. Play for a bit - in particular, make sure you get past the loading screen, since several of the
   traced events (stereo-pass offset resolution, init-options offset resolution) only fire once
   `sceneview_xref` has resolved them against live `FSceneView`/`FSceneViewInitOptions` instances.
4. Optionally click **"DIAG: Flush Trace Now"** before closing the game to make sure the last
   partial buffer is written (the writer also auto-flushes every 2048 buffered records).
5. The trace output lives in the UEVR persistent data directory, under `uevr_trace/`:
   - `uevr_trace/trace.bin` - binary trace records (fixed 48-byte layout, see below).
   - `uevr_trace/manifest.json` - JSON manifest of every module referenced by the trace (name,
	 base address, size), written once per flush.

## How to annotate the binary

1. Copy `trace.bin` and `manifest.json` into this folder (`tools/ghidra/`), or edit the `TRACE_DIR`
   constant at the top of `annotate_uevr_trace.py` to point at wherever you copied them.
2. Open the game's main executable in Ghidra and let auto-analysis finish.
3. Run `annotate_uevr_trace.py` via **Window > Script Manager** (or headlessly with
   `analyzeHeadless ... -postScript annotate_uevr_trace.py`).
4. The script will:
   - Load `manifest.json` to know which modules were referenced.
   - Parse `trace.bin` into individual trace records.
   - Resolve each record's module-relative RVA to an address in the currently open program (using
	 the program's image base), using the main executable as the implicit reference module.
   - Create a bookmark (category `UEVR Trace`) and append a plate comment at each unique address,
	 describing the event (e.g. resolved stereo-pass offset, left/right eye pass values, or
	 init-options family/state/stereo-pass offsets).
5. Use the Bookmarks window (filtered to category `UEVR Trace`) to jump straight to every address
   the runtime trace flagged as interesting.

## Trace record format

Each record is a fixed 48-byte little-endian struct (`uevr_trace::TraceRecord` in
`src/mods/vr/TraceWriter.hpp`):

| Field          | Type     | Notes                                               |
|----------------|----------|------------------------------------------------------|
| timestamp_qpc  | uint64   | `QueryPerformanceCounter` ticks, for ordering only    |
| frame_count    | uint32   | Currently unused/reserved (0)                         |
| event_kind     | uint32   | See `EventKind` enum                                  |
| module_index   | uint32   | Index into `manifest.json`'s `modules` array          |
| _pad           | uint32   | Padding, always 0                                     |
| rva            | uint64   | Module-relative RVA this event concerns               |
| aux_a          | uint64   | Event-specific payload                                |
| aux_b          | uint64   | Event-specific payload                                |

`EventKind` values (keep in sync with `src/mods/vr/TraceWriter.hpp`):

| Value | Name                      | Payload                                                    |
|-------|---------------------------|--------------------------------------------------------------|
| 0     | Unknown                   | -                                                              |
| 1     | SceneViewCtorCallSite     | reserved for future use                                       |
| 2     | LguiVtableSwap            | reserved for future use                                       |
| 3     | NsfEyeClassification      | reserved for future use                                       |
| 4     | StereoPassOffsetResolved  | aux_a = live_stereo_pass_offset; aux_b = (left<<32 \| right)   |
| 5     | InitOptionsOffsetResolved | aux_a = (family_off<<32 \| state_off); aux_b = stereo_pass_off|

## Extending

- To trace a new event, add a value to `EventKind` in `TraceWriter.hpp` (append-only - never
  renumber existing values so old traces keep parsing correctly), call
  `uevr_trace::TraceWriter::get().record(...)` from the relevant hook, and add a matching
  human-readable case to `describe_event()` in `annotate_uevr_trace.py`.
- `TraceRecord` itself is fixed-size and must not be reordered/resized without bumping
  `format_version` in the manifest and updating `RECORD_STRUCT_FORMAT` here to match.
