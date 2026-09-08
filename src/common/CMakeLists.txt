add_library(common STATIC logging.cpp logitem.cpp meta.cpp)
target_link_libraries(common PUBLIC graphmini_headers)
