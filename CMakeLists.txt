cmake_minimum_required(VERSION 3.18)
project(mpclient CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(ANDROID)
  # libmpclient.so: drop-in replacement for the loader's multiplayer client
  add_library(mpclient SHARED src/client.cpp src/client_game.cpp src/android_hook.cpp)
  target_compile_options(mpclient PRIVATE -O2 -fno-exceptions -fvisibility=hidden -ffunction-sections -fdata-sections)
  target_link_options(mpclient PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--build-id=none)
  target_link_libraries(mpclient log dl)
else()
  # host build: end-to-end test against the node server with a simulated game world
  find_package(Threads REQUIRED)
  add_executable(host_test test/host_test.cpp src/client.cpp src/client_game.cpp)
  target_link_libraries(host_test Threads::Threads)
endif()
