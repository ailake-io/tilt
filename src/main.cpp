#include "cli/cli.hpp"
#include "runtime/sql_pool.hpp"

int main(int argc, char** argv) {
  const int rc = tilt::run_cli(argc, argv);
  tilt::rt::sql_pool_fechar_ociosas();
  return rc;
}
