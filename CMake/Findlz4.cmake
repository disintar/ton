include(FindPackageHandleStandardArgs)

if (LZ4_INCLUDE_DIRS AND NOT EXISTS "${LZ4_INCLUDE_DIRS}")
  unset(LZ4_INCLUDE_DIRS CACHE)
  unset(LZ4_INCLUDE_DIRS)
endif()

if (LZ4_INCLUDE_DIR AND NOT EXISTS "${LZ4_INCLUDE_DIR}")
  unset(LZ4_INCLUDE_DIR CACHE)
  unset(LZ4_INCLUDE_DIR)
endif()

if (LZ4_LIBRARIES AND NOT EXISTS "${LZ4_LIBRARIES}")
  unset(LZ4_LIBRARIES CACHE)
  unset(LZ4_LIBRARIES)
endif()

if (LZ4_LIBRARY AND NOT EXISTS "${LZ4_LIBRARY}")
  unset(LZ4_LIBRARY CACHE)
  unset(LZ4_LIBRARY)
endif()

if (LZ4_SOURCE_DIR AND EXISTS "${LZ4_SOURCE_DIR}/lib/lz4.h")
  set(LZ4_INCLUDE_HINTS ${LZ4_SOURCE_DIR}/lib)
else()
  set(LZ4_INCLUDE_HINTS)
endif()

if (LZ4_BINARY_DIR AND EXISTS "${LZ4_BINARY_DIR}/lib")
  set(LZ4_LIBRARY_HINTS ${LZ4_BINARY_DIR}/lib)
else()
  set(LZ4_LIBRARY_HINTS)
endif()

if (NOT LZ4_LIBRARIES OR NOT LZ4_INCLUDE_DIRS)
  find_path(LZ4_INCLUDE_DIRS
    NAMES lz4.h
    HINTS ${LZ4_INCLUDE_HINTS}
  )
  find_library(LZ4_LIBRARIES
    NAMES lz4 lz4_static
    HINTS ${LZ4_LIBRARY_HINTS}
  )
endif()

set(LZ4_INCLUDE_DIR ${LZ4_INCLUDE_DIRS})
set(LZ4_LIBRARY ${LZ4_LIBRARIES})

find_package_handle_standard_args(LZ4 REQUIRED_VARS LZ4_LIBRARIES LZ4_INCLUDE_DIRS)
set(lz4_FOUND ${LZ4_FOUND})

if (LZ4_FOUND)
  if (NOT TARGET lz4::lz4)
    add_library(lz4::lz4 UNKNOWN IMPORTED)
    set_target_properties(lz4::lz4 PROPERTIES
      IMPORTED_LOCATION "${LZ4_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIRS}"
    )
    if (TARGET lz4)
      add_dependencies(lz4::lz4 lz4)
    endif()
  endif()

  if (NOT TARGET LZ4::LZ4)
    add_library(LZ4::LZ4 UNKNOWN IMPORTED)
    set_target_properties(LZ4::LZ4 PROPERTIES
      IMPORTED_LOCATION "${LZ4_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIRS}"
    )
    if (TARGET lz4)
      add_dependencies(LZ4::LZ4 lz4)
    endif()
  endif()
endif()
