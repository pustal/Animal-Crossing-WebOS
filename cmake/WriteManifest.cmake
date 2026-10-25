# WriteManifest.cmake
#
# Script mode helper (cmake -P) run after packaging. Writes the webOS Homebrew
# Channel manifest that goes next to the .ipk in each GitHub release, as
# <id>.manifest.json. The webos-repo entry points at the latest release's copy,
# so a new release reaches TVs without touching the repository.
#
# Variables: IPK (built package), OUTPUT (manifest path), APP_ID, VERSION,
#            TITLE, DESCRIPTION, ICON_URI, SOURCE_URL.

foreach(required IPK OUTPUT APP_ID VERSION TITLE DESCRIPTION ICON_URI SOURCE_URL)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "WriteManifest.cmake needs ${required}")
    endif()
endforeach()

file(SHA256 "${IPK}" ipk_sha256)
get_filename_component(ipk_name "${IPK}" NAME)

# ipkUrl is relative to the manifest, so it always names the .ipk of the same release.
file(WRITE "${OUTPUT}" "{
  \"id\": \"${APP_ID}\",
  \"version\": \"${VERSION}\",
  \"type\": \"native\",
  \"title\": \"${TITLE}\",
  \"appDescription\": \"${DESCRIPTION}\",
  \"iconUri\": \"${ICON_URI}\",
  \"sourceUrl\": \"${SOURCE_URL}\",
  \"rootRequired\": false,
  \"ipkUrl\": \"${ipk_name}\",
  \"ipkHash\": { \"sha256\": \"${ipk_sha256}\" }
}
")

message(STATUS "Homebrew Channel manifest: ${OUTPUT}")
