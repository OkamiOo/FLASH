add_executable(flash
  main.cpp
  mire_source.cpp
  opus_encoder.cpp
)

target_link_libraries(flash PRIVATE flashcore flash::avcodec flash::avutil)

# DLL d'obs-deps chargées par l'appli (les mêmes que celles livrées avec OBS 32.2.2)
set(FLASH_APP_DLLS srt.dll avcodec-62.dll avutil-60.dll swresample-6.dll libx264-164.dll zlib.dll)
foreach(dll IN LISTS FLASH_APP_DLLS)
  add_custom_command(TARGET flash POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FLASH_DEPS}/bin/${dll}" "$<TARGET_FILE_DIR:flash>")
endforeach()
