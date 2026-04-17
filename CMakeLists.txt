cmake_minimum_required(VERSION 3.16)
project(nanots_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build type defaults
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /WX- /MP)
    # Disable MSVC warnings that fire in SQLite amalgamation
    add_compile_options(/wd4127 /wd4244 /wd4267 /wd4996)
else()
    add_compile_options(
        -Wall -Wextra -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Debug>:-g -O0>
    )
endif()

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# SQLite amalgamation (vendored, pinned)
# ---------------------------------------------------------------------------
# SQLite source lives in third_party/sqlite/.
# To update: download the amalgamation from https://sqlite.org/download.html
# and replace sqlite3.c / sqlite3.h.  Record the version in third_party/sqlite/VERSION.
add_library(sqlite3 STATIC
    third_party/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1          # serialized mode — safe for our usage pattern
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_OMIT_LOAD_EXTENSION=1
)
# SQLite amalgamation is C; suppress C++ warnings when it's compiled as C.
set_target_properties(sqlite3 PROPERTIES LANGUAGE C)
if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w)   # suppress all warnings in vendored code
endif()

# ---------------------------------------------------------------------------
# nanots (git submodule at third_party/nanots)
#
# Build strategy: compile ONLY amalgamated_src/nanots.cpp — do NOT compile
# amalgamated_src/sqlite3.c.  nanots.cpp is linked against our already-vendored
# sqlite3 target (3.45.3).  nanots.h includes sqlite3.h from the amalgamated_src
# directory (which declares the 3.38.5 API), but the 3.45.3 implementation is
# fully backward-compatible for every SQLite function nanots uses.  This avoids
# duplicate sqlite3_* symbol definitions that would cause LNK2005 on MSVC if
# we compiled nanots's bundled sqlite3.c alongside ours.
# ---------------------------------------------------------------------------
add_library(nanots_lib STATIC
    third_party/nanots/amalgamated_src/nanots.cpp
)
target_include_directories(nanots_lib PUBLIC
    third_party/nanots/amalgamated_src   # provides nanots.h (which includes sqlite3.h 3.38.5)
    third_party/nanots                   # provides nanots.h and utils.h for the non-amalgamated path
)
target_link_libraries(nanots_lib PUBLIC sqlite3 Threads::Threads)

# Suppress warnings from vendored nanots source.
if(MSVC)
    target_compile_options(nanots_lib PRIVATE /W0)
else()
    target_compile_options(nanots_lib PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# RocksDB (FetchContent)
#
# Minimal build: no compression, no tools, no tests, no gflags.
# FAIL_ON_WARNINGS=OFF is required for MSVC — RocksDB triggers several
# warnings that would otherwise break our /WX- build.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(rocksdb
    GIT_REPOSITORY https://github.com/facebook/rocksdb.git
    GIT_TAG        v9.7.3
    GIT_SHALLOW    TRUE
)
set(WITH_TESTS            OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS  OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS            OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS           OFF CACHE BOOL "" FORCE)
set(WITH_SNAPPY           OFF CACHE BOOL "" FORCE)
set(WITH_LZ4              OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB             OFF CACHE BOOL "" FORCE)
set(WITH_ZSTD             OFF CACHE BOOL "" FORCE)
set(WITH_BZ2              OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS      OFF CACHE BOOL "" FORCE)
set(USE_RTTI              ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rocksdb)

# ---------------------------------------------------------------------------
# bench_lib — OBJECT library so all TUs (including self-registering backends
# and workloads) are always linked.  Using STATIC would let the linker
# dead-strip TUs whose symbols are never directly referenced from main.cpp,
# silently emptying the registries.
# ---------------------------------------------------------------------------
add_library(bench_lib OBJECT
    src/timing.cpp
    src/result.cpp
    src/runner.cpp
    src/registry.cpp

    # Backends
    src/backends/sqlite_backend.cpp
    src/backends/nanots_backend.cpp
    src/backends/rocksdb_backend.cpp

    # Workloads
    src/workloads/sustained_write.cpp
    src/workloads/random_seek.cpp
    src/workloads/concurrent_readers.cpp
    src/workloads/multi_stream_write.cpp
    src/workloads/write_read_contention.cpp
)

target_include_directories(bench_lib PUBLIC include)
target_link_libraries(bench_lib PUBLIC sqlite3 nanots_lib rocksdb Threads::Threads)

# ---------------------------------------------------------------------------
# bench executable
# ---------------------------------------------------------------------------
add_executable(bench src/main.cpp)
target_link_libraries(bench PRIVATE bench_lib nanots_lib rocksdb sqlite3 Threads::Threads)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS bench RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# How to add a new backend (e.g. DuckDB):
#
# Option A — FetchContent:
#   include(FetchContent)
#   FetchContent_Declare(duckdb
#       GIT_REPOSITORY https://github.com/duckdb/duckdb.git
#       GIT_TAG        v0.10.0    # pin the version
#   )
#   FetchContent_MakeAvailable(duckdb)
#   target_link_libraries(bench_lib PUBLIC duckdb)
#
# Option B — git submodule:
#   git submodule add https://github.com/duckdb/duckdb third_party/duckdb
#   add_subdirectory(third_party/duckdb)
#   target_link_libraries(bench_lib PUBLIC duckdb)
#
# Then:
#   1. Write src/backends/duckdb_backend.cpp implementing bench::Backend.
#   2. Add it to the bench_lib sources list above.
#   3. Done — no changes needed in main.cpp, runner.cpp, or any workload.
# ---------------------------------------------------------------------------
