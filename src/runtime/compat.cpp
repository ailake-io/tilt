#include "runtime/compat.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <cerrno>
  #include <direct.h>   // _mkdir
  #include <fcntl.h>
  #include <io.h>
  #include <process.h>  // _getpid
#else
  #include <cerrno>
  #include <climits>    // PATH_MAX
  #include <cstdlib>    // realpath, getenv
  #include <cstring>    // strchr
  #include <sstream>    // PATH splitting
  #include <unistd.h>   // getpid, getcwd, readlink, access
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>  // _NSGetExecutablePath
  #endif
#endif

namespace tilt::rt {

namespace {

// Content snaps e instalacoes portaveis podem expor drivers nativos fora do
// caminho padrao do loader. Mantemos o nome curto usado pelos conectores e
// tentamos cada diretorio de TILT_DRIVER_PATH como fallback.
std::vector<std::string> driver_candidates(const char* path) {
  std::vector<std::string> out;
  if (!path || !*path || std::strchr(path, '/') || std::strchr(path, '\\')) return out;
  const char* raw = std::getenv("TILT_DRIVER_PATH");
  if (!raw || !*raw) return out;
#if defined(_WIN32)
  constexpr char separator = ';';
  constexpr char slash = '\\';
#else
  constexpr char separator = ':';
  constexpr char slash = '/';
#endif
  std::string dir;
  for (const char* p = raw;; ++p) {
    if (*p == separator || *p == '\0') {
      if (!dir.empty()) out.push_back(dir + slash + path);
      dir.clear();
      if (*p == '\0') break;
    } else {
      dir.push_back(*p);
    }
  }
  return out;
}

}  // namespace

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
  if (!mod) {
    for (const std::string& candidate : driver_candidates(path)) {
      mod = LoadLibraryA(candidate.c_str());
      if (mod) break;
    }
  }
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

int tilt_getpid() { return ::_getpid(); }

bool tilt_getcwd(std::string& out) {
  char buf[4096];
  if (!::_getcwd(buf, sizeof(buf))) return false;
  out = buf;
  return true;
}

bool tilt_enable_vt() {
  // Console moderno (Windows 10+) entende ANSI com a flag; sem console
  // (pipe/arquivo) GetConsoleMode falha e seguimos sem cores.
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) return false;
  DWORD mode = 0;
  if (!GetConsoleMode(h, &mode)) return false;
  if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
  return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

std::string tilt_exe_path(const char* argv0) {
  // GetModuleFileNameW trunca para o tamanho do buffer (retorno == tamanho);
  // cresce o buffer ate caber.
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) break;  // falha real; cai no fallback
    if (n < buf.size() - 1) {
      buf.resize(n);
      return std::filesystem::path(buf).string();
    }
    if (buf.size() >= 65536) break;
    buf.resize(buf.size() * 2);
  }
  if (argv0 && *argv0) {
    char full[MAX_PATH] = {0};
    if (GetFullPathNameA(argv0, MAX_PATH, full, nullptr) > 0) return full;
  }
  return {};
}

#else  // POSIX

void* tilt_dlopen(const char* path, bool global) {
  const int flags = RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL);
  void* lib = ::dlopen(path, flags);
  if (lib) return lib;
  for (const std::string& candidate : driver_candidates(path)) {
    lib = ::dlopen(candidate.c_str(), flags);
    if (lib) return lib;
  }
  return nullptr;
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

int tilt_getpid() { return ::getpid(); }

bool tilt_enable_vt() { return true; }  // POSIX entende ANSI; o caller filtra com isatty

std::string tilt_posix_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string tilt_win_quote(const std::string& s) {
  if (s.empty()) return "\"\"";
  bool need = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '&' || c == '|' || c == '<' ||
        c == '>' || c == '^' || c == '%') {
      need = true;
      break;
    }
  }
  if (!need) return s;
  std::string out = "\"";
  std::size_t bs = 0;
  for (char c : s) {
    if (c == '\\') {
      ++bs;
      continue;
    }
    if (c == '"') {
      out.append(bs * 2 + 1, '\\');
      out += '"';
      bs = 0;
      continue;
    }
    out.append(bs, '\\');
    bs = 0;
    out += c;
  }
  out.append(bs * 2, '\\');  // barras finais dobram (senao escapam a aspa final)
  out += '"';
  return out;
}

bool tilt_getcwd(std::string& out) {
  char buf[4096];
  if (!::getcwd(buf, sizeof(buf))) return false;
  out = buf;
  return true;
}

std::string tilt_exe_path(const char* argv0) {
#if defined(__APPLE__)
  // /proc nao existe no macOS; _NSGetExecutablePath devolve o caminho
  // (possivelmente com symlinks nao resolvidos) que realpath normaliza.
  char buf[4096];
  uint32_t n = sizeof(buf);
  if (::_NSGetExecutablePath(buf, &n) == 0) {
    if (char* rp = ::realpath(buf, nullptr)) {
      std::string out(rp);
      std::free(rp);
      return out;
    }
    return buf;
  }
#else
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return buf;
  }
#endif
  if (argv0 && *argv0) {
    if (char* rp = ::realpath(argv0, nullptr)) {
      std::string out(rp);
      std::free(rp);
      return out;
    }
    if (std::strchr(argv0, '/') == nullptr) {
      // nome puro: procura no PATH
      const char* path = ::getenv("PATH");
      if (path) {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
          const std::string cand = (dir.empty() ? "." : dir) + "/" + argv0;
          if (::access(cand.c_str(), X_OK) == 0) {
            if (char* rp = ::realpath(cand.c_str(), nullptr)) {
              std::string out(rp);
              std::free(rp);
              return out;
            }
            return cand;
          }
        }
      }
    }
  }
  return {};
}

#endif

}  // namespace tilt::rt
