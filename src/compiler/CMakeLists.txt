add_subdirectory(scheduling)

add_library(graphmini_codegen STATIC
    compiler.cpp planning.cpp ir.cpp execution_ir.cpp
    vertex_induced.cpp edge_induced.cpp edge_induced_iep.cpp
    passes/auxiliary_graphs.cpp passes/lower_execution.cpp passes/lower_minigraphs.cpp
    passes/lower_loops.cpp
    passes/lower_bitmap.cpp
    passes/lower_iep_bitmap.cpp
    passes/representation.cpp
    codegen/cpp.cpp codegen/openmp.cpp codegen/tbb.cpp codegen/format.cpp)
get_filename_component(GRAPHMINI_PYTHON_BIN "${Python3_EXECUTABLE}" DIRECTORY)
find_program(GRAPHMINI_CLANG_FORMAT_EXECUTABLE NAMES clang-format
    HINTS "${GRAPHMINI_PYTHON_BIN}")
if(GRAPHMINI_CLANG_FORMAT_EXECUTABLE)
    target_compile_definitions(graphmini_codegen PRIVATE
        GRAPHMINI_CLANG_FORMAT="${GRAPHMINI_CLANG_FORMAT_EXECUTABLE}")
endif()
target_link_libraries(graphmini_codegen PUBLIC graphmini_headers PRIVATE common graphpi_scheduler graphmini_scheduler fmt::fmt)
