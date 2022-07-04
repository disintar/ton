cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)


if (NOT OPENSSL_FOUND)
    find_package(OpenSSL REQUIRED)
endif ()

set(INDEXER_SOURCE indexer.cpp json.hpp json-utils.hpp)

add_executable(blockchain-indexer ${INDEXER_SOURCE})

file(COPY ../crypto/fift/lib DESTINATION ./)

target_include_directories(blockchain-indexer PUBLIC)
target_link_libraries(blockchain-indexer overlay tdutils tdactor adnl tl_api dht
        rldp catchain validatorsession full-node validator-disk validator-hardfork ton_validator
        validator-hardfork fift-lib memprof ${JEMALLOC_LIBRARIES})

install(TARGETS blockchain-indexer RUNTIME DESTINATION bin)
