add_library(graphmini_codegen STATIC
        codegen.cpp
        ir.cpp)

target_link_libraries(graphmini_codegen PRIVATE common graphpi_scheduler graphmini_scheduler fmt::fmt)
