add_executable(flash
  banc.cpp
  main.cpp
)

target_link_libraries(flash PRIVATE flashcore)

# libsrt d'obs-deps (la même que celle livrée avec OBS 32.2.2), à côté de l'exe
add_custom_command(TARGET flash POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FLASH_DEPS}/bin/srt.dll" "$<TARGET_FILE_DIR:flash>")
