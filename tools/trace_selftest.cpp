// trace_selftest.exe — runs the tracer's own falsifier.
//
// Proves, on real hardware and without the game, that the shipped arm / trap /
// restore / re-arm / drain path actually works and that retail_symbols.bin
// resolves the addresses we claim it does. Must be run from the folder holding
// retail_symbols.bin (the build drops both in out\).
//
// Exit code = number of failed checks; 0 means everything passed.
#include "../src/retail_fn_trace.h"

int main() {
    return RTF_SelfTest();
}
