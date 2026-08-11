cmake_minimum_required(VERSION 3.16)
project(BackupVerifier LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_definitions(/DNOMINMAX)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_definitions(-D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE)

option(BUILD_GUI "Build SDL3 GUI (requires SDL3; Phase 2+)" OFF)
option(BUILD_TESTS "Build unit tests (Phase 1)" ON)
option(BUILD_TESTGEN "Build test tree generator tool" ON)

add_subdirectory(src)

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
