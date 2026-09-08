pybind11_add_module(graphmini MODULE
        python_api.cpp)
target_link_libraries(graphmini PRIVATE graphmini_codegen graph_runtime)
if(WIN32)
    set_target_properties(graphmini PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
endif()
