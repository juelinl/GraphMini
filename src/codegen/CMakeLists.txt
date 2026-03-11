add_library(codegen STATIC
        codegen.cpp
        ir.cpp)

target_link_libraries(codegen PRIVATE common graphpi_scheduler fmt::fmt)
