cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(VALIDATOR_ENGINE_SOURCE
  validator-engine.hpp
  validator-engine.cpp
  IBlockPublisher.hpp
  IBlockPublisher.cpp
#  BlockPublisherZMQ.cpp BlockPublisherZMQ.hpp
#  BlockPublisherRMQ.cpp BlockPublisherRMQ.hpp
  BlockPublisherKafka.cpp BlockPublisherKafka.hpp
)

add_executable(validator-engine ${VALIDATOR_ENGINE_SOURCE})
target_link_libraries(validator-engine overlay tdutils tdactor adnl tl_api dht
  rldp catchain validatorsession full-node validator ton_validator validator
  fift-lib memprof git ${JEMALLOC_LIBRARIES})

# TODO: option or smth
#find_package(Threads)
#find_package(cppzmq)
#target_link_libraries(validator-engine cppzmq Threads::Threads)

# TODO: option or smth
#target_include_directories(validator-engine PRIVATE ${AMQPCPP_INCLUDE_DIRECTORIES})
#target_link_libraries(validator-engine amqpcpp)

# TODO: option or smth
#target_include_directories(validator-engine PRIVATE ${CPPKAFKA_INCLUDE_DIRECTORIES})
target_link_libraries(validator-engine cppkafka)


install(TARGETS validator-engine RUNTIME DESTINATION bin)
