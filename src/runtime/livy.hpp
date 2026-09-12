#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente do Apache Livy (REST/JSON sobre o http_client generico do runtime):
// ponte zero-link para Spark SQL e para executar pipelines num cluster Spark.
// Nao fecha a sessao apos o uso — sessions sao caras de criar (o driver sobe
// do zero), entao o cliente lista GET /sessions e reutiliza a primeira sessao
// idle com o kind correspondente ("spark" para scala, "pyspark" para python);
// so cria uma nova (POST /sessions com {kind, conf}) quando nao ha nenhuma
// idle. O `conf` (mapa, ex.: {"spark.jars.packages": ..., "spark.master": ...})
// so vale na criacao. Statements (POST /sessions/{id}/statements) tem polling
// de estado ate available/error com timeout de ~120s.
//
// Erros vêm como std::runtime_error com prefixo "livy: " (mensagem do servidor
// ou do Livy: ename/evalue do statement em error), capturavel no tilt com
// tentar/capturar.
//
// livy_sql envolve o SQL num codigo que devolve string JSON
// (Scala: spark.sql("""...""").toJSON.collectAsList().toString; PySpark:
// __import__("json").dumps(...) sobre toJSON().collect()) e parseia o
// data.text/plain da resposta em tabela (lista de objetos -> linhas de mapa).
// livy_executar envia o codigo verbatim e devolve o texto do output.
Value livy_sql(const std::string& url, const std::string& codigo_sql, const std::string& lingua,
               const Value* conf);
Value livy_executar(const std::string& url, const std::string& codigo, const std::string& lingua,
                    const Value* conf);

}  // namespace tilt::rt
