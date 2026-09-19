#include "runtime/leader.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "runtime/compat.hpp"

namespace tilt::rt {

namespace {

int leader_open_exclusive(const std::string& path) {
#if defined(_WIN32)
  return ::_open(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, 0600);
#else
  return ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
}

std::int64_t leader_write(int fd, const std::string& payload) {
#if defined(_WIN32)
  return ::_write(fd, payload.data(), static_cast<unsigned>(payload.size()));
#else
  return ::write(fd, payload.data(), payload.size());
#endif
}

}  // namespace

std::string leader_dono() {
  char host[256] = {};
  ::gethostname(host, sizeof(host) - 1);
  return std::string(host) + ":" + std::to_string(tilt_getpid());
}

static long agora_epoch() {
  return static_cast<long>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

static bool ler_lease(const std::string& path, std::string& dono, long& expira) {
  std::ifstream in(path);
  if (!in) return false;
  std::string conteudo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::istringstream ss(conteudo);
  if (!(ss >> dono >> expira)) return false;
  return true;
}

static void gravar_lease(const std::string& path, const std::string& dono, long expira) {
  const std::string tmp = path + ".tmp." + std::to_string(tilt_getpid());
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << dono << " " << expira << "\n";
  }
  std::rename(tmp.c_str(), path.c_str());
}

bool leader_tentar(const std::string& lease_path, int ttl_seg, std::string& motivo) {
  if (lease_path.empty()) {
    motivo = "sem lease configurado";
    return true;  // sem eleicao: comporta-se como lider unico
  }
  if (ttl_seg <= 0) ttl_seg = 15;
  const std::string eu = leader_dono();
  const long agora = agora_epoch();

  const int fd = leader_open_exclusive(lease_path);
  if (fd >= 0) {
    const std::string payload = eu + " " + std::to_string(agora + ttl_seg) + "\n";
    const ssize_t nw = leader_write(fd, payload);
    tilt_close_file(fd);
    if (nw != static_cast<std::int64_t>(payload.size())) {
      motivo = "falha ao gravar lease";
      return false;
    }
    motivo = "lease adquirido";
    return true;
  }
  std::string dono;
  long expira = 0;
  if (!ler_lease(lease_path, dono, expira)) {
    // Arquivo ilegivel/corrompido: tenta tomar posse por cima.
    gravar_lease(lease_path, eu, agora + ttl_seg);
    motivo = "lease corrompido; posse assumida";
    return true;
  }
  if (dono == eu) {
    gravar_lease(lease_path, eu, agora + ttl_seg);
    motivo = "lease renovado";
    return true;
  }
  if (expira <= agora) {
    gravar_lease(lease_path, eu, agora + ttl_seg);
    motivo = "lease expirado de '" + dono + "'; posse assumida";
    return true;
  }
  motivo = "lider ativo '" + dono + "' ate " + std::to_string(expira);
  return false;
}

void leader_renovar(const std::string& lease_path, int ttl_seg) {
  if (lease_path.empty()) return;
  if (ttl_seg <= 0) ttl_seg = 15;
  std::string dono;
  long expira = 0;
  if (ler_lease(lease_path, dono, expira) && dono == leader_dono()) {
    gravar_lease(lease_path, dono, agora_epoch() + ttl_seg);
  }
}

void leader_liberar(const std::string& lease_path) {
  if (lease_path.empty()) return;
  std::string dono;
  long expira = 0;
  if (ler_lease(lease_path, dono, expira) && dono == leader_dono()) {
    std::remove(lease_path.c_str());
  }
}

}  // namespace tilt::rt
