cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

cmake_policy(SET CMP0074 NEW) # find_package() uses <PackageName>_ROOT variables
cmake_policy(SET CMP0094 NEW) # set(Python_FIND_STRATEGY LOCATION)

set(Python_ROOT ${Python${version}_ROOT})
set(Python_EXECUTABLE ${Python${version}_EXECUTABLE})
set(Python_INCLUDE_DIR ${Python${version}_INCLUDE_DIR})
set(Python_LIBRARY ${Python${version}_LIBRARY})
find_package(Python ${version} COMPONENTS Interpreter Development REQUIRED)

message("Add pybind11 (${Python_SOABI})")
add_subdirectory(../third-party/pybind11 third-party/pybind11-${Python_SOABI} EXCLUDE_FROM_ALL)

set(PYTHON_TVM_SOURCE tvm_python.cpp)
set(PYTHON_TVM_VERSION 0.33)

file(COPY ../crypto/fift/lib DESTINATION ./)

pybind11_add_module(tvm_python-${Python_SOABI} ${PYTHON_TVM_SOURCE})
set_target_properties(tvm_python-${Python_SOABI} PROPERTIES OUTPUT_NAME tvm_python)
target_compile_features(tvm_python-${Python_SOABI} PRIVATE cxx_std_17)
target_link_libraries(tvm_python-${Python_SOABI} PRIVATE overlay tdutils tdactor adnl tl_api dht
        rldp catchain validatorsession full-node validator-disk validator-hardfork ton_validator
        validator-hardfork fift-lib memprof ${JEMALLOC_LIBRARIES})