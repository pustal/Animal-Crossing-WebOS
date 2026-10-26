# Animal Crossing for LG webOS TVs: the ac-decomp based PC port, built natively
# for the TV with SDL-webOS and OpenGL ES 3.
#
# The game sources come from the ACGC Android port (external/ACGC-Android-Port),
# which is the upstream PC port plus the OpenGL ES 3 / TARGET_ANDROID path.
# This build compiles that Android variant for webOS, with src/webos/ taking
# the place of the Android app glue.
#
#   cmake -B build-webos -G Ninja -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#   cmake --build build-webos && cmake --build build-webos --target webos-package

cmake_minimum_required(VERSION 3.21)
project(AnimalCrossingWebOS VERSION 0.1.4 LANGUAGES C CXX)

set(AC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${AC_ROOT}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# The decomp relies on GNU extensions (as upstream builds with gcc).
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(AC_APP_ID "org.animalcrossingwebos.app" CACHE STRING "webOS application id")

# ---------------------------------------------------------------------------
# Target: only the TV (32-bit ARM webOS). The decomp needs 32-bit pointers.
# ---------------------------------------------------------------------------
execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE AC_COMPILER_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT AC_COMPILER_TARGET MATCHES "webos")
    message(FATAL_ERROR "Build with the webOS toolchain (got '${AC_COMPILER_TARGET}'). "
                        "See README.md: -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The game must be built as 32-bit (JSystem casts pointers to u32).")
endif()

set(DECOMP_ROOT "${AC_ROOT}/external/ACGC-Android-Port")
if(NOT EXISTS "${DECOMP_ROOT}/pc/src/pc_main.c")
    message(FATAL_ERROR "Missing ${DECOMP_ROOT}. Run: git submodule update --init")
endif()

include(SDL2WebOSBackport)   # SDL2::SDL2 (bundled SDL-webOS 2.30)
include(GlesLinkStub)        # ACWEBOS::GLESv2 (link-time stub; the TV's driver at run time)

# ---------------------------------------------------------------------------
# Compiler setup (mirrors the Android port's app/src/main/cpp/CMakeLists.txt)
# ---------------------------------------------------------------------------
add_compile_definitions(
    TARGET_PC
    TARGET_ANDROID   # the port's mobile/ES3 path: ES 3 context, fullscreen, vsync, Back = pause
    TARGET_WEBOS
    VERSION=0
    F3DEX_GBI_2
    NDEBUG
    BUGFIXES
    _LANGUAGE_C
    KEYBOARD_TYPING
)

# Same flags as upstream: these neutralise the decomp's undefined-behaviour classes.
add_compile_options(-O2 -fno-strict-aliasing -fwrapv)
# jaudio_NES C files use nullptr.
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Dnullptr=NULL>)
# glibc extensions the port uses (dladdr/Dl_info in pc_main.c); g++ defines this already.
add_compile_options($<$<COMPILE_LANGUAGE:C>:-D_GNU_SOURCE>)
# Reconciles the decomp's libc declarations with glibc (see the header).
add_compile_options("SHELL:-include \"${AC_ROOT}/src/webos/webos_forced_decls.h\"")

include_directories(
    "${AC_ROOT}/src/webos"                  # glad/gl.h and android_glue.h stand-ins (first!)
    "${AC_ROOT}/third_party/khronos"        # GLES3 headers (the sysroot only has ES 2)
    "${DECOMP_ROOT}/include"
    "${DECOMP_ROOT}/src"
    "${DECOMP_ROOT}"
    "${DECOMP_ROOT}/pc/include"
    "${DECOMP_ROOT}/pc/lib/fixnes"
)

