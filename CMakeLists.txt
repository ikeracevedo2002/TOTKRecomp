cmake_minimum_required(VERSION 3.20)

project(TOTKRecomp VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

option(TOTKRECOMP_BUILD_TESTS "Build the TOTKRecomp test suite" ON)
option(TOTKRECOMP_ENABLE_WARNINGS "Enable warnings for project-owned targets" ON)
option(TOTKRECOMP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

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
)
target_include_directories(switchrecomp-memory
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(switchrecomp-memory
    PUBLIC switchrecomp-common switchrecomp-format
)
totkrecomp_enable_project_warnings(switchrecomp-memory)
totkrecomp_enable_sanitizers(switchrecomp-memory)

add_executable(nso-inspect tools/nso-inspect/main.cpp)
target_link_libraries(nso-inspect PRIVATE switchrecomp-memory)
target_compile_definitions(nso-inspect PRIVATE SWITCHRECOMP_VERSION="${PROJECT_VERSION}-dev")
totkrecomp_enable_project_warnings(nso-inspect)
totkrecomp_enable_sanitizers(nso-inspect)

if(TOTKRECOMP_BUILD_TESTS)
    add_executable(switchrecomp-tests
        tests/common_tests.cpp
        tests/format_tests.cpp
        tests/guest_memory_tests.cpp
        tests/nso_tests.cpp
        tests/nso_materialization_tests.cpp
        tests/mod0_dynamic_tests.cpp
        tests/target_tests.cpp
    )
    target_link_libraries(switchrecomp-tests
        PRIVATE
            switchrecomp-common
            switchrecomp-format
            switchrecomp-memory
            switchrecomp-target
            lz4_static
            Catch2::Catch2WithMain
    )
    totkrecomp_enable_project_warnings(switchrecomp-tests)
    totkrecomp_enable_sanitizers(switchrecomp-tests)

    include(Catch)
    catch_discover_tests(switchrecomp-tests)
endif()

install(TARGETS nso-inspect RUNTIME DESTINATION bin)
