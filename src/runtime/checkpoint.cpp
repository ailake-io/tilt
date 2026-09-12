#include "runtime/checkpoint.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace tilt::rt {

std::string checkpoint_resolve(const std::string& offset_local) {
  const char* dir = std::getenv("TILT_CHECKPOINT_DIR");
  if (!dir || !*dir) return offset_local;
  const std::string d = dir;
  if (d.rfind("s3://", 0) == 0 || d.rfind("kafka:", 0) == 0) {
    throw std::runtime_error(
        "checkpoint: backend '" + d +
        "' ainda nao suportado na Fase 12-4 (use TILT_CHECKPOINT_DIR=<dir compartilhado>); "
        "S3/Kafka ficam para a Fase 12-4b");
  }
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  const std::string base = std::filesystem::path(offset_local).filename().string();
  return (std::filesystem::path(d) / base).string();
}

}  // namespace tilt::rt
