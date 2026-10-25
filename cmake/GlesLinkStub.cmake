# GlesLinkStub.cmake
#
# Provides the target ACWEBOS::GLESv2 for linking against OpenGL ES 3.
#
# The webOS sysroot's libGLESv2.so only exports ES 2 functions, while the TV's
# real Mali driver exports the full ES 3.2 set (checked on the TV with the
# ES3-WebOS-probe). We build a stub shared library from the Khronos header
# with every ES 3 entry point and the same soname as the real library, and link
# against it. The executable then records "libGLESv2.so" as a dependency and
# the TV's own driver is loaded at run time. The stub is never packaged.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_ac_gles_stub_source "${CMAKE_CURRENT_BINARY_DIR}/gles_stub/gles3_stub.c")
add_custom_command(
    OUTPUT "${_ac_gles_stub_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/gles_stub"
    COMMAND "${Python3_EXECUTABLE}" "${AC_ROOT}/tools/gen_gles_stub.py"
            "${AC_ROOT}/third_party/khronos/GLES3/gl3.h" "${_ac_gles_stub_source}"
    DEPENDS "${AC_ROOT}/tools/gen_gles_stub.py" "${AC_ROOT}/third_party/khronos/GLES3/gl3.h"
    COMMENT "Generating the OpenGL ES 3 link stub")

add_library(ac_gles_link_stub SHARED "${_ac_gles_stub_source}")
set_target_properties(ac_gles_link_stub PROPERTIES
    OUTPUT_NAME "GLESv2"
    NO_SONAME OFF
    # Same soname as the TV's library, so the executable asks for the real one.
    LINK_FLAGS "-Wl,-soname,libGLESv2.so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gles_stub")

add_library(ACWEBOS::GLESv2 ALIAS ac_gles_link_stub)
