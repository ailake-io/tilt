#include "runtime/cluster.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace tilt::rt {
namespace {

constexpr auto kPoll = std::chrono::milliseconds(25);

bool publicar_texto_atomico(const std::filesystem::path& path, const std::string& texto,
                            std::string& error) {
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      error = "nao foi possivel gravar '" + tmp.string() + "'";
      return false;
    }
    out << texto;
    out.flush();
    if (!out) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      error = "falha ao gravar '" + path.string() + "'";
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    // A segunda tentativa permite reutilizar um diretório depois de uma
    // execução interrompida, sem expor um arquivo parcial aos leitores.
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
  }
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    error = "nao foi possivel publicar '" + path.string() + "': " + ec.message();
    return false;
  }
  return true;
}

std::string ausentes(const std::string& dir, int world, std::int64_t round) {
  std::ostringstream result;
  bool primeiro = true;
  for (int r = 0; r < world; ++r) {
    const auto marker = std::filesystem::path(dir) /
                        ("barrier-" + std::to_string(round) + "-rank-" + std::to_string(r) +
                         ".ready");
    if (std::filesystem::exists(marker)) continue;
    const auto hb = std::filesystem::path(dir) / ("heartbeat-rank-" + std::to_string(r));
    if (!primeiro) result << ",";
    primeiro = false;
    result << "rank " << r;
    std::ifstream input(hb);
    std::int64_t timestamp = 0;
    if (input >> timestamp && timestamp > 0) {
      const auto agora = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
      result << " (heartbeat ha " << std::max<std::int64_t>(0, agora - timestamp) << " ms)";
    } else {
      result << " (sem heartbeat)";
    }
  }
  return result.str();
}

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

bool cluster_heartbeat(const std::string& dir, int rank, int world, std::int64_t round,
                       std::string& error) {
  if (world < 1 || rank < 0 || rank >= world) {
    error = "rank/mundo invalidos para heartbeat";
    return false;
  }
  if (!cluster_preparar(dir, error)) return false;
  const auto path = std::filesystem::path(dir) / ("heartbeat-rank-" + std::to_string(rank));
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::ostringstream text;
  text << now << " " << round << "\n";
  return publicar_texto_atomico(path, text.str(), error);
}

bool cluster_barreira(const std::string& dir, int rank, int world, std::int64_t round, int timeout_sec,
                      std::string& error) {
  if (!cluster_preparar(dir, error)) return false;
  if (!cluster_heartbeat(dir, rank, world, round, error)) return false;
  const std::filesystem::path ready =
      std::filesystem::path(dir) /
      ("barrier-" + std::to_string(round) + "-rank-" + std::to_string(rank) + ".ready");
  if (!publicar_texto_atomico(ready, std::to_string(rank) + "\n", error)) return false;
  const bool pronto = aguardar_predicado(
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
  if (!pronto) {
    const std::string missing = ausentes(dir, world, round);
    if (!missing.empty()) error += "; workers ausentes: " + missing;
  }
  return pronto;
}

bool cluster_aguardar(const std::string& path, int timeout_sec, std::string& error) {
  return aguardar_predicado([&] { return std::filesystem::exists(path); }, timeout_sec, error,
                            path);
}

bool cluster_allreduce(const std::string& dir, int rank, int world, std::int64_t round,
                       const std::vector<float>& local, std::vector<float>& reduced,
                       int timeout_sec, std::string& error, int retries) {
  if (world < 2 || rank < 0 || rank >= world) {
    error = "rank/mundo invalidos para all-reduce";
    return false;
  }
  if (!cluster_preparar(dir, error)) return false;
  const auto prefix = std::string("allreduce-") + std::to_string(round);
  const auto local_path = std::filesystem::path(dir) /
                          (prefix + "-rank-" + std::to_string(rank) + ".bin");
  std::ostringstream encoded;
  encoded << local.size() << "\n" << std::setprecision(9);
  for (float value : local) encoded << value << "\n";
  if (!cluster_heartbeat(dir, rank, world, round, error) ||
      !publicar_texto_atomico(local_path, encoded.str(), error))
    return false;
  // Mantem o namespace de barreiras separado do sync de epoca.
  const std::int64_t barrier_base = 1000000 + round * 2;
  auto barreira_com_retentativa = [&](std::int64_t barrier_round) {
    const int tentativas = std::max(0, retries);
    for (int tentativa = 0; tentativa <= tentativas; ++tentativa) {
      if (cluster_barreira(dir, rank, world, barrier_round, timeout_sec, error)) return true;
      if (tentativa < tentativas) error.clear();
    }
    return false;
  };
  if (!barreira_com_retentativa(barrier_base)) return false;
  const auto aggregate_path = std::filesystem::path(dir) / (prefix + "-reduced.bin");
  if (rank == 0) {
    std::vector<double> sum(local.size(), 0.0);
    for (int r = 0; r < world; ++r) {
      const auto path = std::filesystem::path(dir) /
                        (prefix + "-rank-" + std::to_string(r) + ".bin");
      std::ifstream input(path);
      std::size_t count = 0;
      if (!input || !(input >> count) || count != local.size()) {
        error = "all-reduce: vetor divergente ou arquivo ausente do rank " + std::to_string(r);
        return false;
      }
      for (std::size_t k = 0; k < count; ++k) {
        double value = 0.0;
        if (!(input >> value)) {
          error = "all-reduce: arquivo truncado do rank " + std::to_string(r);
          return false;
        }
        sum[k] += value;
      }
    }
    std::ostringstream output;
    output << local.size() << "\n" << std::setprecision(9);
    for (double value : sum) output << static_cast<float>(value / world) << "\n";
    if (!publicar_texto_atomico(aggregate_path, output.str(), error)) return false;
  }
  if (!cluster_aguardar(aggregate_path.string(), timeout_sec, error)) return false;
  std::ifstream input(aggregate_path);
  std::size_t count = 0;
  if (!input || !(input >> count) || count != local.size()) {
    error = "all-reduce: resultado agregado invalido";
    return false;
  }
  reduced.resize(count);
  for (std::size_t k = 0; k < count; ++k) {
    if (!(input >> reduced[k])) {
      error = "all-reduce: resultado agregado truncado";
      return false;
    }
  }
  if (!barreira_com_retentativa(barrier_base + 1)) return false;
  // O resultado ja foi consumido por todos os ranks. Remover os artefatos de
  // cada passo evita que um treino longo acumule milhares de arquivos; em
  // caso de falha, a limpeza nao ocorre e os arquivos ajudam no diagnostico.
  std::error_code ignored;
  std::filesystem::remove(local_path, ignored);
  if (rank == 0) std::filesystem::remove(aggregate_path, ignored);
  // Os marcadores de barreira ficam no diretório: removê-los aqui permitiria
  // que um rank rápido apagasse o marcador antes de um rank lento terminar de
  // observá-lo. Os arquivos são pequenos e podem ser limpos junto ao diretório
  // do job; os payloads grandes já foram removidos acima.
  return true;
}

}  // namespace tilt::rt
