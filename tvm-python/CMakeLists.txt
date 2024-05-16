cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
set(OPENSSL_USE_STATIC_LIBS TRUE)

if (NOT Python_FOUND)
    find_package(Python REQUIRED Development)
endif ()
find_package(OpenSSL REQUIRED)
if (NOT SECP256K1_FOUND)
    find_package(Secp256k1 REQUIRED)
endif ()
if (NOT SODIUM_FOUND)
    find_package(Sodium REQUIRED)
endif ()

add_library(combined INTERFACE)
target_link_libraries(combined INTERFACE
        tonlib overlay tdutils tdactor adnl tl_api dht terminal
        rldp catchain validatorsession full-node emulator emulator_static
        validator-disk validator-hardfork ton_validator memprof)
bundle_static_library_deps(combined combined_bundled)

get_target_property(deps combined INTERFACE_LINK_LIBRARIES)
set(include_dirs)

foreach (target ${deps})
    get_target_property(target_includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if (target_includes)
        list(APPEND include_dirs ${target_includes})
    endif ()
endforeach ()

set(PYTHON_EMULATOR_SOURCE
        PyCellSlice.cpp
        PyCellSlice.h
        PyCell.cpp
        PyCell.h
        PyCellBuilder.cpp
        PyCellBuilder.h
        PyDict.cpp
        PyDict.h
        PyTools.h
        PyTools.cpp
        PyTVM.cpp
        PyTVM.h
        PyFift.cpp
        PyFift.h
        PyStack.h
        PyStack.cpp
        PyEmulator.cpp
        PyEmulator.h
        PySmcAddress.cpp
        PySmcAddress.h
        PyKeys.cpp
        PyKeys.h
        PyLiteClient.cpp
        PyLiteClient.h
        python_ton.cpp)

pybind11_add_module(python_ton ${PYTHON_EMULATOR_SOURCE})

target_link_libraries(python_ton PRIVATE -static-libstdc++
        combined_bundled fift-lib
        tlbc-lib ton_crypto_core
        ${BLST_LIB} ${SECP256K1_LIBRARY} ${JEMALLOC_LIBRARIES}
        OpenSSL::Crypto ${SODIUM_LIBRARY_RELEASE})

target_include_directories(python_ton PRIVATE ${include_dirs})


set(PYTHON_FUNC_SOURCE
        PyFunc.cpp
        PyFunc.h
        python_func.cpp)

pybind11_add_module(python_func ${PYTHON_FUNC_SOURCE})

target_link_libraries(python_func PRIVATE combined_bundled func-lib)

target_include_directories(python_func PRIVATE ${include_dirs})

install(TARGETS python_ton python_func
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT tvm-python
    EXCLUDE_FROM_ALL)

