#include "runtime/compat.hpp"

#include <random>
#include <stdexcept>

#if defined(_WIN32)
  #include <cerrno>
  #include <fcntl.h>
  #include <io.h>
#else
  #include <cerrno>
#endif

namespace tilt::rt {

#if defined(_WIN32)

int tilt_socket_init() {
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa);
}

int tilt_socket_cleanup() { return WSACleanup(); }

namespace {
thread_local std::string g_dl_error;

void set_dl_error() {
  const DWORD code = GetLastError();
  if (code == 0) {
    g_dl_error = "erro desconhecido ao carregar biblioteca";
    return;
  }
  char buf[512] = {0};
  const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, 0, buf, sizeof(buf) - 1, nullptr);
  g_dl_error = n > 0 ? std::string(buf, n) : ("erro Windows " + std::to_string(code));
  while (!g_dl_error.empty() &&
         (g_dl_error.back() == '\r' || g_dl_error.back() == '\n' || g_dl_error.back() == ' ')) {
    g_dl_error.pop_back();
  }
}
}  // namespace

void* tilt_dlopen(const char* path, bool /*global*/) {
  HMODULE mod = LoadLibraryA(path);
  if (!mod) set_dl_error();
  return static_cast<void*>(mod);
}

void* tilt_dlsym(void* lib, const char* name) {
  if (!lib) {
    g_dl_error = "biblioteca nao carregada";
    return nullptr;
  }
  FARPROC proc = GetProcAddress(static_cast<HMODULE>(lib), name);
  if (!proc) set_dl_error();
  return reinterpret_cast<void*>(proc);
}

const char* tilt_dlerror() {
  if (g_dl_error.empty()) return nullptr;
  const std::string err = g_dl_error;
  g_dl_error.clear();
  // O conteudo precisa sobreviver ao clear(): devolvemos o buffer atual e
  // deixamos a proxima chamada sobrescreve-lo. Para isso guardamos uma copia
  // estatica por thread.
  thread_local std::string g_last;
  g_last = err;
  return g_last.c_str();
}

int tilt_dlclose(void* lib) {
  if (!lib) return -1;
  return FreeLibrary(static_cast<HMODULE>(lib)) ? 0 : -1;
}

FILE* tilt_popen(const char* cmd, const char* mode) { return _popen(cmd, mode); }

int tilt_pclose(FILE* pipe) { return _pclose(pipe); }

int tilt_close_file(int fd) { return _close(fd); }

int tilt_tempfile(const char* tag, std::string& path) {
  char dir[MAX_PATH + 1] = {0};
  const DWORD n = GetTempPathA(MAX_PATH, dir);
  if (n == 0 || n > MAX_PATH) return -1;
  std::string base = dir;
  while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();

  std::mt19937_64 gen(std::random_device{}());
  static constexpr char kHex[] = "0123456789abcdef";
  for (int tentativa = 0; tentativa < 100; ++tentativa) {
    std::string candidate = base + "\\tilt_" + tag + "_";
    for (int i = 0; i < 8; ++i) {
      candidate += kHex[static_cast<std::uint64_t>(gen()) & 0xFu];
    }
    const int fd = _open(candidate.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
    if (fd >= 0) {
      path = candidate;
      return fd;
    }
    if (errno != EEXIST) return -1;
  }
  return -1;
}

bool tilt_file_exists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::int64_t tilt_file_size(const std::string& path) {
  struct _stat64 st {};
  if (_stat64(path.c_str(), &st) != 0) return -1;
  return static_cast<std::int64_t>(st.st_size);
}

bool tilt_is_directory(const std::string& path) {
  const DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int tilt_mkdir(const std::string& path) { return _mkdir(path.c_str()); }

bool tilt_listdir(const std::string& path, std::vector<std::string>& out) {
  out.clear();
  WIN32_FIND_DATAA fd{};
  const HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    const char* name = fd.cFileName;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
    out.emplace_back(name);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return true;
}

std::tm tilt_gmtime(std::time_t t) {
  std::tm out {};
  gmtime_s(&out, &t);
  return out;
}

std::tm tilt_localtime(std::time_t t) {
  std::tm out {};
  localtime_s(&out, &t);
  return out;
}

#else  // POSIX

void* tilt_dlopen(const char* path, bool global) {
  return ::dlopen(path, RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
}

void* tilt_dlsym(void* lib, const char* name) { return ::dlsym(lib, name); }

const char* tilt_dlerror() { return ::dlerror(); }

int tilt_dlclose(void* lib) { return ::dlclose(lib); }

int tilt_tempfile(const char* tag, std::string& path) {
  std::string tmpl = "/tmp/tilt_" + std::string(tag) + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const int fd = ::mkstemp(buf.data());
  if (fd < 0) return -1;
  path.assign(buf.data());
  return fd;
}

bool tilt_file_exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

std::int64_t tilt_file_size(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<std::int64_t>(st.st_size);
}

bool tilt_is_directory(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int tilt_mkdir(const std::string& path) { return ::mkdir(path.c_str(), 0755); }

bool tilt_listdir(const std::string& path, std::vector<std::string>& out) {
  out.clear();
  DIR* d = ::opendir(path.c_str());
  if (!d) return false;
  while (dirent* e = ::readdir(d)) {
    const char* name = e->d_name;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
    out.emplace_back(name);
  }
  ::closedir(d);
  return true;
}

std::tm tilt_gmtime(std::time_t t) {
  std::tm out {};
  gmtime_r(&t, &out);
  return out;
}

std::tm tilt_localtime(std::time_t t) {
  std::tm out {};
  localtime_r(&t, &out);
  return out;
}

#endif

}  // namespace tilt::rt
