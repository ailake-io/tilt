add_test(
  NAME golden
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/golden)

add_test(
  NAME http
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/http_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/servico.tilt)

add_test(
  NAME native
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/native_test.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nativo.tilt)
