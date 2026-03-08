cmake_minimum_required(VERSION 3.16)

project(prono VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────
# C++ Standard
# ─────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────
# Build type
# ─────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ─────────────────────────────────────────
# Output directories
# ─────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ─────────────────────────────────────────
# Source files
# Automatically collect all source files
# ─────────────────────────────────────────
file(GLOB_RECURSE PRONO_SOURCES
    src/*.cpp
)

file(GLOB_RECURSE PRONO_HEADERS
    src/*.hpp
    src/*.h
)

# ─────────────────────────────────────────
# Executable
# ─────────────────────────────────────────
add_executable(prono
    ${PRONO_SOURCES}
    ${PRONO_HEADERS}
)

target_include_directories(prono
    PRIVATE
    src
)

# ─────────────────────────────────────────
# Compiler warnings
# ─────────────────────────────────────────
if(MSVC)
    target_compile_options(prono PRIVATE /W4)
else()
    target_compile_options(prono PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ─────────────────────────────────────────
# Install rules
# ─────────────────────────────────────────
install(TARGETS prono
    RUNTIME DESTINATION bin
)

# ─────────────────────────────────────────
# Packaging (optional)
# ─────────────────────────────────────────
include(CPack)
