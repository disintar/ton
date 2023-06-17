cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(LIBNAME ${CMAKE_STATIC_LIBRARY_PREFIX}combined${CMAKE_STATIC_LIBRARY_SUFFIX})
add_custom_command(
        OUTPUT ${LIBNAME}
        # COMMAND ${CMAKE_CXX_ARCHIVE_CREATE} ${LIBNAME}
        COMMAND ${CMAKE_AR} qc ${LIBNAME}
                 $<TARGET_FILE:tonlib>
                 $<TARGET_FILE:overlay>
                 $<TARGET_FILE:tdutils>
                 $<TARGET_FILE:tdactor>
                 $<TARGET_FILE:adnl>
                 $<TARGET_FILE:tl_api>
                 $<TARGET_FILE:dht>
                 $<TARGET_FILE:rldp>
                 $<TARGET_FILE:catchain>
                 $<TARGET_FILE:validatorsession>
                 $<TARGET_FILE:full-node>
                 $<TARGET_FILE:emulator>
                 $<TARGET_FILE:emulator_static>
                 $<TARGET_FILE:validator-disk>
                 $<TARGET_FILE:validator-hardfork>
                 $<TARGET_FILE:ton_validator>
                 $<TARGET_FILE:validator-hardfork>
                 $<TARGET_FILE:fift-lib>
                 $<TARGET_FILE:memprof>
        DEPENDS tonlib overlay tdutils tdactor adnl tl_api dht
                 rldp catchain validatorsession full-node emulator emulator_static validator-disk validator-hardfork ton_validator
                 validator-hardfork fift-lib memprof
        COMMENT "Creating combined static library")
add_custom_target(combined_target DEPENDS ${LIBNAME})
add_library(combined STATIC IMPORTED)
set_property(TARGET combined PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/${LIBNAME})
add_dependencies(combined combined_target)


set(PYTHON_EMULATOR_SOURCE
        PyCellSlice.cpp
        PyCellSlice.h
        PyCell.cpp
        PyCell.h
        PyCellBuilder.cpp
        PyCellBuilder.h
        PyDict.cpp
        PyDict.h
        PyEmulator.cpp
        PyEmulator.h
        python_ton.cpp)

pybind11_add_module(python_ton ${PYTHON_EMULATOR_SOURCE})
target_link_libraries(python_ton PRIVATE combined ${JEMALLOC_LIBRARIES})
target_include_directories(python_ton PRIVATE
        $<TARGET_PROPERTY:tonlib,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:overlay,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:tdutils,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:tdactor,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:adnl,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:tl_api,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:dht,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:rldp,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:catchain,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:validatorsession,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:full-node,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:emulator,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:emulator_static,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:validator-disk,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:validator-hardfork,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:ton_validator,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:validator-hardfork,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:fift-lib,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:memprof,INTERFACE_INCLUDE_DIRECTORIES>)

install(TARGETS python_ton
        COMPONENT tvm-python
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        EXCLUDE_FROM_ALL)
