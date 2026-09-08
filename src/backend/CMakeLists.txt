add_library(graphmini_backend INTERFACE)
target_link_libraries(graphmini_backend INTERFACE graphmini_headers OpenMP::OpenMP_CXX TBB::tbb TBB::tbbmalloc)
