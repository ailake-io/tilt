#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tilt::rt {

bool cluster_preparar(const std::string& dir, std::string& error);
bool cluster_barreira(const std::string& dir, int rank, int world, std::int64_t round,
                      int timeout_sec, std::string& error);
bool cluster_aguardar(const std::string& path, int timeout_sec, std::string& error);
bool cluster_heartbeat(const std::string& dir, int rank, int world, std::int64_t round,
                       std::string& error);
bool cluster_allreduce(const std::string& dir, int rank, int world, std::int64_t round,
                       const std::vector<float>& local, std::vector<float>& reduced,
                       int timeout_sec, std::string& error, int retries = 0);

}  // namespace tilt::rt
