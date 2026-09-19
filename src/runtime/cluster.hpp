#pragma once

#include <string>

namespace tilt::rt {

bool cluster_preparar(const std::string& dir, std::string& error);
bool cluster_barreira(const std::string& dir, int rank, int world, int round,
                      int timeout_sec, std::string& error);
bool cluster_aguardar(const std::string& path, int timeout_sec, std::string& error);

}  // namespace tilt::rt
