#pragma once

// Camada de compatibilidade POSIX <-> Windows (MSVC e MinGW) do runtime tilt.
//
// Nucleo do port Windows: todo include de cabecalho POSIX (sys/socket.h,
// netdb.h, dlfcn.h, dirent.h, unistd.h, sys/stat.h) fica confinado neste
// header — em #ifndef _WIN32 — e os pontos de chamada usam as funcoes
// tilt_* abaixo. Em Linux/macOS cada tilt_* e um inline transparente sobre a
// API POSIX original (ex.: tilt_close_socket -> close), entao o comportamento
// POSIX nao muda. Sob _WIN32 as funcoes mapeiam para Winsock2 / Win32:
//   - sockets:  socket/connect/accept/recv/send existem no Winsock com os
//     mesmos nomes; close(2) vira closesocket e fd de socket e tratado como
//     int (o handle SOCKET cabe em int nas implementacoes atuais).
//   - dlopen/dlsym/dlerror/dlclose -> LoadLibraryA/GetProcAddress/FreeLibrary
//     (dlerror usa FormatMessage + GetLastError).
//   - popen/pclose -> _popen/_pclose (curl.exe invocado no cmd, mesmo
//     padrao do s3/llm/qdrant/iceberg REST).
//   - dirent -> FindFirstFileA/FindNextFileA (tilt_listdir devolve so os
//     nomes das entradas, o suficiente para delta/iceberg).
//   - mkstemp -> tilt_tempfile (arquivo exclusivo em $TEMP, fd via _open).
//
// Inclua este header no lugar dos headers POSIX; nao inclua <windows.h> nem
// <winsock2.h> diretamente em outros arquivos (a ordem winsock2 -> windows
// ja e garantida aqui via WIN32_LEAN_AND_MEAN/NOMINMAX).

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  // O fd_set do Winsock comporta 64 sockets por padrao; o servidor HTTP usa
  // kMaxConns = 256. Precisa vir antes de <winsock2.h>.
  #ifndef FD_SETSIZE
    #define FD_SETSIZE 1024
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>

  #include <cstdio>
  #include <cstdint>
  #include <ctime>
  #include <string>
  #include <vector>
#else
  #include <arpa/inet.h>
  #include <dirent.h>
  #include <dlfcn.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #include <cerrno>
  #include <cstdio>
  #include <cstdint>
  #include <ctime>
  #include <string>
  #include <vector>
#endif

