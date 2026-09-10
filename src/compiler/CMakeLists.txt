add_subdirectory(scheduling)

add_library(graphmini_codegen STATIC
    compiler.cpp planning.cpp ir.cpp execution_ir.cpp
    vertex_induced.cpp edge_induced.cpp edge_induced_iep.cpp
    passes/auxiliary_graphs.cpp passes/lower_execution.cpp passes/lower_minigraphs.cpp
    passes/lower_loops.cpp
    passes/lower_bitmap.cpp
    passes/lower_iep_bitmap.cpp
    passes/representation.cpp
    codegen/cpp.cpp codegen/bitmap.cpp codegen/openmp.cpp codegen/tbb.cpp codegen/format.cpp)
get_filename_component(GRAPHMINI_PYTHON_BIN "${Python3_EXECUTABLE}" DIRECTORY)
find_program(GRAPHMINI_CLANG_FORMAT_EXECUTABLE NAMES clang-format
    HINTS "${GRAPHMINI_PYTHON_BIN}")
if(GRAPHMINI_CLANG_FORMAT_EXECUTABLE)
    target_compile_definitions(graphmini_codegen PRIVATE
        GRAPHMINI_CLANG_FORMAT="${GRAPHMINI_CLANG_FORMAT_EXECUTABLE}")
endif()
target_link_libraries(graphmini_codegen PUBLIC graphmini_headers PRIVATE common graphpi_scheduler graphmini_scheduler fmt::fmt)

# Schedule-keyed lookup must reject catalogs made by an older code generator,
# even when the runtime ABI is unchanged. Keep this separate from the binary
# build ID so unchanged generated sources can still reuse existing libraries.
file(GLOB_RECURSE codegen_identity_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(codegen_identity "schedule-cache-v1;${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${fmt_VERSION}")
foreach(input IN LISTS codegen_identity_inputs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(SHA256 "${input}" input_hash)
    string(APPEND codegen_identity ";${input_hash}")
endforeach()
if(GRAPHMINI_CLANG_FORMAT_EXECUTABLE)
    execute_process(COMMAND "${GRAPHMINI_CLANG_FORMAT_EXECUTABLE}" --version
        OUTPUT_VARIABLE formatter_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(APPEND codegen_identity ";${formatter_version}")
endif()
string(SHA256 GRAPHMINI_CODEGEN_BUILD_ID "${codegen_identity}")
configure_file(compiler_cache_id.h.in "${PROJECT_BINARY_DIR}/configure/compiler_cache_id.h" @ONLY)
