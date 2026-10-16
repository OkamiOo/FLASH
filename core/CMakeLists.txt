add_library(flashcore STATIC
  src/clock.cpp
  src/crc32.cpp
  src/d3d11.cpp
  src/log.cpp
  src/mire.cpp
  src/nvenc.cpp
  src/receiver.cpp
  src/srt_link.cpp
  src/ts_demux.cpp
  src/ts_mux.cpp
)

target_include_directories(flashcore PUBLIC include)
target_link_libraries(flashcore PUBLIC flash::srt d3d11 dxgi ws2_32 winmm)
