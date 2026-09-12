#pragma once

#include <string>

namespace tilt::rt {

// Eleicao de lider por lease em arquivo (Fase 12-4): garante que apenas uma
// replica executa o loop `--agendar` por vez em deploy multi-replica sobre
// filesystem compartilhado (NFS/EFS) — sem dependencia externa.
//
// Protocolo: `lease_path` contem `<owner> <expiry_epoch>`; acquire cria com
// O_CREAT|O_EXCL (vitoria imediata); se ja existe e o expiry passou, o
// chamador toma posse (takeover); caso contrario e follower. Renew reescreve
// o lease apenas se ainda somos o dono. TTL default 15s.
//
// Env: TILT_LEADER_LEASE=<path do lease>, TILT_LEADER_TTL=<segundos>.
bool leader_tentar(const std::string& lease_path, int ttl_seg, std::string& motivo);
void leader_renovar(const std::string& lease_path, int ttl_seg);
void leader_liberar(const std::string& lease_path);
std::string leader_dono();

}  // namespace tilt::rt
