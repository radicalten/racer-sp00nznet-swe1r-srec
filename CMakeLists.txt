cmake_minimum_required(VERSION 3.20)

# Must come before project() to take effect.
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE PATH
    "vcpkg toolchain file")

project(SWE1Racer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# =============================================================================
# Options
# =============================================================================
set(N64_MODERN_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/../N64ModernRuntime" CACHE PATH
    "Path to N64ModernRuntime")
set(RT64_DIR "${CMAKE_SOURCE_DIR}/../rt64" CACHE PATH
    "Path to RT64 renderer")

# =============================================================================
# SDL2 (via vcpkg)
# =============================================================================
find_package(SDL2 CONFIG REQUIRED)

# =============================================================================
# RT64 Renderer
# =============================================================================
# RT64 links SDL2 by bare name; ensure vcpkg's lib dir is on the search path
link_directories("C:/vcpkg/installed/x64-windows/lib")
link_directories("C:/vcpkg/installed/x64-windows/lib/manual-link")
set(RT64_STATIC ON CACHE BOOL "Build RT64 as static library" FORCE)
add_subdirectory(${RT64_DIR} ${CMAKE_BINARY_DIR}/rt64)

# =============================================================================
# N64ModernRuntime (librecomp + ultramodern)
# =============================================================================
add_subdirectory(${N64_MODERN_RUNTIME_DIR} ${CMAKE_BINARY_DIR}/N64ModernRuntime)
# The game's own libultra pokes VI/AI/PI/MI registers (0xA4000000+) directly;
# commit memory past kseg0 so those accesses land in scratch memory.
target_compile_definitions(librecomp PUBLIC RECOMP_MEM_SIZE_MB=640)

# =============================================================================
# Recompiled Functions Library
# =============================================================================
file(GLOB RECOMP_SOURCES "RecompiledFuncs/*.c")

add_library(RecompiledFuncs STATIC ${RECOMP_SOURCES})

target_include_directories(RecompiledFuncs PRIVATE
    ${N64_MODERN_RUNTIME_DIR}/N64Recomp/include  # recomp.h
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs           # funcs.h
)

# Disable strict aliasing for recompiled code (MIPS memory access patterns)
if(MSVC)
    # MSVC doesn't have strict aliasing by default
else()
    target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing)
endif()

# =============================================================================
# Main Executable
# =============================================================================
add_executable(SWE1Racer
    src/main.cpp
    src/section_table.cpp
    src/rt64_render_context.cpp
    src/audio.cpp
    src/config.cpp
    src/input.cpp
)

target_include_directories(SWE1Racer PRIVATE
    ${N64_MODERN_RUNTIME_DIR}/N64Recomp/include     # recomp.h
    ${N64_MODERN_RUNTIME_DIR}/librecomp/include      # librecomp headers
    ${N64_MODERN_RUNTIME_DIR}/ultramodern/include    # ultramodern headers
    ${RT64_DIR}/src                                   # RT64 headers
    ${RT64_DIR}/src/contrib                           # RT64 contrib (json, im3d, etc.)
    ${RT64_DIR}/src/contrib/hlslpp/include            # hlsl++
    ${RT64_DIR}/src/contrib/plume                     # plume render interface
    ${RT64_DIR}/src/contrib/imgui                     # imgui
    ${RT64_DIR}/src/contrib/mupen64plus-core/src/api  # mupen64plus API types
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs              # funcs.h
    ${CMAKE_SOURCE_DIR}/include
)

# RT64 requires these defines for its headers (normally set at directory scope)
target_compile_definitions(SWE1Racer PRIVATE NOMINMAX HLSL_CPU FFX_GCC)

target_link_libraries(SWE1Racer PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    SDL2::SDL2
    SDL2::SDL2main
)

if(WIN32)
    target_link_libraries(SWE1Racer PRIVATE dxgi d3d12 d3dcompiler dbghelp)
    target_link_options(SWE1Racer PRIVATE /MAP)
    # RT64 loads the DXC runtime next to the exe; without it setup() silently skips init.
    add_custom_command(TARGET SWE1Racer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${RT64_DIR}/src/contrib/dxc/bin/x64/dxil.dll ${RT64_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll
        $<TARGET_FILE_DIR:SWE1Racer>)
endif()
