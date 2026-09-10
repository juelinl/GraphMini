# Optional ahead-of-time cache population. Each source owns its object file;
# unlike runtime compile_plan, these targets can safely build in parallel.
option(GRAPHMINI_PRECOMPILED_PLANS "Enable generated bitmap catalog build targets" OFF)
if(NOT GRAPHMINI_PRECOMPILED_PLANS)
    return()
endif()
if(GRAPHMINI_EXPERIMENTAL_BACKEND_MODULE OR GRAPHMINI_EXPERIMENTAL_TBB_MODULE OR
   GRAPHMINI_EXPERIMENTAL_HEADER_UNITS OR GRAPHMINI_EXPERIMENTAL_NO_INLINE OR
   GRAPHMINI_PROFILE_QUERY_COMPILATION)
    message(FATAL_ERROR "Bitmap catalog bulk builds currently require the standard PCH configuration")
endif()
# The index selects active variants. Old bitmap source/cache artifacts can be
# retained without continuing to build them after IEP becomes preferred.
file(GLOB catalog_indexes CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/plans/*/index.json")
set(catalog_sources "")
foreach(index IN LISTS catalog_indexes)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${index}")
    file(READ "${index}" catalog_json)
    string(JSON entry_count LENGTH "${catalog_json}" entries)
    get_filename_component(size_dir "${index}" DIRECTORY)
    if(entry_count EQUAL 0)
        continue()
    endif()
    math(EXPR last_entry "${entry_count} - 1")
    foreach(i RANGE 0 ${last_entry})
        string(JSON source_file ERROR_VARIABLE source_error GET "${catalog_json}" entries ${i} source_file)
        if(source_error)
            # Migration from the original bitmap-only catalog.
            string(JSON selected GET "${catalog_json}" entries ${i} bitmap_selected)
            if(NOT selected)
                continue()
            endif()
            set(source_file "bitmap.cpp")
        endif()
        string(JSON schedule_id GET "${catalog_json}" entries ${i} schedule_id)
        list(APPEND catalog_sources "${size_dir}/${schedule_id}/${source_file}")
    endforeach()
endforeach()
set(catalog_cache "${PROJECT_BINARY_DIR}/python_plan_cache/tbb-${TBB_VERSION}/${GRAPHMINI_PLAN_BUILD_ID}")
add_custom_target(bitmap_catalog)
foreach(source IN LISTS catalog_sources)
    file(SHA256 "${source}" source_hash)
    set(target "bitmap_plan_${source_hash}")
    if(TARGET "${target}")
        continue()
    endif()
    add_library("${target}" SHARED EXCLUDE_FROM_ALL "${source}")
    target_link_libraries("${target}" PRIVATE graphmini_backend)
    target_include_directories("${target}" PRIVATE "${PROJECT_SOURCE_DIR}/src/codegen_output")
    # Match the runtime module's preprocessor configuration when reusing PCH.
    set_target_properties("${target}" PROPERTIES DEFINE_SYMBOL plan_module_EXPORTS
        PREFIX "" OUTPUT_NAME "${source_hash}"
        LIBRARY_OUTPUT_DIRECTORY "${catalog_cache}"
        RUNTIME_OUTPUT_DIRECTORY "${catalog_cache}")
    target_precompile_headers("${target}" REUSE_FROM plan_module)
    add_dependencies(bitmap_catalog "${target}")
endforeach()
