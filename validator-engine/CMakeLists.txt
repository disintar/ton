cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(VALIDATOR_ENGINE_SOURCE
  validator-engine.hpp
  validator-engine.cpp
  IBlockParser.hpp
  IBlockParser.cpp
  BlockPublisherKafka.hpp BlockPublisherKafka.cpp
         IBlockRequestReceiver.hpp BlockRequestReceiverKafka.cpp BlockRequestReceiverKafka.hpp)

add_executable(validator-engine ${VALIDATOR_ENGINE_SOURCE})
target_link_libraries(validator-engine overlay tdutils tdactor adnl tl_api dht
  rldp rldp2 catchain validatorsession full-node validator ton_validator validator
  fift-lib memprof git ${JEMALLOC_LIBRARIES})

# TODO: option or smth
#target_include_directories(validator-engine PRIVATE ${CPPKAFKA_INCLUDE_DIRECTORIES})
target_link_libraries(validator-engine cppkafka)


install(TARGETS validator-engine RUNTIME DESTINATION bin)
