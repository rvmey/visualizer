cmake_minimum_required(VERSION 3.21)
project(MilkdropVisualizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(projectM4 REQUIRED)
find_package(projectM4Playlist REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)
find_package(GLEW REQUIRED)

# --- Embed the .milk preset library into the executable ---
#
# tools/preset_packer concatenates every .milk file under
# MILKDROP_PRESETS_SOURCE_DIR into one blob (presets.pack) plus a generated
# manifest .cpp of relative-path/offset/size entries. The blob is embedded as
# the PRESETS_PACK RCDATA resource (app_presets.rc.in) and unpacked at
# startup by src/preset_extractor.cpp, so the app needs no external presets
# folder. The pack is only (re)built when presets.pack is missing -- delete
# ${CMAKE_BINARY_DIR}/generated/presets.pack to re-pack after changing the
# source preset set.
set(MILKDROP_PRESETS_SOURCE_DIR "D:/appdev/milkdrop/presets-cream-of-the-crop" CACHE PATH
    "Directory of .milk presets to embed into the executable")

add_executable(preset_packer tools/preset_packer.cpp)

set(PRESETS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(PRESETS_PACK_ABS_PATH "${PRESETS_GENERATED_DIR}/presets.pack")
set(PRESETS_MANIFEST_CPP "${PRESETS_GENERATED_DIR}/embedded_presets_data.cpp")

add_custom_command(
    OUTPUT "${PRESETS_PACK_ABS_PATH}" "${PRESETS_MANIFEST_CPP}"
    COMMAND preset_packer "${MILKDROP_PRESETS_SOURCE_DIR}" "${PRESETS_PACK_ABS_PATH}" "${PRESETS_MANIFEST_CPP}"
    DEPENDS preset_packer
    COMMENT "Packing MilkDrop presets from ${MILKDROP_PRESETS_SOURCE_DIR}"
    VERBATIM
)
add_custom_target(pack_presets DEPENDS "${PRESETS_PACK_ABS_PATH}" "${PRESETS_MANIFEST_CPP}")

configure_file(app_presets.rc.in "${PRESETS_GENERATED_DIR}/app_presets.rc" @ONLY)

add_executable(MilkdropVisualizer
    src/main.cpp
    src/audio_capture.cpp
    src/audio_capture.h
    src/preset_switcher.cpp
    src/preset_switcher.h
    src/preset_extractor.cpp
    src/preset_extractor.h
    src/embedded_presets.h
    "${PRESETS_MANIFEST_CPP}"
    app.rc
    "${PRESETS_GENERATED_DIR}/app_presets.rc"
)
add_dependencies(MilkdropVisualizer pack_presets)
target_include_directories(MilkdropVisualizer PRIVATE src)

target_link_libraries(MilkdropVisualizer PRIVATE
    libprojectM::projectM
    libprojectM::playlist
    SDL2::SDL2
    SDL2::SDL2main
    $<IF:$<TARGET_EXISTS:SDL2_ttf::SDL2_ttf>,SDL2_ttf::SDL2_ttf,SDL2_ttf::SDL2_ttf-static>
)

if(TARGET GLEW::GLEW)
    target_link_libraries(MilkdropVisualizer PRIVATE GLEW::GLEW)
elseif(TARGET GLEW::glew)
    target_link_libraries(MilkdropVisualizer PRIVATE GLEW::glew)
elseif(TARGET GLEW::glew_s)
    target_link_libraries(MilkdropVisualizer PRIVATE GLEW::glew_s)
endif()

if(WIN32)
    target_link_libraries(MilkdropVisualizer PRIVATE ole32)
    target_compile_definitions(MilkdropVisualizer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    set_target_properties(MilkdropVisualizer PROPERTIES
        VS_DPI_AWARE "PerMonitor"
    )
endif()

# Copy projectM DLLs to output directory
set(_PM_INSTALL_BIN "${projectM4_DIR}/../../../bin")
cmake_path(NORMAL_PATH _PM_INSTALL_BIN)
file(GLOB _PM_DLLS "${_PM_INSTALL_BIN}/*.dll")
foreach(_dll ${_PM_DLLS})
    add_custom_command(TARGET MilkdropVisualizer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" $<TARGET_FILE_DIR:MilkdropVisualizer>
    )
endforeach()

# Copy SDL2 DLL
add_custom_command(TARGET MilkdropVisualizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:MilkdropVisualizer>
)

# Copy GLEW DLL
if(TARGET GLEW::glew)
    add_custom_command(TARGET MilkdropVisualizer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GLEW::glew> $<TARGET_FILE_DIR:MilkdropVisualizer>
    )
endif()
