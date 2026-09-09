cmake_minimum_required(VERSION 3.20)
# Also usable standalone: no Python, TBB or OpenMP dependency for this backend.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(GraphMiniMetal LANGUAGES CXX OBJCXX)
    enable_testing()
endif()
if(NOT APPLE)
    message(FATAL_ERROR "GraphMini Metal requires macOS")
endif()
enable_language(OBJCXX)
find_program(GRAPHMINI_XCRUN xcrun REQUIRED)
set(metallib "${CMAKE_CURRENT_BINARY_DIR}/graphmini.metallib")
add_custom_command(OUTPUT "${metallib}"
    COMMAND "${GRAPHMINI_XCRUN}" -sdk macosx metal -std=metal3.1 -O3 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/clique.metal" -o "${CMAKE_CURRENT_BINARY_DIR}/clique.air"
    COMMAND "${GRAPHMINI_XCRUN}" -sdk macosx metallib
        "${CMAKE_CURRENT_BINARY_DIR}/clique.air" -o "${metallib}"
    DEPENDS clique.metal VERBATIM)
add_custom_target(graphmini_metal_shaders DEPENDS "${metallib}")
add_library(graphmini_metal STATIC backend.mm)
add_dependencies(graphmini_metal graphmini_metal_shaders)
set_target_properties(graphmini_metal PROPERTIES CXX_STANDARD 17 OBJCXX_STANDARD 17)
target_compile_options(graphmini_metal PRIVATE -fobjc-arc)
target_include_directories(graphmini_metal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(graphmini_metal PRIVATE GRAPHMINI_METALLIB="${metallib}")
target_link_libraries(graphmini_metal PUBLIC "-framework Metal" "-framework Foundation")
add_executable(graphmini_metal_cli main.cpp)
target_compile_features(graphmini_metal_cli PRIVATE cxx_std_17)
target_link_libraries(graphmini_metal_cli PRIVATE graphmini_metal)
add_test(NAME metal_device_smoke COMMAND graphmini_metal_cli --self-test)
add_executable(graphmini_metal_test "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/metal.cpp")
target_compile_features(graphmini_metal_test PRIVATE cxx_std_17)
target_link_libraries(graphmini_metal_test PRIVATE graphmini_metal)
add_test(NAME metal_correctness COMMAND graphmini_metal_test)
