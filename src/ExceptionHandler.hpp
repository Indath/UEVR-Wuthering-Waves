#include <windows.h>

namespace framework {
LONG WINAPI global_exception_handler(struct _EXCEPTION_POINTERS* ei);
void setup_exception_handler();

// Writes a full minidump (all threads, with stacks) of the CURRENTLY RUNNING process,
// without needing an exception to occur. Useful for diagnosing hangs/stalls where the
// process is still alive but stuck (e.g. deadlocked or spinning in a JIT'd trampoline).
// Returns true if the dump was written successfully.
bool write_live_process_dump(const std::string& filename);
}
