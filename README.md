# microprof

A general purpose instrumentation profiler for C, single header.

Measure named regions of your code, get an aggregate table on stdout and a
Chrome Trace JSON you can open in `chrome://tracing` or [Perfetto](https://ui.perfetto.dev).

- No libc: Linux uses raw syscalls, Windows uses direct kernel32 imports.
- No allocation: fixed static arena (or hand in your own memory).
- Self-contained: defines the `i8..u64`/`b32` aliases itself, or defers to
  `base.h` if that was included first.

## Including it

In exactly **one** `.c` file:

```c
#define PROFILER_IMPLEMENTATION
#include "profiler.h"
```

Everywhere else just `#include "profiler.h"`.

## Instrumenting

```c
void simulate(void) {
    PROF_SCOPE("simulate") {
        PROF_SCOPE("broadphase") { ... }
        PROF_SCOPE("solve")      { ... }
    }
}

void tick(void) {
    PROF_SCOPE("frame") { simulate(); render(); }
    prof_frame_end();
}
```

Available macros:

| Macro                | Meaning                                              |
| -------------------- | ---------------------------------------------------- |
| `PROF_SCOPE(name)`   | Profile the following block.                         |
| `PROF_FUNCTION()`    | `PROF_SCOPE(__FUNCTION__)`.                          |
| `PROF_BEGIN(name)`   | Explicit begin, pair with `PROF_END()`.              |
| `PROF_END()`         | Close the innermost zone.                            |

`PROF_SCOPE`/`PROF_FUNCTION` use a for-loop, so an early `return`/`break`/`goto`
out of the body skips the matching end. Use `PROF_BEGIN`/`PROF_END` if the body
can leave early.

## Reporting

```c
prof_report();                    /* text table -> stdout */
prof_write_trace("trace.json");   /* chrome / perfetto  */
prof_frame_end();                 /* call once per frame */
prof_reset();                     /* zero the counters   */
prof_set_recording(false);        /* stop the event stream, keep aggregating */
prof_init(memory, bytes);         /* use your own event arena instead */
```

`prof_report()` prints self (exclusive) time, inclusive time, percentage of the
top-level total, and hit counts, sorted by self time:

## OPTIONAL Configuration

Define before including `profiler.h`:

| Macro                | Default | Meaning                              |
| -------------------- | ------- | ------------------------------------ |
| `PROF_ARENA_BYTES`   | 8 MiB   | Event stream backing store.          |
| `PROF_MAX_DEPTH`     | 256     | Max nesting depth per thread.        |
| `PROF_MAX_ZONES`     | 1024    | Max distinct call sites.             |
| `PROF_MAX_THREADS`   | 16      | Max threads.                         |
| `PROF_ZONE_SLOTS`    | 4096    | Zone hash table size (power of two). |
| `PROF_PROCESS_NAME`  | "process" | Name in trace metadata.            |
| `PROF_DISABLE`       | —       | Compile all macros to nothing.       |