# ---------------------------------------------------------------------------
# Game sources (same selection as the Android port)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE GAME_C_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/*.c")
file(GLOB_RECURSE GAME_CXX_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/*.cpp")

foreach(pattern
        ".*/AUS/.*" ".*/dolphin/gx/.*" ".*/dolphin/os/.*" ".*/dolphin/vi/.*" ".*/dolphin/pad/.*"
        ".*/dolphin/card/.*" ".*/dolphin/dvd/.*" ".*/dolphin/ai/.*" ".*/dolphin/ar/.*" ".*/dolphin/dsp/.*"
        ".*/dolphin/exi/.*" ".*/dolphin/si/.*" ".*/dolphin/db/.*" ".*/dolphin/mtx/.*" ".*/dolphin/base/.*"
        ".*/dolphin/gba/.*" ".*/dolphin/amcstubs/.*" ".*/dolphin/OdemuExi2/.*" ".*/dolphin/odenotstub/.*"
        ".*/TRK_MINNOW_DOLPHIN/.*" ".*/Runtime\\.PPCEABI\\.H/.*" ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*")
    list(FILTER GAME_C_SOURCES EXCLUDE REGEX "${pattern}")
endforeach()

file(GLOB_RECURSE DATA_C_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/data/*.c")
list(APPEND GAME_C_SOURCES ${DATA_C_SOURCES})
list(REMOVE_DUPLICATES GAME_C_SOURCES)

foreach(pattern
        ".*/src/furniture/.*" ".*/src/data/model/obj_e_boat/.*" ".*/ac_npc_shop_common\\.c$"
        ".*/ac_animal_logo_misc\\.c$" ".*/m_item_debug\\.c$" ".*/actor/npc/.*_anime\\.c$" ".*/Famicom/.*"
        ".*/MSL_C/.*" ".*/jaudio_NES/internal/dsp_cardunlock\\.c$" ".*/jaudio_NES/internal/dsp_GBAKey\\.c$"
        ".*/libc64/malloc\\.c$" ".*/libc64/sprintf\\.c$" ".*/libc64/aprintf\\.c$" ".*/libultra/.*"
        ".*/ReconfigBATs\\.c$" ".*/executor\\.c$" ".*/ef_effect_lib\\.c$" ".*/m_card\\.c$" ".*/osreport\\.c$"
        ".*/emu64_utility\\.c$")
    list(FILTER GAME_C_SOURCES EXCLUDE REGEX "${pattern}")
endforeach()

foreach(pattern ".*/AUS/.*" ".*/dolphin/.*" ".*/TRK_MINNOW_DOLPHIN/.*" ".*/Runtime\\.PPCEABI\\.H/.*"
                ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*" ".*/Famicom/.*" ".*/emu64_print\\.cpp$"
                ".*/jsyswrapper_ext\\.cpp$" ".*/jsyswrapper_main\\.cpp$")
    list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX "${pattern}")
endforeach()
list(APPEND GAME_CXX_SOURCES
    "${DECOMP_ROOT}/src/static/Famicom/famicom.cpp"
    "${DECOMP_ROOT}/src/static/Famicom/famicom_nesinfo.cpp"
    "${DECOMP_ROOT}/src/static/Famicom/ks_nes_core.cpp"
    "${DECOMP_ROOT}/src/static/Famicom/ks_nes_draw.cpp")

# PC port layer.
set(PC_NAMES
    pc_main pc_gx pc_gx_tev pc_gx_texture pc_gbi_runtime pc_os pc_dvd pc_pad pc_typing pc_card pc_vi
    pc_audio pc_aram pc_misc pc_mtx pc_m_card pc_save_bswap pc_model_viewer pc_texture_pack pc_settings
    pc_profiler pc_pause_menu pc_settings_menu pc_menu_util pc_text_draw pc_keybindings pc_nes_fixnes
    pc_stubs pc_assets pc_msg_eur pc_disc)
set(PC_SOURCES "${DECOMP_ROOT}/src/static/dolphin/pad/Padclamp.c" "${DECOMP_ROOT}/pc/src/pc_stubs_cpp.cpp")
foreach(name IN LISTS PC_NAMES)
    list(APPEND PC_SOURCES "${DECOMP_ROOT}/pc/src/${name}.c")
