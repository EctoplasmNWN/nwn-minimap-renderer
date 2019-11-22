cmake_minimum_required(VERSION 3.8)
project(mapgen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

include_directories(external/lodepng)
include_directories(external/TGA)
include_directories(external/NWNFileFormats)
add_subdirectory(external/NWNFileFormats)
add_subdirectory(src)
