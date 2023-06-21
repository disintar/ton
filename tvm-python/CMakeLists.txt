cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

find_package(Python REQUIRED Development)
find_package(OpenSSL REQUIRED)
find_package(Secp256k1 REQUIRED)
find_package(Sodium REQUIRED)

add_library(combined INTERFACE)
target_link_libraries(combined INTERFACE
        tonlib overlay tdutils tdactor adnl tl_api dht
        rldp catchain validatorsession full-node emulator emulator_static validator-disk validator-hardfork ton_validator
        validator-hardfork fift-lib memprof)
bundle_static_library_deps(combined combined_bundled)

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
target_link_libraries(python_ton PRIVATE
        combined_bundled tdutils crc32c ${BLST_LIB}
        ${SECP256K1_LIBRARY} ${JEMALLOC_LIBRARIES} OpenSSL::Crypto
        ${SODIUM_LIBRARY_RELEASE})

install(TARGETS python_ton
        COMPONENT tvm-python
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        EXCLUDE_FROM_ALL)
