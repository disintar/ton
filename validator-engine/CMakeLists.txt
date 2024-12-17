cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
    find_package(OpenSSL REQUIRED)
endif ()

if (NIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MHD REQUIRED libmicrohttpd)
else()
    find_package(MHD REQUIRED)
endif()

set(VALIDATOR_ENGINE_SOURCE
        prometheus/PrometheusExporterActor.h
        prometheus/PrometheusExporterActor.cpp
        ClusterSyncer.hpp
        ClusterSyncer.cpp
        validator-engine.hpp
        validator-engine.cpp
        IBlockParser.hpp
        IBlockParser.cpp
        BlockParserAsync.cpp
        BlockParserAsync.hpp
        BlockPublisherKafka.hpp
        BlockPublisherKafka.cpp
        IBlockRequestReceiver.hpp
        BlockRequestReceiverKafka.hpp
        BlockRequestReceiverKafka.cpp)

add_executable(validator-engine ${VALIDATOR_ENGINE_SOURCE})
target_link_libraries(validator-engine overlay tdutils tdactor adnl tl_api dht
  rldp rldp2 catchain validatorsession full-node validator ton_validator validator
  fift-lib memprof git ${JEMALLOC_LIBRARIES} ${MHD_LIBRARY})

target_include_directories(validator-engine PUBLIC ${MHD_INCLUDE_DIR})

if (JEMALLOC_FOUND)
  target_include_directories(validator-engine PRIVATE ${JEMALLOC_INCLUDE_DIR})
  target_compile_definitions(validator-engine PRIVATE -DTON_USE_JEMALLOC=1)
endif()

# TODO: option or smth
#target_include_directories(validator-engine PRIVATE ${CPPKAFKA_INCLUDE_DIRECTORIES})
target_link_libraries(validator-engine cppkafka)


install(TARGETS validator-engine RUNTIME DESTINATION bin)
