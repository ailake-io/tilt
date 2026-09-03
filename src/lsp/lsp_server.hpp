#pragma once

#include <iosfwd>

namespace tilt::lsp {

// Runs a minimal Language Server Protocol loop (JSON-RPC over the given
// streams): initialize, didOpen/didChange, completion, publishDiagnostics,
// shutdown/exit. Returns the process exit code.
int run_lsp(std::istream& in, std::ostream& out);

}  // namespace tilt::lsp
