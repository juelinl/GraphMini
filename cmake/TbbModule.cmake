# The upstream module stays untouched. Patch only a build-directory copy of
# 2023.1.0, and fail rather than silently applying the workaround to unknown text.
if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "The experimental TBB module requires CMake >=3.28")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR "The experimental TBB module currently requires upstream Clang and Ninja")
endif()
get_target_property(tbb_include_dirs TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
find_file(tbb_module_source NAMES oneapi/tbb.cppm
    PATHS ${tbb_include_dirs} NO_DEFAULT_PATH REQUIRED NO_CACHE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tbb_module_source}")
file(READ "${tbb_module_source}" tbb_module_text)
if(TBB_VERSION VERSION_EQUAL "2023.1.0")
    foreach(symbol cache_aligned_resource scalable_memory_resource)
        set(export_line "    using tbb::v1::${symbol};")
        string(REGEX MATCHALL "using tbb::v1::${symbol}" matches "${tbb_module_text}")
        list(LENGTH matches match_count)
        string(FIND "${tbb_module_text}" "${export_line}" export_position)
        if(NOT match_count EQUAL 1 OR export_position EQUAL -1)
            message(FATAL_ERROR "Unexpected TBB 2023.1.0 module export: ${symbol}")
        endif()
        string(REPLACE "${export_line}"
            "#if __TBB_CPP17_MEMORY_RESOURCE_PRESENT\n${export_line}\n#endif"
            tbb_module_text "${tbb_module_text}")
    endforeach()
    message(STATUS "Applying build-local TBB 2023.1.0 memory-resource export guards")
endif()
set(tbb_generated_dir "${PROJECT_BINARY_DIR}/generated/tbb-module")
file(MAKE_DIRECTORY "${tbb_generated_dir}")
# Unlike file(WRITE), CONFIGURE preserves the timestamp when contents match.
file(CONFIGURE OUTPUT "${tbb_generated_dir}/tbb.cppm" CONTENT "${tbb_module_text}" @ONLY)

add_library(graphmini_tbb_module STATIC)
target_compile_features(graphmini_tbb_module PUBLIC cxx_std_20)
set_target_properties(graphmini_tbb_module PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_link_libraries(graphmini_tbb_module PUBLIC TBB::tbb TBB::tbbmalloc OpenMP::OpenMP_CXX)
target_sources(graphmini_tbb_module PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${tbb_generated_dir}" FILES "${tbb_generated_dir}/tbb.cppm")
