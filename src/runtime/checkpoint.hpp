#pragma once

#include <string>

namespace tilt::rt {

// Checkpoint distribuido da janela (Marco 1): resolve onde o `.tilt-offset`
// realmente mora e le/grava por ele.
//
// - sem env: path local de sempre (`<fonte>.tilt-offset`);
// - TILT_CHECKPOINT_DIR=<dir compartilhado>: `<dir>/<basename>` (NFS/EFS,
//   multi-replica com eleicao de lider como escritor unico);
// - TILT_CHECKPOINT_DIR=s3://<bucket>/<prefixo>: objeto
//   `s3://<bucket>/<prefixo>/<basename>` (PUT/GET pelo cliente S3; o mapa
//   inteiro e regravado a cada save, como no arquivo);
// - TILT_CHECKPOINT_DIR=kafka:<topico>: o mapa inteiro vai como 1 record
//   por save no topico (bootstrap via KAFKA_BOOTSTRAP); a leitura junta os
//   records (last-wins) e extrai a chave do arquivo.
//
// S3 e Kafka reaproveitam os conectores existentes (curl / wire protocol).
std::string checkpoint_resolve(const std::string& offset_local);

// Le o conteudo bruto do checkpoint resolvido ("", vazio, quando ausente).
// Erro de transporte/autenticacao vira excecao.
std::string checkpoint_ler(const std::string& resolvido);

// Grava o conteudo bruto (substitui o anterior).
void checkpoint_gravar(const std::string& resolvido, const std::string& corpo);

}  // namespace tilt::rt
