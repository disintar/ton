
set(LITESERVER_DAEMON_SOURCE lite-server-daemon.cpp lite-server-config.hpp ../blockchain-indexer/json.hpp)
set(LITEPROXY_SOURCE adnl-lite-proxy.cpp lite-server-config.hpp ../blockchain-indexer/json.hpp)

add_executable(lite-server ${LITESERVER_DAEMON_SOURCE})
add_executable(lite-proxy ${LITEPROXY_SOURCE})

target_link_libraries(lite-server overlay tdutils tdactor adnl tl_api dht
        rldp rldp2 catchain validatorsession full-node validator-disk validator ton_validator validator
        fift-lib memprof git ${JEMALLOC_LIBRARIES} cppkafka)

target_link_libraries(lite-proxy overlay tdutils tdactor adnl tl_api dht
        rldp rldp2 catchain validatorsession full-node validator-disk validator ton_validator validator
        fift-lib memprof git ${JEMALLOC_LIBRARIES} cppkafka)

install(TARGETS lite-server RUNTIME DESTINATION bin)