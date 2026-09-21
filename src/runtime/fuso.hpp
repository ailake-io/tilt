// Conversao de fusos horarios para limpeza de dados (`converter_fuso`).
#pragma once

#include <string>

namespace tilt::rt {

// Converte `texto` (data e hora ISO, `AAAA-MM-DD[T ]HH:MM[:SS]`, ou os formatos de
// `normalizar_data`) do fuso `origem` para `destino`; devolve `AAAA-MM-DDTHH:MM:SS`.
// Um sufixo `Z`/`+HH:MM` no proprio texto vale mais que `origem`. Fusos: `UTC`,
// deslocamento fixo (`-03:00`, `+0530`) ou nome IANA (`America/Sao_Paulo`; POSIX, usa o
// tzdata do sistema; no Windows so UTC e deslocamentos). Lanca std::runtime_error para
// fuso desconhecido; devolve "" se o texto nao e uma data/hora valida.
std::string converter_fuso(const std::string& texto, const std::string& origem,
                           const std::string& destino);

}  // namespace tilt::rt
