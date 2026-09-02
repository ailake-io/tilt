add_test(
  NAME golden
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_golden.sh
          $<TARGET_FILE:tilt>
          ${CMAKE_CURRENT_SOURCE_DIR}/golden)
