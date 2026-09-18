cmake_minimum_required(VERSION 3.20)

project(fd2re_web LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The web target must be configured with emcmake.")
endif()

set(FD2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../fd2re")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CMAKE_EXECUTABLE_SUFFIX ".html")

# Build the bundled synthesizer from the vendored source. The web target does
# not link to a system audio library or fetch anything at runtime.
set(libADLMIDI_STATIC ON CACHE BOOL "" FORCE)
set(libADLMIDI_SHARED OFF CACHE BOOL "" FORCE)
set(WITH_EMBEDDED_BANKS OFF CACHE BOOL "" FORCE)
set(WITH_MIDI_SEQUENCER ON CACHE BOOL "" FORCE)
set(WITH_XMI_SUPPORT ON CACHE BOOL "" FORCE)
set(USE_DOSBOX_EMULATOR ON CACHE BOOL "" FORCE)
set(USE_NUKED_EMULATOR ON CACHE BOOL "" FORCE)
set(USE_OPAL_EMULATOR OFF CACHE BOOL "" FORCE)
set(USE_JAVA_EMULATOR OFF CACHE BOOL "" FORCE)
set(USE_ESFMU_EMULATOR OFF CACHE BOOL "" FORCE)
set(USE_MAME_EMULATOR OFF CACHE BOOL "" FORCE)
set(WITH_GENADLDATA OFF CACHE BOOL "" FORCE)
add_subdirectory("${FD2_ROOT}/vendor/libADLMIDI"
                 "${CMAKE_BINARY_DIR}/libADLMIDI")

file(GLOB FD2_SOURCES CONFIGURE_DEPENDS "${FD2_ROOT}/src/*.c")
add_executable(fd2re ${FD2_SOURCES})
target_include_directories(fd2re PRIVATE
    "${FD2_ROOT}/include"
    "${FD2_ROOT}/vendor/libADLMIDI/include")
target_link_libraries(fd2re PRIVATE ADLMIDI)
# USE_SDL must be a compile option too: emcc adds the SDL2 port include path
# at compile time only when the setting is present on the compile command.
target_compile_options(fd2re PRIVATE "-sUSE_SDL=2")
set_target_properties(fd2re PROPERTIES OUTPUT_NAME "index")

target_link_options(fd2re PRIVATE
    "-sUSE_SDL=2"
    "-sASYNCIFY"
    "-sASYNCIFY_IMPORTS=__wrap_usleep,__wrap_SDL_Delay"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=67108864"
    "-sSTACK_SIZE=1048576"
    "-sINVOKE_RUN=0"
    "-sFORCE_FILESYSTEM=1"
    "-lidbfs.js"
    "-sEXPORTED_FUNCTIONS=_main,_vram_base,_g_music_ok,_g_music_track_cur"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,FS"
    "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/jslib.js"
    "-Wl,--wrap=SDL_Delay"
    "-Wl,--wrap=usleep"
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/shell.html")

set(FD2_ASSETS
    ANI.DAT BG.DAT DATO.DAT FDFIELD.DAT FDMUS.DAT FDOTHER.DAT
    FDSHAP.DAT FDTXT.DAT FIGANI.DAT TAI.DAT FDICON.B24 FD2.TMP
    FD2SAMPLE.wopl)
foreach(asset IN LISTS FD2_ASSETS)
    target_link_options(fd2re PRIVATE
        "--preload-file=${FD2_ROOT}/${asset}@/${asset}")
endforeach()
target_link_options(fd2re PRIVATE
    "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/../FD2.SAV@/FD2.SAV")

add_custom_command(TARGET fd2re POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E touch
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/.nojekyll"
    VERBATIM)
