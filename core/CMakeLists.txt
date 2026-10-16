add_library(flashcore STATIC
  src/audio_encoder.cpp
  src/clock.cpp
  src/converter.cpp
  src/crc32.cpp
  src/d3d11.cpp
  src/discovery.cpp
  src/input_mire.cpp
  src/input_wasapi.cpp
  src/input_wgc.cpp
  src/log.cpp
  src/mf_encoder.cpp
  src/mire.cpp
  src/nvenc.cpp
  src/pairing.cpp
  src/receiver.cpp
  src/security.cpp
  src/sender.cpp
  src/srt_link.cpp
  src/ts_demux.cpp
  src/ts_mux.cpp
  ../third_party/monocypher/monocypher.c
)

target_include_directories(flashcore PUBLIC include PRIVATE ../third_party/monocypher)
target_link_libraries(flashcore PUBLIC flash::srt Opus::opus d3d11 dxgi dwmapi ws2_32 winmm avrt mmdevapi windowsapp mfplat mfuuid strmiids iphlpapi crypt32 bcrypt)
# C++/WinRT ne garde pas l'emplacement des erreurs dans le source (aucun chemin dans les binaires)
target_compile_definitions(flashcore PUBLIC WINRT_NO_SOURCE_LOCATION)
