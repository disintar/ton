cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

find_package(Python REQUIRED Development)

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
target_link_libraries(python_ton PRIVATE tonlib overlay tdutils tdactor adnl tl_api dht
        rldp catchain validatorsession full-node emulator emulator_static validator-disk validator-hardfork ton_validator
        validator-hardfork fift-lib memprof ${JEMALLOC_LIBRARIES})

install(TARGETS python_ton
        COMPONENT tvm-python
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        EXCLUDE_FROM_ALL)
