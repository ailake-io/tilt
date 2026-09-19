#include "runtime/cluster.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

namespace tilt::rt {
namespace {

constexpr auto kPoll = std::chrono::milliseconds(25);

bool aguardar_predicado(const std::function<bool()>& pronto, int timeout_sec, std::string& error,
                        const std::string& alvo) {
  const auto inicio = std::chrono::steady_clock::now();
  const auto timeout = std::chrono::seconds(timeout_sec > 0 ? timeout_sec : 120);
  while (!pronto()) {
    if (std::chrono::steady_clock::now() - inicio >= timeout) {
      error = "timeout aguardando '" + alvo + "'";
      return false;
    }
    std::this_thread::sleep_for(kPoll);
  }
  return true;
}

}  // namespace

bool cluster_preparar(const std::string& dir, std::string& error) {
  if (dir.empty()) {
    error = "diretorio do cluster vazio";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    error = "nao foi possivel criar diretorio do cluster '" + dir + "': " + ec.message();
    return false;
  }
  return true;
}

bool cluster_barreira(const std::string& dir, int rank, int world, int round, int timeout_sec,
                      std::string& error) {
  if (!cluster_preparar(dir, error)) return false;
  const std::filesystem::path ready =
      std::filesystem::path(dir) /
      ("barrier-" + std::to_string(round) + "-rank-" + std::to_string(rank) + ".ready");
  const std::filesystem::path tmp = ready.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      error = "nao foi possivel gravar '" + tmp.string() + "'";
      return false;
    }
    out << rank << "\n";
  }
  std::error_code ec;
  std::filesystem::rename(tmp, ready, ec);
  if (ec) {
    std::filesystem::remove(tmp);
    error = "nao foi possivel publicar barreira '" + ready.string() + "': " + ec.message();
    return false;
  }
  return aguardar_predicado(
      [&] {
        for (int r = 0; r < world; ++r) {
          if (!std::filesystem::exists(std::filesystem::path(dir) /
                                       ("barrier-" + std::to_string(round) + "-rank-" +
                                        std::to_string(r) + ".ready")))
            return false;
        }
        return true;
      },
      timeout_sec, error, "barreira " + std::to_string(round));
}

bool cluster_aguardar(const std::string& path, int timeout_sec, std::string& error) {
  return aguardar_predicado([&] { return std::filesystem::exists(path); }, timeout_sec, error,
                            path);
}

}  // namespace tilt::rt
