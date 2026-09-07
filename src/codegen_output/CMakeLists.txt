add_library(plan STATIC
        plan.cpp plan.h)
target_precompile_headers(plan PUBLIC ../backend/backend.h)
target_link_libraries(plan PUBLIC OpenMP::OpenMP_CXX TBB::tbb TBB::tbbmalloc)

if(GRAPHMINI_EXPERIMENTAL_HEADER_UNITS)
    add_library(plan_module SHARED plan_header_unit.cpp plan.h)
    set_target_properties(plan_module PROPERTIES CXX_STANDARD 20 CXX_SCAN_FOR_MODULES OFF)
    set(header_unit "${CMAKE_CURRENT_BINARY_DIR}/graphmini_backend.pcm")
    target_compile_definitions(plan_module PRIVATE GRAPHMINI_USE_HEADER_UNIT=1)
    target_compile_options(plan_module PRIVATE "-fmodule-file=${header_unit}")
    add_custom_command(OUTPUT "${header_unit}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build_header_unit.py"
            --database "${PROJECT_BINARY_DIR}/compile_commands.json"
            --header "${PROJECT_SOURCE_DIR}/src/backend/backend.h"
            --output "${header_unit}"
        DEPENDS "${PROJECT_SOURCE_DIR}/scripts/build_header_unit.py"
            "${PROJECT_BINARY_DIR}/compile_commands.json"
            "${PROJECT_SOURCE_DIR}/src/backend/backend.h"
        DEPFILE "${header_unit}.d"
        VERBATIM)
    set_source_files_properties(plan_header_unit.cpp PROPERTIES OBJECT_DEPENDS "${header_unit}")
else()
    add_library(plan_module SHARED plan.cpp plan.h)
    target_precompile_headers(plan_module PUBLIC ../backend/backend.h)
endif()
target_link_libraries(plan_module PUBLIC OpenMP::OpenMP_CXX TBB::tbb TBB::tbbmalloc)

add_library(plan_profile STATIC
        plan_profile.cpp plan_profile.h)
target_precompile_headers(plan_profile PUBLIC ../backend_prof/backend.h)
target_link_libraries(plan_profile PUBLIC OpenMP::OpenMP_CXX TBB::tbb TBB::tbbmalloc)
