add_test(
  NAME golden
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/golden)

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
  NAME kafka
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/kafka_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/kafka_roundtrip.tilt)

add_test(
  NAME native
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/native_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nativo.tilt
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nativo2.tilt)

find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME lsp COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/lsp_test.py $<TARGET_FILE:tilt>)
endif()
