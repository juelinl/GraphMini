add_library(graph_runtime STATIC plan_module.cpp graph_builder.cpp)
target_link_libraries(graph_runtime PUBLIC common graphmini_backend ${CMAKE_DL_LIBS})
