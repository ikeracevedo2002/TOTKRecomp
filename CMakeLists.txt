cmake_minimum_required(VERSION 3.20)

project(TOTKRecomp VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

option(TOTKRECOMP_BUILD_TESTS "Build the TOTKRecomp test suite" ON)
option(TOTKRECOMP_ENABLE_WARNINGS "Enable warnings for project-owned targets" ON)
option(TOTKRECOMP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SWITCHRECOMP_ENABLE_LLVM "Enable the optional LLVM lowering/JIT backend" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

set(LZ4_BUILD_CLI OFF CACHE BOOL "Disable the LZ4 command-line tool" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "Disable the legacy LZ4 command-line tool" FORCE)
set(LZ4_BUNDLED_MODE ON CACHE BOOL "Build LZ4 as a bundled dependency" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build bundled dependencies as static libraries" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Build the bundled LZ4 static library" FORCE)
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG 5ff839680134437dbf4678f3d0c7b371d84f4964
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(lz4)
if(NOT TARGET lz4_static)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(${lz4_SOURCE_DIR}/build/cmake ${lz4_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

set(CAPSTONE_BUILD_SHARED_LIBS OFF CACHE BOOL "Build Capstone as a static library" FORCE)
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "Disable Capstone tests" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "Disable the Capstone command-line tool" FORCE)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "Disable unused Capstone architectures" FORCE)
set(CAPSTONE_ARM_SUPPORT OFF CACHE BOOL "Disable Capstone ARM support" FORCE)
set(CAPSTONE_ARM64_SUPPORT ON CACHE BOOL "Enable Capstone AArch64 support" FORCE)
set(CAPSTONE_M68K_SUPPORT OFF CACHE BOOL "Disable Capstone M68K support" FORCE)
set(CAPSTONE_MIPS_SUPPORT OFF CACHE BOOL "Disable Capstone MIPS support" FORCE)
set(CAPSTONE_PPC_SUPPORT OFF CACHE BOOL "Disable Capstone PPC support" FORCE)
set(CAPSTONE_SPARC_SUPPORT OFF CACHE BOOL "Disable Capstone SPARC support" FORCE)
set(CAPSTONE_SYSZ_SUPPORT OFF CACHE BOOL "Disable Capstone SystemZ support" FORCE)
set(CAPSTONE_XCORE_SUPPORT OFF CACHE BOOL "Disable Capstone XCore support" FORCE)
set(CAPSTONE_X86_SUPPORT OFF CACHE BOOL "Disable Capstone X86 support" FORCE)
set(CAPSTONE_TMS320C64X_SUPPORT OFF CACHE BOOL "Disable Capstone TMS320C64x support" FORCE)
set(CAPSTONE_M680X_SUPPORT OFF CACHE BOOL "Disable Capstone M680X support" FORCE)
set(CAPSTONE_EVM_SUPPORT OFF CACHE BOOL "Disable Capstone EVM support" FORCE)
set(CAPSTONE_WASM_SUPPORT OFF CACHE BOOL "Disable Capstone WebAssembly support" FORCE)
set(CAPSTONE_MOS65XX_SUPPORT OFF CACHE BOOL "Disable Capstone MOS65XX support" FORCE)
set(CAPSTONE_BPF_SUPPORT OFF CACHE BOOL "Disable Capstone BPF support" FORCE)
set(CAPSTONE_RISCV_SUPPORT OFF CACHE BOOL "Disable Capstone RISC-V support" FORCE)
set(CAPSTONE_SH_SUPPORT OFF CACHE BOOL "Disable Capstone SH support" FORCE)
set(CAPSTONE_TRICORE_SUPPORT OFF CACHE BOOL "Disable Capstone TriCore support" FORCE)
FetchContent_Declare(
    capstone
    GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
    GIT_TAG 5cca00533dadfe53181f1de3525f859769f69b65
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(capstone)

if(TOTKRECOMP_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.4
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

function(totkrecomp_enable_project_warnings target)
    if(NOT TOTKRECOMP_ENABLE_WARNINGS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
    endif()
endfunction()

function(totkrecomp_enable_sanitizers target)
    if(NOT TOTKRECOMP_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        message(WARNING "TOTKRECOMP_ENABLE_SANITIZERS is not supported by this project on MSVC")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(switchrecomp-common
    src/switchrecomp/common/error.cpp
    src/switchrecomp/common/binary_reader.cpp
    src/switchrecomp/common/logging.cpp
    src/switchrecomp/common/sha256.cpp
)
target_include_directories(switchrecomp-common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(switchrecomp-common
    PUBLIC SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev"
)
if(WIN32)
    target_link_libraries(switchrecomp-common PRIVATE bcrypt)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(switchrecomp-common PRIVATE OpenSSL::Crypto)
endif()
totkrecomp_enable_project_warnings(switchrecomp-common)
totkrecomp_enable_sanitizers(switchrecomp-common)

add_library(switchrecomp-target
    src/switchrecomp/target/file_validation.cpp
    src/switchrecomp/target/manifest.cpp
)
target_include_directories(switchrecomp-target
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-target
    PUBLIC switchrecomp-common nlohmann_json::nlohmann_json
)
totkrecomp_enable_project_warnings(switchrecomp-target)
totkrecomp_enable_sanitizers(switchrecomp-target)

add_library(switchrecomp-format
    src/switchrecomp/format/nso.cpp
    src/switchrecomp/format/nso_magic.cpp
)
target_include_directories(switchrecomp-format
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-format
    PUBLIC switchrecomp-common
    PRIVATE lz4_static
)
totkrecomp_enable_project_warnings(switchrecomp-format)
totkrecomp_enable_sanitizers(switchrecomp-format)

add_library(switchrecomp-memory
    src/switchrecomp/memory/guest_memory.cpp
    src/switchrecomp/loader/nso_guest_loader.cpp
    # Metadata parsers consume GuestMemory, so they live in this layer while
    # retaining their public format headers under include/.../format.
    src/switchrecomp/format/module_metadata.cpp
    src/switchrecomp/format/mod0.cpp
    src/switchrecomp/format/elf_dynamic.cpp
    src/switchrecomp/format/elf_rela.cpp
    src/switchrecomp/format/dynamic_symbols.cpp
    src/switchrecomp/loader/symbol_resolver.cpp
    src/switchrecomp/loader/relocation_processor.cpp
)
target_include_directories(switchrecomp-memory
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-memory
    PUBLIC switchrecomp-common switchrecomp-format
)
totkrecomp_enable_project_warnings(switchrecomp-memory)
totkrecomp_enable_sanitizers(switchrecomp-memory)

add_library(switchrecomp-aarch64
    src/switchrecomp/aarch64/register.cpp
    src/switchrecomp/aarch64/instruction.cpp
    src/switchrecomp/aarch64/decoder.cpp
)
target_include_directories(switchrecomp-aarch64
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-aarch64
    PUBLIC switchrecomp-common switchrecomp-memory
    PRIVATE capstone
)
totkrecomp_enable_project_warnings(switchrecomp-aarch64)
totkrecomp_enable_sanitizers(switchrecomp-aarch64)

add_library(switchrecomp-analysis
    src/switchrecomp/analysis/control_flow_graph.cpp
    src/switchrecomp/analysis/cfg_analyzer.cpp
)
target_include_directories(switchrecomp-analysis
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-analysis
    PUBLIC switchrecomp-common switchrecomp-memory switchrecomp-aarch64
)
totkrecomp_enable_project_warnings(switchrecomp-analysis)
totkrecomp_enable_sanitizers(switchrecomp-analysis)

add_library(switchrecomp-runtime
    src/switchrecomp/runtime/cpu_state.cpp
    src/switchrecomp/runtime/abi.cpp
)
target_include_directories(switchrecomp-runtime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-runtime
    PUBLIC switchrecomp-common switchrecomp-memory switchrecomp-aarch64
)
totkrecomp_enable_project_warnings(switchrecomp-runtime)
totkrecomp_enable_sanitizers(switchrecomp-runtime)

add_library(switchrecomp-ir
    src/switchrecomp/ir/type.cpp
    src/switchrecomp/ir/instruction.cpp
    src/switchrecomp/ir/builder.cpp
    src/switchrecomp/ir/verifier.cpp
    src/switchrecomp/ir/printer.cpp
    src/switchrecomp/ir/interpreter.cpp
)
target_include_directories(switchrecomp-ir
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-ir
    PUBLIC switchrecomp-common switchrecomp-memory switchrecomp-aarch64 switchrecomp-runtime
)
totkrecomp_enable_project_warnings(switchrecomp-ir)
totkrecomp_enable_sanitizers(switchrecomp-ir)

add_library(switchrecomp-lift
    src/switchrecomp/lift/aarch64_lifter.cpp
)
target_include_directories(switchrecomp-lift
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-lift
    PUBLIC switchrecomp-common switchrecomp-aarch64 switchrecomp-analysis switchrecomp-ir
)
totkrecomp_enable_project_warnings(switchrecomp-lift)
totkrecomp_enable_sanitizers(switchrecomp-lift)

if(SWITCHRECOMP_ENABLE_LLVM)
    find_package(LLVM CONFIG QUIET)
endif()

if(LLVM_FOUND)
    set(SWITCHRECOMP_LLVM_HEADERS_FOUND OFF)
    foreach(llvm_include_dir IN LISTS LLVM_INCLUDE_DIRS)
        if(EXISTS "${llvm_include_dir}/llvm/IR/LLVMContext.h" AND
           EXISTS "${llvm_include_dir}/llvm/ExecutionEngine/Orc/LLJIT.h" AND
           EXISTS "${llvm_include_dir}/llvm/Target/TargetMachine.h")
            set(SWITCHRECOMP_LLVM_HEADERS_FOUND ON)
        endif()
    endforeach()
    if(NOT SWITCHRECOMP_LLVM_HEADERS_FOUND)
        message(WARNING "LLVMConfig.cmake was found but LLVM development headers are missing; disabling the LLVM backend")
        set(LLVM_FOUND FALSE)
    endif()
endif()

if(LLVM_FOUND)
    message(STATUS "SwitchRecomp LLVM backend enabled: LLVM ${LLVM_VERSION_STRING}")
    if(DEFINED LLVM_VERSION_MAJOR AND NOT LLVM_VERSION_MAJOR EQUAL 22)
        message(WARNING "SwitchRecomp prefers LLVM 22.1.8; found LLVM ${LLVM_VERSION_STRING}")
    endif()
    llvm_map_components_to_libnames(SWITCHRECOMP_LLVM_LIBS
        Core Analysis Support IRReader AsmParser OrcJIT ExecutionEngine MC Target CodeGen native)
endif()

add_library(switchrecomp-codegen
    src/switchrecomp/codegen/llvm_lowerer.cpp
    src/switchrecomp/codegen/llvm_jit.cpp
    src/switchrecomp/codegen/llvm_object_emitter.cpp
)
target_include_directories(switchrecomp-codegen
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-codegen
    PUBLIC switchrecomp-common switchrecomp-ir switchrecomp-runtime
)
if(LLVM_FOUND)
    target_compile_definitions(switchrecomp-codegen PRIVATE SWITCHRECOMP_HAS_LLVM=1)
    target_include_directories(switchrecomp-codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(switchrecomp-codegen PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(switchrecomp-codegen PRIVATE ${SWITCHRECOMP_LLVM_LIBS})
endif()
totkrecomp_enable_project_warnings(switchrecomp-codegen)
totkrecomp_enable_sanitizers(switchrecomp-codegen)

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
target_link_libraries(aarch64-lift PRIVATE switchrecomp-lift switchrecomp-codegen)
target_compile_definitions(aarch64-lift PRIVATE SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev")
if(UNIX AND NOT APPLE)
    target_link_options(aarch64-lift PRIVATE -rdynamic)
endif()
totkrecomp_enable_project_warnings(aarch64-lift)
totkrecomp_enable_sanitizers(aarch64-lift)

add_executable(nso-dynamic-inspect tools/nso-dynamic-inspect/main.cpp)
target_link_libraries(nso-dynamic-inspect
    PRIVATE switchrecomp-memory nlohmann_json::nlohmann_json
)
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
        tests/reference_executor.cpp
    )
    target_link_libraries(switchrecomp-tests
        PRIVATE
            switchrecomp-common
            switchrecomp-format
            switchrecomp-memory
            switchrecomp-aarch64
            switchrecomp-analysis
            switchrecomp-runtime
            switchrecomp-ir
            switchrecomp-lift
            switchrecomp-codegen
            switchrecomp-target
            lz4_static
            Catch2::Catch2WithMain
    )
    if(UNIX AND NOT APPLE)
        target_link_options(switchrecomp-tests PRIVATE -rdynamic)
    endif()
    totkrecomp_enable_project_warnings(switchrecomp-tests)
    totkrecomp_enable_sanitizers(switchrecomp-tests)

    include(Catch)
    catch_discover_tests(switchrecomp-tests)
endif()

install(TARGETS nso-inspect nso-dynamic-inspect aarch64-analyze aarch64-lift RUNTIME DESTINATION bin)
