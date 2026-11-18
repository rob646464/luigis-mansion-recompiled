#pragma once
#include <cstddef>

namespace port {
// Installs abort/terminate/SEH handlers that print a symbolized stack trace to stderr.
void installCrashHandler();
// Logs "[hang] ..." with the current phase (setPhase) when the main thread has not called
// heartbeat() for 15 s (then every 30 s). Linux: every thread's stack once per run at the first
// report (issue #11). LM_HANG_DUMP=<seconds>: also every thread's stack (three samples a second
// apart, per stall).
void startHangWatchdog();
void heartbeat();
// What the main thread is doing, for the hang log (a string literal: kept by pointer); nullptr
// while it waits on purpose (paused in the background), which is never reported.
void setPhase(const char* phase);
// The phase set by setPhase (what the main thread is doing), for other logs.
const char* phase();
// The main loop has ended on purpose and the process is tearing down: the hang watchdog and
// reportStall stay quiet from here on (releasing the GPU objects and writing the caches can take a
// while, which is not a hang), and the run is recorded as a clean exit, so a kill during the shutdown
// does not look like a crash at the next start. A crash in the shutdown is still reported.
void setShuttingDown();
// Extra state for every hang report (what Aurora's threads are doing, whether the game kept
// running). Called on the watchdog thread: with stallBegins = true when the main thread has just
// missed its first heartbeat (remember counters; `out` is ignored), false for a report.
using HangDetailFn = void (*)(char* out, std::size_t size, bool stallBegins);
void setHangDetail(HangDetailFn fn);
// A stall the main loop itself noticed (it runs, but something else stopped, e.g. no frame was
// presented or the game thread stopped finishing frames): one "[hang] <what>; <detail>" line, and
// with dumpThreads every thread's stack (at most twice per run).
void reportStall(const char* what, bool dumpThreads);
}  // namespace port
