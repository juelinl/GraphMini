add_subdirectory(codegen)
add_subdirectory(preprocess)
add_subdirectory(codegen_output)

add_library(graphpi_scheduler STATIC
        graphpi_scheduler.cpp)

add_library(graphmini_scheduler STATIC
        graphmini_scheduler.cpp)
target_link_libraries(graphmini_scheduler PRIVATE TBB::tbb)

add_library(common STATIC
        logging.cpp
        logitem.cpp
        meta.cpp)

# graph converter
add_executable(prep prep.cpp)
target_link_libraries(prep PRIVATE preprocess common cxxopts::cxxopts)

# benchmark executable
add_executable(runner runner.cpp)
target_link_libraries(runner PRIVATE common plan)

# profiling executable
add_executable(prof_runner prof_runner.cpp)
target_link_libraries(prof_runner PRIVATE common plan_profile)

# benchmark executable frontend
add_executable(run run.cpp)
target_link_libraries(run PRIVATE codegen cxxopts::cxxopts fmt::fmt TBB::tbb)

# profile executable frontend
add_executable(profile profile.cpp)
target_link_libraries(profile PRIVATE common codegen cxxopts::cxxopts fmt::fmt TBB::tbb TBB::tbbmalloc ${CMAKE_DL_LIBS})
