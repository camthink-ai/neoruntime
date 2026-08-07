cmake_minimum_required(VERSION 3.10)
project(shm-tools LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# SHM ring buffer reader (metadata, fps, latency, dump)
add_executable(shm-reader shm_reader.c)
target_link_libraries(shm-reader rt)

# NV12 → JPEG converter (zero dependency, built-in encoder)
add_executable(nv12-to-jpeg nv12_to_jpeg.c)
target_link_libraries(nv12-to-jpeg m)

install(TARGETS shm-reader nv12-to-jpeg DESTINATION /opt/aipc/bin)
