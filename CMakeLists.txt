cmake_minimum_required(VERSION 3.16)
project(LimitOrderBook VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# The server has no third-party dependencies: the HTTP/WebSocket layer is
# implemented on POSIX sockets, so there is nothing to fetch and the build
# works offline on any Linux/macOS machine with a C++20 compiler.
add_library(lob_core STATIC
  src/OrderBook.cpp
  src/MatchingEngine.cpp
  src/MarketMaker.cpp
  src/util/Json.cpp
  src/net/Crypto.cpp
  src/net/WebServer.cpp
  src/app/Gateway.cpp
)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_core PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lob_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(lob_server src/main.cpp)
target_link_libraries(lob_server PRIVATE lob_core)
target_compile_definitions(lob_server PRIVATE LOB_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web")

option(LOB_BUILD_TESTS "Build unit tests" ON)
if(LOB_BUILD_TESTS)
  enable_testing()
  add_executable(lob_tests tests/test_main.cpp)
  target_link_libraries(lob_tests PRIVATE lob_core)
  add_test(NAME lob_tests COMMAND lob_tests)
endif()