namespace tilt::rt {

#if defined(_WIN32)
// O Winsock trabalha com int nos retornos de recv/send; o alias espelha
// ssize_t POSIX para os mesmos call sites compilarem nas duas plataformas.
using ssize_t = std::intptr_t;
#endif

// --------------------------------------------------------------------------
// Ciclo de vida do Winsock (WSAStartup/WSACleanup). No POSIX sao no-ops.
// O CLI instancia TiltWsaGuard no inicio do run_cli sob _WIN32.
// --------------------------------------------------------------------------
#if defined(_WIN32)
int tilt_socket_init();
int tilt_socket_cleanup();
struct TiltWsaGuard {
  TiltWsaGuard() { (void)tilt_socket_init(); }
  ~TiltWsaGuard() { (void)tilt_socket_cleanup(); }
  TiltWsaGuard(const TiltWsaGuard&) = delete;
  TiltWsaGuard& operator=(const TiltWsaGuard&) = delete;
};
#else
inline int tilt_socket_init() { return 0; }
inline int tilt_socket_cleanup() { return 0; }
#endif

// --------------------------------------------------------------------------
// Sockets. tilt_send ja embute MSG_NOSIGNAL no POSIX (no Windows o flag nao
// existe: SIGPIPE nao existe e o Winsock nao levanta esse sinal).
// --------------------------------------------------------------------------
#if defined(_WIN32)
inline int tilt_socket(int family, int type, int protocol) {
  return static_cast<int>(::socket(family, type, protocol));
}
inline int tilt_close_socket(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
inline ssize_t tilt_send(int fd, const char* p, std::size_t n) {
  return ::send(static_cast<SOCKET>(fd), p, static_cast<int>(n), 0);
}
inline ssize_t tilt_recv(int fd, char* p, std::size_t n) {
  return ::recv(static_cast<SOCKET>(fd), p, static_cast<int>(n), 0);
}
inline int tilt_last_net_error() { return WSAGetLastError(); }
constexpr int kNetWouldBlock = WSAEWOULDBLOCK;
// Timeout de recv/send: no POSIX e um timeval via setsockopt; no Windows o
// Winsock espera DWORD em milissegundos no mesmo optname.
inline void tilt_set_sock_timeouts(int fd, int sec) {
  DWORD ms = static_cast<DWORD>(sec) * 1000u;
  ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&ms), sizeof(ms));
  ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&ms), sizeof(ms));
}
inline void tilt_set_reuseaddr(int fd) {
  int on = 1;
  ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&on), sizeof(on));
}
#else
inline int tilt_socket(int family, int type, int protocol) {
  return ::socket(family, type, protocol);
}
inline int tilt_close_socket(int fd) { return ::close(fd); }
inline ssize_t tilt_send(int fd, const char* p, std::size_t n) {
  return ::send(fd, p, n, MSG_NOSIGNAL);
}
inline ssize_t tilt_recv(int fd, char* p, std::size_t n) { return ::recv(fd, p, n, 0); }
inline int tilt_last_net_error() { return errno; }
constexpr int kNetWouldBlock = EAGAIN;
inline void tilt_set_sock_timeouts(int fd, int sec) {
  timeval tv{};
  tv.tv_sec = sec;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
inline void tilt_set_reuseaddr(int fd) {
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}
#endif

// --------------------------------------------------------------------------
// Processos e arquivos.
// --------------------------------------------------------------------------
#if defined(_WIN32)
FILE* tilt_popen(const char* cmd, const char* mode);   // _popen (cmd.exe)
int tilt_pclose(FILE* pipe);                           // _pclose
int tilt_close_file(int fd);                           // _close (fd de arquivo)
#else
inline FILE* tilt_popen(const char* cmd, const char* mode) { return ::popen(cmd, mode); }
inline int tilt_pclose(FILE* pipe) { return ::pclose(pipe); }
inline int tilt_close_file(int fd) { return ::close(fd); }
#endif

// Cria um arquivo temporario exclusivo ("tilt_<tag>_XXXXXX") em $TEMP
// (Windows) ou /tmp (POSIX), preenche `path` com o caminho e devolve o fd
// aberto (>= 0, para _close/tilt_close_file), ou -1 em erro.
int tilt_tempfile(const char* tag, std::string& path);

bool tilt_file_exists(const std::string& path);
std::int64_t tilt_file_size(const std::string& path);  // -1 se ausente/erro
bool tilt_is_directory(const std::string& path);
int tilt_mkdir(const std::string& path);               // 0 ok (modo 0755 POSIX)

// Lista os nomes das entradas de um diretorio em `out`; devolve false se o
// diretorio nao abrir (para o caller manter seu erro claro). "." e ".." sao
// omitidos — os call sites so filtram por sufixo/prefixo.
bool tilt_listdir(const std::string& path, std::vector<std::string>& out);

// gmtime_r/localtime_r vs gmtime_s/localtime_s.
std::tm tilt_gmtime(std::time_t t);
std::tm tilt_localtime(std::time_t t);

// getpid/_getpid e getcwd/_getcwd (buffer de PATH_MAX; false em erro).
int tilt_getpid();
bool tilt_getcwd(std::string& out);

// Caminho absoluto do executavel do processo (para localizar a stdlib ao
// lado do binario). Windows: GetModuleFileNameW; macOS: _NSGetExecutablePath
// + realpath; demais POSIX: readlink(/proc/self/exe). Se a deteccao nativa
// falhar, usa `argv0` — o CLI repassa argv[0] — via realpath; nome puro (sem
// '/') e procurado no PATH. String vazia se nada resolver.
std::string tilt_exe_path(const char* argv0 = nullptr);

// --------------------------------------------------------------------------
// Carregamento dinamico de bibliotecas (dlopen vs LoadLibrary).
// tilt_dlopen tenta `path`; `global` corresponde a RTLD_GLOBAL (POSIX-only,
// ignorado no Windows). tilt_dlerror devolve nullptr quando nao ha erro;
// caso contrario aponta para um buffer thread-local valido ate a proxima
// chamada tilt_dl* na mesma thread.
// --------------------------------------------------------------------------
void* tilt_dlopen(const char* path, bool global = false);
void* tilt_dlsym(void* lib, const char* name);
const char* tilt_dlerror();
int tilt_dlclose(void* lib);

}  // namespace tilt::rt
