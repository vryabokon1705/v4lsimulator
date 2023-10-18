cmake_minimum_required(VERSION 3.10.2 FATAL_ERROR)

include_directories(/usr/include/gz/transport12 /usr/include/gz/msgs9 /usr/include/gz/rendering7 /usr/include/gz/math7 /usr/include/gz/rendering7 /usr/include/gz/sim7 /usr/include/gz/sdformat13)

find_package(gz-cmake3 REQUIRED)
find_package(gz-common5 REQUIRED)



gz_find_package(gz-plugin2 REQUIRED COMPONENTS register)
#gz_find_package(gz-common5 REQUIRED COMPONENTS register)
set(GZ_PLUGIN_VER ${gz-plugin2_VERSION_MAJOR})

# Add sources for each plugin to be registered.
add_library(V4lSimulator SHARED V4lSimulator.cc V4lVideoEncoder.cpp)
set_property(TARGET V4lSimulator PROPERTY CXX_STANDARD 17)
target_link_libraries(V4lSimulator
  gz-common5::gz-common5
  gz-plugin${GZ_PLUGIN_VER}::gz-plugin${GZ_PLUGIN_VER}
  gz-sim7-rendering
  gz-common5-av
)

