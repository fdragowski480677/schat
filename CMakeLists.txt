cmake_minimum_required(VERSION 3.16)
project(schat VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(schat_core STATIC src/protocol.cpp src/net.cpp)
target_include_directories(schat_core PUBLIC src)
target_compile_options(schat_core PRIVATE -Wall -Wextra)

add_executable(schat src/main.cpp src/server.cpp src/client.cpp
                     src/discovery.cpp src/terminal.cpp)
target_link_libraries(schat PRIVATE schat_core)
target_compile_options(schat PRIVATE -Wall -Wextra)

install(TARGETS schat RUNTIME DESTINATION bin)

enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE schat_core)
add_test(NAME protocol COMMAND test_protocol)
add_test(NAME integration
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE:schat>)
