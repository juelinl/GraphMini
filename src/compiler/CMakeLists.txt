add_library(graphmini_codegen STATIC
    compiler.cpp planning.cpp ir.cpp
    vertex_induced.cpp edge_induced.cpp edge_induced_iep.cpp
    passes/auxiliary_graphs.cpp
    codegen/cpp.cpp codegen/openmp.cpp codegen/tbb.cpp)
target_link_libraries(graphmini_codegen PRIVATE common graphpi_scheduler graphmini_scheduler fmt::fmt)

if(GRAPHMINI_BUILD_TESTS)
    add_executable(compiler_regression ../../tests/compiler_regression.cpp)
    target_compile_definitions(compiler_regression PRIVATE GRAPHMINI_REFACTORED)
    target_link_libraries(compiler_regression PRIVATE graphmini_codegen)
    add_test(NAME compiler_regression COMMAND compiler_regression)
    add_executable(compilation_benchmark ../../tests/compilation_benchmark.cpp)
    target_link_libraries(compilation_benchmark PRIVATE graphmini_codegen)
endif()
