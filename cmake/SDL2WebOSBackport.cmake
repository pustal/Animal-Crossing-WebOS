# SDL2WebOSBackport.cmake
#
# Provides the SDL2::SDL2 target for webOS builds using webosbrew's SDL-webOS
# backport (https://github.com/webosbrew/SDL-webOS), the same prebuilt SDL2 that
# Moonlight TV ships. Compared with the SDL2 in the TV firmware it adds the
# remote control scancodes (SDL_SCANCODE_WEBOS_*), the hints that deliver the
# Back/Exit keys to the app, and up-to-date game controller support.
#
# The shared library is installed next to the app (lib/) and found via RPATH.

include(ExternalProject)

set(AC_SDL2_BACKPORT_RELEASE "release-2.30.12-webos.6" CACHE STRING "SDL-webOS release to bundle")
# Hash of the release above; clear it if you deliberately switch releases.
set(AC_SDL2_BACKPORT_SHA256 "63dc9ea268bdf64801fce5e0c19585ee6aaf79f0b08486c853c47f922385c698"
    CACHE STRING "SHA-256 of the SDL-webOS archive (empty to skip verification)")

string(REGEX REPLACE "^release-([0-9]+\\.[0-9]+\\.[0-9]+)-webos\\.[0-9]+$" "SDL2-\\1-webos.tar.gz"
       _ac_sdl2_archive "${AC_SDL2_BACKPORT_RELEASE}")

set(_ac_sdl2_hash_args)
if(AC_SDL2_BACKPORT_SHA256)
    set(_ac_sdl2_hash_args URL_HASH "SHA256=${AC_SDL2_BACKPORT_SHA256}")
endif()

ExternalProject_Add(ac_sdl2_backport
    URL "https://github.com/webosbrew/SDL-webOS/releases/download/${AC_SDL2_BACKPORT_RELEASE}/${_ac_sdl2_archive}"
    ${_ac_sdl2_hash_args}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR> <INSTALL_DIR>
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libSDL2-2.0.so.0
)
ExternalProject_Get_Property(ac_sdl2_backport INSTALL_DIR)

# The include directory only exists after the download; create it so CMake accepts it now.
file(MAKE_DIRECTORY "${INSTALL_DIR}/include/SDL2")

add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
set_target_properties(SDL2::SDL2 PROPERTIES
    IMPORTED_LOCATION "${INSTALL_DIR}/lib/libSDL2-2.0.so.0"
    IMPORTED_SONAME "libSDL2-2.0.so.0"
    INTERFACE_INCLUDE_DIRECTORIES "${INSTALL_DIR}/include/SDL2"
    INTERFACE_COMPILE_DEFINITIONS "__WEBOS__")
add_dependencies(SDL2::SDL2 ac_sdl2_backport)

# Path of the library file to bundle into the IPK.
set(AC_SDL2_RUNTIME_LIBRARY "${INSTALL_DIR}/lib/libSDL2-2.0.so.0")
