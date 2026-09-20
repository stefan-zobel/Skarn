#pragma once

// =============================================================================
// SelfTest.h -- skarn_lsp's own test suite (`skarn_lsp --selftest`).
//
// Like vm_tests, main() IS the runner: one line per check, a
// "==== skarn_lsp selftest: N passed, M failed ====" summary, and a non-zero exit code
// on any failure, so a build gate needs nothing but the exit status. Covers the JSON
// reader/writer, the message framing, the front-end-to-diagnostic mapping (in-memory
// sources, no files needed) and one scripted protocol session end to end.
// =============================================================================

#include <iosfwd>

namespace lsp {

int run_selftest(std::ostream& out);

} // namespace lsp
