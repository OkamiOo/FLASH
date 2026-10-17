# Bibliothèque d'import de libobs, générée depuis l'obs.dll d'OBS 32.2.2 : pas besoin de
# recompiler libobs, seuls ses en-têtes servent.
set(OBS_DLL "${FLASH_TEST_OBS}/bin/64bit/obs.dll")
set(OBS_DEF "${CMAKE_CURRENT_BINARY_DIR}/obs.def")
set(OBS_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")

get_filename_component(_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
find_program(FLASH_DUMPBIN dumpbin HINTS "${_msvc_bin}" REQUIRED)
find_program(FLASH_LIBEXE lib HINTS "${_msvc_bin}" REQUIRED)

if(NOT EXISTS "${OBS_IMPLIB}" OR "${OBS_DLL}" IS_NEWER_THAN "${OBS_IMPLIB}")
  execute_process(COMMAND "${FLASH_DUMPBIN}" /nologo /exports "${OBS_DLL}" OUTPUT_VARIABLE _exports)
  string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ +[0-9A-F]+ +[A-Za-z_][A-Za-z0-9_]*" _lines "${_exports}")
  set(_def "LIBRARY obs\nEXPORTS\n")
  foreach(_line IN LISTS _lines)
    string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_]*)$" "\\1" _name "${_line}")
    string(APPEND _def "  ${_name}\n")
  endforeach()
  file(WRITE "${OBS_DEF}" "${_def}")
  execute_process(COMMAND "${FLASH_LIBEXE}" /nologo /def:${OBS_DEF} /machine:x64 /out:${OBS_IMPLIB}
                  RESULT_VARIABLE _res OUTPUT_QUIET)
  if(_res)
    message(FATAL_ERROR "Génération de obs.lib impossible")
  endif()
endif()

add_library(flash::obs INTERFACE IMPORTED)
set_target_properties(flash::obs PROPERTIES
  INTERFACE_LINK_LIBRARIES "${OBS_IMPLIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${FLASH_OBS_SRC}/libobs;${CMAKE_CURRENT_SOURCE_DIR}/compat")

add_library(flash-obs MODULE
  audio_track.cpp
  connection.cpp
  directory.cpp
  plugin.cpp
  provider.cpp
  sources.cpp
  video_track.cpp
)

target_link_libraries(flash-obs PRIVATE flashcore flash::obs flash::avcodec flash::avutil flash::swresample)
set_target_properties(flash-obs PROPERTIES PREFIX "")

target_compile_definitions(flash-obs PRIVATE FLASH_VERSION="${PROJECT_VERSION}")
