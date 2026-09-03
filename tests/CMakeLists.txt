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

find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME lsp COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/lsp_test.py $<TARGET_FILE:tilt>)
endif()
