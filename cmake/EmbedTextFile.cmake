# EmbedTextFile.cmake
#
# Script mode helper (cmake -P) that turns a text file into a C++ header
# declaring it as a string constant, so the transfer page ships inside the
# binary and needs no extra files on the TV.
#
# Required variables:
#   INPUT     path of the text file to embed
#   OUTPUT    path of the header to write
#   VARIABLE  name of the generated constant

if(NOT INPUT OR NOT OUTPUT OR NOT VARIABLE)
    message(FATAL_ERROR "EmbedTextFile.cmake needs INPUT, OUTPUT and VARIABLE")
endif()

file(READ "${INPUT}" content)

# A raw string literal keeps the HTML readable in the generated header.
# Its delimiter must never appear in the embedded text.
set(delimiter "ACWEBOS_EMBED")
string(FIND "${content}" ")${delimiter}\"" clash)
if(NOT clash EQUAL -1)
    message(FATAL_ERROR "${INPUT} contains the raw-string delimiter ${delimiter}")
endif()

get_filename_component(source_name "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
"// Generated from ${source_name} by cmake/EmbedTextFile.cmake. Do not edit.
#pragma once

static constexpr const char ${VARIABLE}[] = R\"${delimiter}(${content})${delimiter}\";
")

# Only touch the real header when the content changed, to avoid needless rebuilds.
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
