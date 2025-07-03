cmake_minimum_required(VERSION 3.13)

project(mini-redis CXX)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)

add_executable(server ${SOURCE_FILES})

target_link_libraries(server PRIVATE Threads::Threads)
