add_executable(flash
  main.cpp
  mire_source.cpp
  opus_encoder.cpp
)

target_link_libraries(flash PRIVATE flashcore Opus::opus)

# libsrt d'obs-deps (la même que celle livrée avec OBS 32.2.2), à côté de l'exe
add_custom_command(TARGET flash POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FLASH_DEPS}/bin/srt.dll" "$<TARGET_FILE_DIR:flash>")
