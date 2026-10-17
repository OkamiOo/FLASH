add_executable(flash WIN32
  banc.cpp
  engine.cpp
  main.cpp
  obs_client.cpp
  ui.cpp
  updater.cpp
  viewer.cpp
  flash.rc
)

# La page et l'icône sont embarquées dans l'exe : toute modification refait la ressource
set_source_files_properties(flash.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html;${CMAKE_CURRENT_SOURCE_DIR}/ui/flash.ico")

# Appli fenêtrée qui garde main() : la ligne de commande écrit dans la console qui l'a lancée
target_link_options(flash PRIVATE /ENTRY:mainCRTStartup)

set(FLASH_WEBVIEW2 "${CMAKE_SOURCE_DIR}/.deps/webview2/build/native")
target_include_directories(flash PRIVATE "${FLASH_WEBVIEW2}/include")
target_link_libraries(flash PRIVATE flashcore "${FLASH_WEBVIEW2}/x64/WebView2LoaderStatic.lib" version shlwapi d2d1 dwrite shcore)
target_compile_definitions(flash PRIVATE FLASH_VERSION="${PROJECT_VERSION}")

# libsrt d'obs-deps (la même que celle livrée avec OBS 32.2.2), à côté de l'exe
add_custom_command(TARGET flash POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FLASH_DEPS}/bin/srt.dll" "$<TARGET_FILE_DIR:flash>")
