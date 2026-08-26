cmake_minimum_required(VERSION 3.21)
project(MilkdropVisualizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(projectM4 REQUIRED)
find_package(projectM4Playlist REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)
find_package(GLEW REQUIRED)

add_executable(MilkdropVisualizer
    src/main.cpp
    src/audio_capture.cpp
    src/audio_capture.h
    src/preset_switcher.cpp
    src/preset_switcher.h
    app.rc
)

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
