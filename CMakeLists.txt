cmake_minimum_required(VERSION 3.28)
project(pixet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/index)
add_subdirectory(src/app)
add_subdirectory(tests)
