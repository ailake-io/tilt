#pragma once

#include <map>
#include <string>

#include "runtime/tensor.hpp"

namespace tilt::rt {

bool safetensors_salvar(const std::string& path, const std::map<std::string, Tensor>& tensores,
                        const std::map<std::string, std::string>& metadata, std::string& erro);
bool safetensors_carregar(const std::string& path, std::map<std::string, Tensor>& tensores,
                          std::map<std::string, std::string>& metadata, std::string& erro);

}  // namespace tilt::rt
