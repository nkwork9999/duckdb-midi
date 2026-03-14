cmake_minimum_required(VERSION 3.5...3.29)

set(TARGET_NAME midi)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# midifile ソースを収集
file(GLOB MIDIFILE_SOURCES "third_party/midifile/src/*.cpp")

set(EXTENSION_SOURCES
    src/midi_extension.cpp
    src/read_midi.cpp
    src/midi_notes.cpp
    src/midi_meta.cpp
    src/midi_scalar.cpp
    src/midi_playback.cpp
    src/midi_write.cpp
    ${MIDIFILE_SOURCES}
)

include_directories(src/include third_party/midifile/include)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# midifile ヘッダーパス
target_include_directories(${EXTENSION_NAME} PRIVATE third_party/midifile/include)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE third_party/midifile/include)

# macOS AudioToolbox（再生機能用）
if(APPLE)
    find_library(AUDIOTOOLBOX AudioToolbox)
    find_library(COREAUDIO CoreAudio)
    find_library(COREFOUNDATION CoreFoundation)
    if(AUDIOTOOLBOX AND COREAUDIO AND COREFOUNDATION)
        target_link_libraries(${EXTENSION_NAME} ${AUDIOTOOLBOX} ${COREAUDIO} ${COREFOUNDATION})
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${AUDIOTOOLBOX} ${COREAUDIO} ${COREFOUNDATION})
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MIDI_PLAYBACK_MACOS)
        target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE MIDI_PLAYBACK_MACOS)
    endif()
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
