cmake_minimum_required(VERSION 3.24)

project(TOTKRecomp VERSION 0.1.0 LANGUAGES C CXX)

option(TOTKRECOMP_BUILD_TESTS "Build the project test suite" ON)
option(TOTKRECOMP_ENABLE_SANITIZERS "Enable supported address/undefined sanitizers" OFF)
option(TOTKRECOMP_ENABLE_LLVM "Enable the optional LLVM 18 lowering/JIT backend" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

function(totkrecomp_enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

function(totkrecomp_enable_sanitizers target)
    if(NOT TOTKRECOMP_ENABLE_SANITIZERS)
        return()
    endif()
    if(MSVC)
        message(WARNING "TOTKRECOMP_ENABLE_SANITIZERS is currently supported on GCC/Clang only")
        return()
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(lz4 CONFIG QUIET)
if(NOT lz4_FOUND)
    set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(lz4 URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(lz4)
endif()

find_package(Capstone CONFIG QUIET)
if(NOT Capstone_FOUND AND NOT TARGET capstone::capstone AND NOT TARGET capstone)
    set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(capstone URL https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.3.tar.gz)
    FetchContent_MakeAvailable(capstone)
endif()

if(TOTKRECOMP_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        FetchContent_Declare(Catch2 URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz)
        FetchContent_MakeAvailable(Catch2)
    endif()
    enable_testing()
endif()

add_library(switchrecomp-common
    src/switchrecomp/common/error.cpp
    src/switchrecomp/common/logging.cpp
    src/switchrecomp/common/sha256.cpp
)
target_include_directories(switchrecomp-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
totkrecomp_enable_project_warnings(switchrecomp-common)
totkrecomp_enable_sanitizers(switchrecomp-common)

add_library(switchrecomp-format
    src/switchrecomp/format/nso0.cpp
    src/switchrecomp/format/nso_image.cpp
    src/switchrecomp/format/mod0.cpp
    src/switchrecomp/format/dynamic.cpp
)
target_include_directories(switchrecomp-format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-format PUBLIC switchrecomp-common lz4_static)
totkrecomp_enable_project_warnings(switchrecomp-format)
totkrecomp_enable_sanitizers(switchrecomp-format)

add_library(switchrecomp-memory
    src/switchrecomp/memory/guest_memory.cpp
    src/switchrecomp/memory/guest_loader.cpp
    src/switchrecomp/memory/relocation.cpp
    src/switchrecomp/memory/symbol_resolver.cpp
)
target_include_directories(switchrecomp-memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-memory PUBLIC switchrecomp-common switchrecomp-format)
totkrecomp_enable_project_warnings(switchrecomp-memory)
totkrecomp_enable_sanitizers(switchrecomp-memory)

add_library(switchrecomp-target
    src/switchrecomp/target/manifest.cpp
)
target_include_directories(switchrecomp-target PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-target PUBLIC switchrecomp-common switchrecomp-memory nlohmann_json::nlohmann_json)
totkrecomp_enable_project_warnings(switchrecomp-target)
totkrecomp_enable_sanitizers(switchrecomp-target)

add_library(switchrecomp-aarch64
    src/switchrecomp/aarch64/register.cpp
    src/switchrecomp/aarch64/instruction.cpp
    src/switchrecomp/aarch64/decoder.cpp
)
target_include_directories(switchrecomp-aarch64 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(TARGET capstone::capstone)
    target_link_libraries(switchrecomp-aarch64 PUBLIC switchrecomp-common switchrecomp-memory capstone::capstone)
else()
    target_link_libraries(switchrecomp-aarch64 PUBLIC switchrecomp-common switchrecomp-memory capstone)
endif()
totkrecomp_enable_project_warnings(switchrecomp-aarch64)
totkrecomp_enable_sanitizers(switchrecomp-aarch64)

add_library(switchrecomp-analysis
    src/switchrecomp/analysis/control_flow_graph.cpp
    src/switchrecomp/analysis/cfg_analyzer.cpp
    src/switchrecomp/analysis/coverage.cpp
)
target_include_directories(switchrecomp-analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-analysis PUBLIC switchrecomp-common switchrecomp-memory switchrecomp-aarch64)
totkrecomp_enable_project_warnings(switchrecomp-analysis)
totkrecomp_enable_sanitizers(switchrecomp-analysis)

add_library(switchrecomp-ir
    src/switchrecomp/ir/type.cpp
    src/switchrecomp/ir/opcode.cpp
    src/switchrecomp/ir/register.cpp
    src/switchrecomp/ir/function.cpp
    src/switchrecomp/ir/builder.cpp
    src/switchrecomp/ir/verifier.cpp
    src/switchrecomp/ir/printer.cpp
)
target_include_directories(switchrecomp-ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-ir PUBLIC switchrecomp-common)
totkrecomp_enable_project_warnings(switchrecomp-ir)
totkrecomp_enable_sanitizers(switchrecomp-ir)

add_library(switchrecomp-runtime-core
    src/switchrecomp/runtime/cpu_state.cpp
    src/switchrecomp/runtime/context.cpp
    src/switchrecomp/runtime/fp.cpp
    src/switchrecomp/runtime/atomic_memory.cpp
    src/switchrecomp/runtime/thread.cpp
)
target_include_directories(switchrecomp-runtime-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-runtime-core PUBLIC switchrecomp-common switchrecomp-memory switchrecomp-ir)
totkrecomp_enable_project_warnings(switchrecomp-runtime-core)
totkrecomp_enable_sanitizers(switchrecomp-runtime-core)

add_library(switchrecomp-lifter src/switchrecomp/lifter/lifter.cpp)
target_include_directories(switchrecomp-lifter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-lifter PUBLIC switchrecomp-common switchrecomp-aarch64 switchrecomp-analysis switchrecomp-ir)
totkrecomp_enable_project_warnings(switchrecomp-lifter)
totkrecomp_enable_sanitizers(switchrecomp-lifter)

add_library(switchrecomp-interpreter src/switchrecomp/interpreter/interpreter.cpp)
target_include_directories(switchrecomp-interpreter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(switchrecomp-interpreter PUBLIC switchrecomp-common switchrecomp-ir switchrecomp-runtime-core)
totkrecomp_enable_project_warnings(switchrecomp-interpreter)
totkrecomp_enable_sanitizers(switchrecomp-interpreter)

set(TOTKRECOMP_HAS_LLVM OFF)
if(TOTKRECOMP_ENABLE_LLVM)
    find_package(LLVM 18.1.3 CONFIG QUIET)
    if(LLVM_FOUND)
        set(TOTKRECOMP_HAS_LLVM ON)
        add_library(switchrecomp-codegen-llvm src/switchrecomp/codegen/llvm_backend.cpp)
        target_include_directories(switchrecomp-codegen-llvm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_include_directories(switchrecomp-codegen-llvm SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
        target_compile_options(switchrecomp-codegen-llvm PRIVATE ${LLVM_DEFINITIONS})
        target_link_libraries(switchrecomp-codegen-llvm PUBLIC switchrecomp-common switchrecomp-ir switchrecomp-runtime-core)
        if(COMMAND llvm_map_components_to_libnames)
            llvm_map_components_to_libnames(TOTKRECOMP_LLVM_LIBS core orcjit native support)
            target_link_libraries(switchrecomp-codegen-llvm PRIVATE ${TOTKRECOMP_LLVM_LIBS})
        elseif(TARGET LLVM)
            target_link_libraries(switchrecomp-codegen-llvm PRIVATE LLVM)
        endif()
        totkrecomp_enable_project_warnings(switchrecomp-codegen-llvm)
        totkrecomp_enable_sanitizers(switchrecomp-codegen-llvm)
    else()
        message(WARNING "TOTKRECOMP_ENABLE_LLVM=ON but LLVM was not found; LLVM backend is disabled")
    endif()
endif()

add_executable(nso-inspect tools/nso-inspect/main.cpp)
target_link_libraries(nso-inspect PRIVATE switchrecomp-memory)
target_compile_definitions(nso-inspect PRIVATE SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev")
totkrecomp_enable_project_warnings(nso-inspect)
totkrecomp_enable_sanitizers(nso-inspect)

add_executable(aarch64-analyze tools/aarch64-analyze/main.cpp)
target_link_libraries(aarch64-analyze PRIVATE switchrecomp-analysis)
target_compile_definitions(aarch64-analyze PRIVATE SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev")
totkrecomp_enable_project_warnings(aarch64-analyze)
totkrecomp_enable_sanitizers(aarch64-analyze)

add_executable(aarch64-lift tools/aarch64-lift/main.cpp)
target_link_libraries(aarch64-lift PRIVATE switchrecomp-analysis switchrecomp-ir switchrecomp-lifter switchrecomp-interpreter)
if(TOTKRECOMP_HAS_LLVM)
    target_link_libraries(aarch64-lift PRIVATE switchrecomp-codegen-llvm)
    target_compile_definitions(aarch64-lift PRIVATE TOTKRECOMP_HAS_LLVM=1)
endif()
target_compile_definitions(aarch64-lift PRIVATE SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev")
totkrecomp_enable_project_warnings(aarch64-lift)
totkrecomp_enable_sanitizers(aarch64-lift)

add_executable(nso-dynamic-inspect tools/nso-dynamic-inspect/main.cpp)
target_link_libraries(nso-dynamic-inspect PRIVATE switchrecomp-memory nlohmann_json::nlohmann_json)
target_compile_definitions(nso-dynamic-inspect PRIVATE SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev")
totkrecomp_enable_project_warnings(nso-dynamic-inspect)
totkrecomp_enable_sanitizers(nso-dynamic-inspect)

if(TOTKRECOMP_BUILD_TESTS)
    add_executable(switchrecomp-tests
        tests/common_tests.cpp
        tests/format_tests.cpp
        tests/guest_memory_tests.cpp
        tests/nso_tests.cpp
        tests/nso_materialization_tests.cpp
        tests/mod0_dynamic_tests.cpp
        tests/target_tests.cpp
        tests/aarch64_decoder_tests.cpp
        tests/cfg_analyzer_tests.cpp
        tests/relocation_symbol_tests.cpp
        tests/semantic_ir_tests.cpp
        tests/lifter_interpreter_tests.cpp
        tests/milestone7_semantics_tests.cpp
        tests/milestone8_fp_simd_tests.cpp
        tests/milestone9_threads_atomics_tests.cpp
    )
    target_link_libraries(switchrecomp-tests PRIVATE switchrecomp-common switchrecomp-format switchrecomp-memory switchrecomp-aarch64 switchrecomp-analysis switchrecomp-ir switchrecomp-runtime-core switchrecomp-lifter switchrecomp-interpreter switchrecomp-target lz4_static Catch2::Catch2WithMain)
    totkrecomp_enable_project_warnings(switchrecomp-tests)
    totkrecomp_enable_sanitizers(switchrecomp-tests)
    if(TOTKRECOMP_HAS_LLVM)
        target_sources(switchrecomp-tests PRIVATE tests/llvm_backend_tests.cpp)
        target_link_libraries(switchrecomp-tests PRIVATE switchrecomp-codegen-llvm)
        target_compile_definitions(switchrecomp-tests PRIVATE TOTKRECOMP_HAS_LLVM=1)
    endif()
    include(Catch)
    catch_discover_tests(switchrecomp-tests)
endif()

install(TARGETS nso-inspect nso-dynamic-inspect aarch64-analyze aarch64-lift RUNTIME DESTINATION bin)
