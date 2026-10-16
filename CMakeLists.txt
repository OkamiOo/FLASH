cmake_minimum_required(VERSION 3.28)

project(flash VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "FLASH se compile en 64 bits uniquement")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# CRT statique : ni l'appli ni le plugin ne dépendent d'un redistribuable Visual C++
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(FLASH_DEPS "${CMAKE_SOURCE_DIR}/.deps/obs-deps")
set(FLASH_OBS_SRC "${CMAKE_SOURCE_DIR}/.deps/obs-src/obs-studio-32.2.2-sources")
set(FLASH_TEST_OBS "${CMAKE_SOURCE_DIR}/.test-obs")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
add_compile_options(/utf-8 /W3 /permissive- /Zc:__cplusplus /Brepro /GS /Gy)
# Pas de PDB ni de chemin de compilation dans les binaires
add_link_options(/Brepro /DEBUG:NONE /INCREMENTAL:NO /OPT:REF /OPT:ICF)

# Bibliothèques importées d'obs-deps (FFmpeg et SRT livrés avec OBS 32.2.2)
function(flash_import_lib name lib)
  add_library(${name} INTERFACE IMPORTED GLOBAL)
  set_target_properties(${name} PROPERTIES
    INTERFACE_LINK_LIBRARIES "${FLASH_DEPS}/lib/${lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${FLASH_DEPS}/include")
endfunction()

flash_import_lib(flash::srt srt.lib)
flash_import_lib(flash::avcodec avcodec.lib)
flash_import_lib(flash::avutil avutil.lib)
flash_import_lib(flash::swresample swresample.lib)

add_subdirectory(core)
add_subdirectory(app)
add_subdirectory(obs)
