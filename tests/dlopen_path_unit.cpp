#include <cstdlib>
#include <cstdio>

#include "runtime/compat.hpp"

int main() {
  const char* name = std::getenv("TILT_DLOPEN_TEST_NAME");
  if (!name || !*name) {
    std::fprintf(stderr, "TILT_DLOPEN_TEST_NAME ausente\n");
    return 1;
  }
  void* lib = tilt::rt::tilt_dlopen(name);
  if (!lib) {
    const char* error = tilt::rt::tilt_dlerror();
    std::fprintf(stderr, "loader nao encontrou %s: %s\n", name, error ? error : "sem erro");
    return 1;
  }
  if (!tilt::rt::tilt_dlsym(lib, "cos")) {
    std::fprintf(stderr, "biblioteca carregada sem simbolo cos\n");
    tilt::rt::tilt_dlclose(lib);
    return 1;
  }
  tilt::rt::tilt_dlclose(lib);
  std::puts("dlopen_path_unit ok");
  return 0;
}
