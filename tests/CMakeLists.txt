if(NOT WIN32)
add_test(
  NAME sharding
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sharding_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(
  NAME cluster_train
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cluster_train_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(
  NAME sql_pushdown
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sql_pushdown_test.sh
          $<TARGET_FILE:tilt>
)

add_test(
  NAME thread_pool
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/thread_pool_test.sh
          ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(
  NAME tensor_benchmark
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tensor_benchmark_test.sh
          ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(
  NAME tensor_pool
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tensor_pool_test.sh
          ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(
  NAME golden
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/golden)

add_test(
  NAME docs
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/docs_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_test(
  NAME llm_robusto
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/llm_retry_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/llm_robusto.tilt)

add_test(
  NAME pipeline_ops
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pipeline_ops_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_test(
  NAME pipeline_callbacks
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pipeline_callbacks_test.sh
          $<TARGET_FILE:tilt>)

add_test(
  NAME table_maintenance
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/table_maintenance_test.sh
          $<TARGET_FILE:tilt>)

add_test(
  NAME servico_ops
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/servico_ops_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ops_servico.tilt)

add_test(
  NAME servico_entrada
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/servico_entrada_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico_entrada.tilt)

add_test(
  NAME servico_allowlist
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/servico_allowlist_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico_allowlist.tilt)

add_test(
  NAME servico_versao
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/servico_versao_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico_versao.tilt)

add_test(
  NAME servico_shutdown
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/servico_shutdown_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ops_servico.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico_lento.tilt)

add_test(
  NAME http
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/http_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico_meio.tilt)

add_test(
  NAME par
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/par_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico_lento.tilt)

add_test(
  NAME s3
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/s3_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/s3_roundtrip.tilt)

add_test(
  NAME http_client
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/http_client_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/http_generico.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rede_roundtrip.tilt)

add_test(
  NAME mlflow
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mlflow_test.sh
          $<TARGET_FILE:tilt>)
add_test(
  NAME kafka
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/kafka_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/kafka_roundtrip.tilt)
add_test(
  NAME kafka_txn
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/kafka_txn_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/kafka_transacao.tilt)


add_test(
  NAME redis
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/redis_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/redis_roundtrip.tilt)

add_test(
  NAME mongo
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mongo_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mongo_roundtrip.tilt)

add_test(
  NAME mongo_aggr
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mongo_aggr_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mongo_aggr.tilt)

add_test(
  NAME pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pg_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pg_roundtrip.tilt)

add_test(
  NAME sqlite
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sqlite_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sqlite_params.tilt)

add_test(
  NAME sql_pool
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sql_pool_test.sh)

add_test(
  NAME tiltc
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tiltc_test.sh
          $<TARGET_FILE:tilt>)

add_test(
  NAME schema_widen
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/schema_widen_test.sh
          $<TARGET_FILE:tilt>)

add_test(
  NAME quote
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/quote_test.sh)

add_test(
  NAME dlopen_path
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dlopen_path_test.sh)

add_test(
  NAME update_script
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/update_script_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_test(
  NAME parquet
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/parquet_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/parquet_interop.tilt)

add_test(
  NAME avro
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/avro_test.sh
          $<TARGET_FILE:tilt>)

add_test(
  NAME native
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/native_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nativo.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nativo2.tilt)

add_test(
  NAME native_arm64
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/native_arm64_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nativo2.tilt)

add_test(
  NAME jit
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/jit_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_test(
  NAME iceberg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/iceberg_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_roundtrip.tilt)

add_test(
  NAME delta
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/delta_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/delta_particionado.tilt)

add_test(
  NAME iceberg_rest
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/iceberg_rest_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_roundtrip.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_rest_evolucao.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_rest_fantasma.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_rest_sobrescrita.tilt)

add_test(
  NAME iceberg_catalog
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/iceberg_catalog_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_catalogo.tilt)

add_test(
  NAME spark_catalog
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/spark_catalog_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/iceberg_catalogo.tilt)

add_test(
  NAME spark
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/spark_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/spark_interop.tilt)

add_test(
  NAME livy
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/livy_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/livy_spark.tilt)

add_test(
  NAME duckdb
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/duckdb_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/duckdb_roundtrip.tilt)

add_test(
  NAME mysql
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mysql_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mysql_roundtrip.tilt)

add_test(
  NAME clickhouse
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/clickhouse_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/clickhouse_roundtrip.tilt)

add_test(
  NAME elasticsearch
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/elasticsearch_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/elasticsearch_roundtrip.tilt)

add_test(
  NAME janela_offset
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/janela_offset_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/janela_offset)

add_test(
  NAME cursor_backfill
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cursor_backfill_test.sh
          $<TARGET_FILE:tilt>)

add_test(
  NAME leader_checkpoint
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/leader_checkpoint_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/janela_offset)

add_test(
  NAME checkpoint_backend
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/checkpoint_backend_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/janela_offset)

add_test(
  NAME tls
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tls_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/redis_tls.tilt)

add_test(
  NAME weaviate
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/weaviate_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/weaviate_rag.tilt)

add_test(
  NAME pinecone
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/pinecone_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pinecone_rag.tilt)

add_test(
  NAME chroma
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/chroma_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/chroma_rag.tilt)

add_test(
  NAME vector_real
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/vector_connectors_real_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set_tests_properties(vector_real PROPERTIES TIMEOUT 180)

find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME lsp COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/lsp_test.py $<TARGET_FILE:tilt>)
endif()
else()
  find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
  if(NOT POWERSHELL_EXECUTABLE)
    message(FATAL_ERROR "PowerShell e necessario para a suite CTest do Windows")
  endif()
  add_test(
    NAME windows_functional
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -NonInteractive
            -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/windows_functional.ps1
            -Bin $<TARGET_FILE:tilt>)
  set_tests_properties(windows_functional PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    TIMEOUT 180)
endif()
