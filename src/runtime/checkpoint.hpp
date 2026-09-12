#pragma once

#include <string>

namespace tilt::rt {

// Checkpoint distribuido da janela de contagem (Fase 12-4): resolve onde o
// `.tilt-offset` realmente mora. Sem env, devolve o path local de sempre.
// Com TILT_CHECKPOINT_DIR=<dir compartilhado>, devolve
// `<dir>/<basename-do-offset>` — multiplas replicas sobre NFS/EFS passam a
// compartilhar o mesmo offset (com eleicao de lider garantindo escritor
// unico). URIs s3:// e kafka: sao reconhecidas e rejeitadas com erro claro
// (Fase 12-4b: backends S3/Kafka reaproveitando os conectores existentes).
std::string checkpoint_resolve(const std::string& offset_local);

}  // namespace tilt::rt
