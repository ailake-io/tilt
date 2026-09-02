#pragma once

namespace tilt {

// Entry point for the `tilt` command-line tool.
// Returns the process exit code: 0 ok, 1 diagnostics, 2 usage, 3 not implemented.
int run_cli(int argc, char** argv);

}  // namespace tilt
