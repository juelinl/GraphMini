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
target_link_libraries(run PRIVATE graphmini_codegen cxxopts::cxxopts fmt::fmt TBB::tbb)

# profile executable frontend
add_executable(profile profile.cpp)
target_link_libraries(profile PRIVATE common graphmini_codegen cxxopts::cxxopts fmt::fmt TBB::tbb TBB::tbbmalloc ${CMAKE_DL_LIBS})
