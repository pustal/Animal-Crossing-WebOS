# WebOSPackage.cmake
#
# Adds the "webos-package" target. It stages the IPK layout under <build>/ipk/:
#   appinfo.json, icons
#   bin/AnimalCrossing           the game (stripped)
#   lib/libSDL2-2.0.so.0         bundled SDL-webOS
#   lib/libacwebos_transfer.so   the upload page (C++, kept out of the game executable)
#   share/shaders/               the port's GLSL sources (rewritten to ES at run time)
#   LICENSE, THIRD_PARTY_NOTICES.md, LICENSE-ACGC-Android-Port.txt, LICENSE-cpp-httplib.txt
# then builds dist/<id>_<version>_arm.ipk with ares-package (@webos-tools/cli)
# and the Homebrew Channel manifest next to it.
#
# No game data is packaged: the player supplies their own disc image.

find_program(AC_ARES_PACKAGE ares-package)

set(AC_IPK_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ipk")
set(AC_DIST_DIR "${AC_ROOT}/dist")
set(AC_IPK_NAME "${AC_APP_ID}_${PROJECT_VERSION}_arm.ipk")

configure_file("${AC_ROOT}/webos/appinfo.json.in" "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" @ONLY)

if(NOT CMAKE_STRIP)
    set(CMAKE_STRIP strip)
endif()

add_custom_target(webos-stage
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${AC_IPK_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${AC_IPK_STAGE}/bin" "${AC_IPK_STAGE}/lib" "${AC_IPK_STAGE}/share"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" "${AC_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${AC_ROOT}/webos/icon.png" "${AC_ROOT}/webos/largeIcon.png" "${AC_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:AnimalCrossing>" "${AC_IPK_STAGE}/bin/AnimalCrossing"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${AC_IPK_STAGE}/bin/AnimalCrossing"
    COMMAND "${CMAKE_COMMAND}" -E copy "${AC_SDL2_RUNTIME_LIBRARY}" "${AC_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${AC_IPK_STAGE}/lib/libSDL2-2.0.so.0"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:acwebos_transfer>" "${AC_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${AC_IPK_STAGE}/lib/$<TARGET_FILE_NAME:acwebos_transfer>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${DECOMP_ROOT}/pc/shaders" "${AC_IPK_STAGE}/share/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy "${AC_ROOT}/LICENSE" "${AC_ROOT}/THIRD_PARTY_NOTICES.md" "${AC_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${DECOMP_ROOT}/LICENSE" "${AC_IPK_STAGE}/LICENSE-ACGC-Android-Port.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${AC_ROOT}/third_party/cpp-httplib/LICENSE" "${AC_IPK_STAGE}/LICENSE-cpp-httplib.txt"
    DEPENDS AnimalCrossing acwebos_transfer
    COMMENT "Staging the IPK contents in ${AC_IPK_STAGE}"
    VERBATIM)

if(AC_ARES_PACKAGE)
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${AC_DIST_DIR}"
        COMMAND "${AC_ARES_PACKAGE}" --no-minify "${AC_IPK_STAGE}" -o "${AC_DIST_DIR}"
        COMMAND "${CMAKE_COMMAND}"
                -DIPK=${AC_DIST_DIR}/${AC_IPK_NAME}
                -DOUTPUT=${AC_DIST_DIR}/${AC_APP_ID}.manifest.json
                -DAPP_ID=${AC_APP_ID}
                -DVERSION=${PROJECT_VERSION}
                "-DTITLE=Animal Crossing"
                "-DDESCRIPTION=Native webOS port of the Animal Crossing (GameCube) decompilation. Bring your own disc image."
                -DICON_URI=https://raw.githubusercontent.com/pustal/Animal-Crossing-WebOS/main/webos/largeIcon.png
                -DSOURCE_URL=https://github.com/pustal/Animal-Crossing-WebOS
                -P "${AC_ROOT}/cmake/WriteManifest.cmake"
        DEPENDS webos-stage
        COMMENT "Building ${AC_IPK_NAME}"
        VERBATIM)
else()
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E echo "ares-package not found: npm install -g @webos-tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E false
        DEPENDS webos-stage)
endif()
