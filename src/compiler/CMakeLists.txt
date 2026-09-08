add_subdirectory(scheduling)

add_library(graphmini_codegen STATIC
    compiler.cpp planning.cpp ir.cpp execution_ir.cpp
    vertex_induced.cpp edge_induced.cpp edge_induced_iep.cpp
    passes/auxiliary_graphs.cpp passes/lower_execution.cpp passes/lower_minigraphs.cpp
    passes/lower_loops.cpp
    passes/lower_bitmap.cpp
    passes/representation.cpp
    codegen/cpp.cpp codegen/openmp.cpp codegen/tbb.cpp)
target_link_libraries(graphmini_codegen PUBLIC graphmini_headers PRIVATE common graphpi_scheduler graphmini_scheduler fmt::fmt)
