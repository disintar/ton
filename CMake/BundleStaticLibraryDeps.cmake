# https://gitlab.kitware.com/cmake/cmake/-/issues/19224
function(bundle_static_library_deps tgt_name bundled_tgt_name)
  function(_recursively_collect_dependencies input_target)
    set(_input_link_libraries LINK_LIBRARIES)
    get_target_property(_input_type ${input_target} TYPE)
    if (${_input_type} STREQUAL "INTERFACE_LIBRARY")
      set(_input_link_libraries INTERFACE_LINK_LIBRARIES)
    endif()
    get_target_property(public_dependencies ${input_target} ${_input_link_libraries})
    foreach(dependency IN LISTS public_dependencies)
      if(TARGET ${dependency})
        get_target_property(alias ${dependency} ALIASED_TARGET)
        if (TARGET ${alias})
          set(dependency ${alias})
        endif()
        get_target_property(_type ${dependency} TYPE)
        if (${_type} STREQUAL "STATIC_LIBRARY")
          list(APPEND static_libs ${dependency})
        endif()

        get_property(library_already_added
          GLOBAL PROPERTY _${tgt_name}_static_bundle_${dependency})
        if (NOT library_already_added)
          set_property(GLOBAL PROPERTY _${tgt_name}_static_bundle_${dependency} ON)
          _recursively_collect_dependencies(${dependency})
        endif()
      endif()
    endforeach()
    set(static_libs ${static_libs} PARENT_SCOPE)
  endfunction()

  _recursively_collect_dependencies(${tgt_name})

  list(REMOVE_DUPLICATES static_libs)
  
  message(STATUS "Bundling ${tgt_name} with ${static_libs}")

  set(bundled_tgt_full_name 
    ${CMAKE_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}${bundled_tgt_name}${CMAKE_STATIC_LIBRARY_SUFFIX})

  foreach(tgt IN LISTS static_libs)
    list(APPEND static_libs_full_names $<TARGET_FILE:${tgt}>)
  endforeach()

  set(ar_tool ${CMAKE_AR})
  if (CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(ar_tool ${CMAKE_CXX_COMPILER_AR})
  endif()

  if (CMAKE_CXX_COMPILER_ID MATCHES "^GNU$")
    file(WRITE ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in
      "CREATE ${bundled_tgt_full_name}\n" )
        
    foreach(tgt IN LISTS static_libs)
      file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in
        "ADDLIB $<TARGET_FILE:${tgt}>\n")
    endforeach()
    
    file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in "SAVE\n")
    file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in "END\n")

    file(GENERATE
      OUTPUT ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar
      INPUT ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in)

    add_custom_command(
      COMMAND ${ar_tool} -M < ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar
      OUTPUT ${bundled_tgt_full_name}
      DEPENDS ${static_libs}
      COMMENT "Bundling ${bundled_tgt_name}"
      VERBATIM)
  elseif (CMAKE_CXX_COMPILER_ID MATCHES "^Clang$")
    set(temp_dirs "")

    foreach(lib ${static_libs})
      get_filename_component(lib_name ${lib} NAME_WE)
      set(temp_dir "${CMAKE_CURRENT_BINARY_DIR}/${lib_name}")
      file(MAKE_DIRECTORY ${temp_dir})

      add_custom_command(OUTPUT "${temp_dir}/done"
        COMMAND ${CMAKE_AR} -x $<TARGET_FILE:${lib}>
        WORKING_DIRECTORY ${temp_dir}
        COMMAND touch "${temp_dir}/done"
        DEPENDS ${lib}
        COMMENT "Extracting library ${lib} to ${temp_dir}"
        VERBATIM
      )
      list(APPEND temp_dirs "${temp_dir}/done")
    endforeach()

    add_custom_command(OUTPUT ${bundled_tgt_full_name}
      COMMAND bash -c "${CMAKE_AR} -qc ${bundled_tgt_full_name} $(find -name \"*.o\")"
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      DEPENDS ${temp_dirs}
      COMMENT "Bundling into ${bundled_tgt_full_name}"
      VERBATIM
    )
  elseif(MSVC)
    get_filename_component(_vs_bin_path "${CMAKE_LINKER}" DIRECTORY)
    find_program(lib_tool lib HINTS ${_vs_bin_path} REQUIRED)

    add_custom_command(
      COMMAND ${lib_tool} /NOLOGO /OUT:${bundled_tgt_full_name} ${static_libs_full_names}
      OUTPUT ${bundled_tgt_full_name}
      DEPENDS ${static_libs}
      COMMENT "Bundling ${bundled_tgt_name}"
      VERBATIM)
  else()
    message(FATAL_ERROR "Unknown bundle scenario!")
  endif()

  add_custom_target(bundling_target ALL DEPENDS ${bundled_tgt_full_name})
  add_dependencies(bundling_target ${tgt_name})
  
  add_library(${bundled_tgt_name} STATIC IMPORTED)
  set_target_properties(${bundled_tgt_name} 
    PROPERTIES
      IMPORTED_LOCATION ${bundled_tgt_full_name}
      # XXX: doesn't work for INTERFACE bundling targets
      INTERFACE_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:${tgt_name},INTERFACE_INCLUDE_DIRECTORIES>)
  add_dependencies(${bundled_tgt_name} bundling_target)

endfunction()