endforeach()

# fixNES: the NES emulator used for the in-game NES.
set(FIXNES_DIR "${DECOMP_ROOT}/pc/lib/fixnes")
file(GLOB FIXNES_MAPPER_SOURCES "${FIXNES_DIR}/mapper/*.c")
set(FIXNES_SOURCES ${FIXNES_MAPPER_SOURCES})
foreach(name cpu ppu apu mem input mapper mapperList vrc_irq audio fm2play audio_fds audio_mmc5 audio_vrc6
             audio_vrc7 audio_n163 audio_s5b)
    list(APPEND FIXNES_SOURCES "${FIXNES_DIR}/${name}.c")
endforeach()
set_source_files_properties(${FIXNES_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;-O2"
    COMPILE_DEFINITIONS "COL_TEX_BSWAP;COL_GL_BSWAP;DO_INLINE_ATTRIBS=1")
set_source_files_properties("${DECOMP_ROOT}/pc/src/pc_nes_fixnes.c" PROPERTIES COMPILE_OPTIONS "-w;-O2")

# The game and the boot code each have a main(); pc_main.c's main() is the real one.
set_source_files_properties("${DECOMP_ROOT}/src/main.c" PROPERTIES COMPILE_DEFINITIONS "main=ac_entry")
set_source_files_properties("${DECOMP_ROOT}/src/static/boot.c" PROPERTIES COMPILE_DEFINITIONS "main=boot_main")

# .c files that contain C++ code.
set_source_files_properties(
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/ja_calc.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/jammain_2.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/game/game64.c"
    PROPERTIES LANGUAGE CXX)

# Decomp and port sources: warnings silenced, as upstream.
set_source_files_properties(${GAME_C_SOURCES} ${GAME_CXX_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-fpermissive")
set(PC_C_SOURCES ${PC_SOURCES})
list(FILTER PC_C_SOURCES EXCLUDE REGEX "\\.cpp$")
set_source_files_properties(${PC_C_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${DECOMP_ROOT}/pc/src/pc_stubs_cpp.cpp" PROPERTIES COMPILE_OPTIONS "-w;-fpermissive")

# ---------------------------------------------------------------------------
# The executable
# ---------------------------------------------------------------------------
add_executable(AnimalCrossing
    ${PC_SOURCES}
    ${FIXNES_SOURCES}
    ${GAME_C_SOURCES}
    ${GAME_CXX_SOURCES}
    src/webos/webos_glue.c
    src/webos/webos_bstring.c
    src/webos/webos_gpu_profile.c
    src/webos/webos_present.c
    src/webos/webos_remote.c
    src/webos/webos_shader_cache.c
    src/webos/webos_tev_specialize.c)

# Our own glue is held to stricter warnings than the decomp.
set_source_files_properties(
    src/webos/webos_glue.c src/webos/webos_gpu_profile.c src/webos/webos_present.c src/webos/webos_remote.c src/webos/webos_shader_cache.c src/webos/webos_tev_specialize.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# The port includes both <SDL.h> and <SDL2/SDL_x.h>: both must resolve to the
# bundled SDL, not to the older SDL 2 headers in the toolchain's sysroot.
get_target_property(_ac_sdl_include SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
get_filename_component(_ac_sdl_include_parent "${_ac_sdl_include}" DIRECTORY)
target_include_directories(AnimalCrossing BEFORE PRIVATE "${_ac_sdl_include_parent}")

find_package(Threads REQUIRED)
target_link_libraries(AnimalCrossing PRIVATE SDL2::SDL2 ACWEBOS::GLESv2 Threads::Threads rt m ${CMAKE_DL_LIBS})
# The TV's C++ runtime is older than the toolchain's; carry our own.
target_link_options(AnimalCrossing PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(AnimalCrossing PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "\$ORIGIN/../lib")

include(WebOSPackage)
