#pragma once

#include <string>
#include <vector>

namespace tilt::rt {

// Servidor Iceberg REST Open API read-only (fase 30): expoe as tabelas
// Iceberg locais (formato Hadoop, escritas pela tilt — ver iceberg.hpp) para
// engines como Spark SQL configurarem um SparkCatalog tipo "rest" com a URI
// apontando para este servidor. Reaproveita o HttpServer epoll do `tilt
// servir` (runtime/http_server.hpp).
//
// Subconjunto v1 implementado (prefixo configuravel, default "/v1"):
//   GET  <prefixo>/config                          -> defaults/overrides vazios
//   GET  <prefixo>/namespaces                      -> [["default"]]
//   GET  <prefixo>/namespaces/default              -> namespace + properties
//   GET  <prefixo>/namespaces/default/tables       -> lista de tabelas
//   GET  <prefixo>/namespaces/default/tables/<nome> -> loadTable (metadata com
//                                                     locations reescritas)
//   GET/HEAD <prefixo>/files/<rel-ao-root>           -> bytes do arquivo
//       (metadata.json, manifest .avro, data .parquet) sob o root. Forma
//       path-style de proposito: o Hadoop Path (cliente Spark) re-encodea
//       query strings. A forma legada ?path=<abs> tambem e aceita.
// Escrita (createTable/commit), HEAD de tabela e demais rotas: 501/404 com
// erro claro — o catalogo e read-only na 1a passada.
//
// Tabela Iceberg = subdiretorio direto do root que contem metadata/. O
// loadTable resolve o v<N>.metadata.json mais recente (mesma regra do modo
// Hadoop: maior versao parseada do nome, empate lexicografico) e reescreve
// as locations file:// (ou caminho absoluto) dentro do root para URLs
// http(s) deste servidor, usando o header Host da requisicao — assim o
// Spark resolve os mesmos host:porta que usou no loadTable. A reescrita das
// manifest-lists (default ligada) pode ser desligada para engines que leem o
// manifest list pelo FileSystem do Hadoop (ver IcebergCatalogConfig).
struct IcebergCatalogConfig {
  std::string root;            // diretorio-raiz das tabelas (absoluto)
  std::string host = "0.0.0.0";  // endereco de escuta
  int port = 8191;
  std::string prefix = "/v1";  // prefixo do Iceberg REST Open API
  int threads = 0;             // 0 = padrao (min(4, cores)); handler e
                               // thread-safe (estado so de leitura)
  // Reescreve snapshots[].manifest-list para URLs deste servidor (default).
  // Desligue (--sem-reecrita-manifests) para engines que leem o manifest list
  // pelo FileSystem do Hadoop (ex.: Spark sem S3): o fs.http do Hadoop reporta
  // length -1 e o leitor Avro do Iceberg rejeita length < 4 sem ler o arquivo
  // ("Not an Avro data file") — nesses casos o caller precisa acessar os
  // file:// locais (ex.: montando o diretorio no mesmo path).
  bool reescrever_manifests = true;
};

// Nomes das tabelas Iceberg sob root: subdiretorios com metadata/, ordenados.
std::vector<std::string> iceberg_catalog_tables(const std::string& root);

// Sobe o servidor e bloqueia ate encerrar. Retorna 0 ok, 1 em erro fatal
// (mensagem em stderr).
int iceberg_catalog_serve(const IcebergCatalogConfig& cfg);

}  // namespace tilt::rt
