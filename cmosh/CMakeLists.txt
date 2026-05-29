add_library(cmosh_core STATIC
  cmosh_aes.c
  cmosh_base64.c
  cmosh_bootstrap.c
  cmosh_fragment.c
  cmosh_ocb.c
  cmosh_proto.c
  cmosh_stubs.c
  cmosh_transport.c)

target_include_directories(cmosh_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(cmosh_core PRIVATE ${CMAKE_SOURCE_DIR}/ssh/zlib.c)
target_link_libraries(cmosh_core utils)

add_executable(cmosh
  cmosh_main.c
  cmosh_platform.c)

target_link_libraries(cmosh cmosh_core ${platform_libraries})
if(WIN32)
  target_link_libraries(cmosh ws2_32)
endif()

add_executable(test_cmosh
  cmosh_test.c)
target_link_libraries(test_cmosh cmosh_core ${platform_libraries})
