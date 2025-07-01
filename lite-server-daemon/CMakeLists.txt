
set(LITESERVER_DAEMON_SOURCE lite-server-daemon.cpp lite-server-config.hpp ../blockchain-indexer/json.hpp ../validator-engine/prometheus/PrometheusExporterActor.h ../validator-engine/prometheus/PrometheusExporterActor.cpp)
set(LITEPROXY_SOURCE adnl-lite-proxy.cpp lite-server-config.hpp ../blockchain-indexer/json.hpp ../validator-engine/prometheus/PrometheusExporterActor.h ../validator-engine/prometheus/PrometheusExporterActor.cpp)

add_executable(lite-server ${LITESERVER_DAEMON_SOURCE})
add_executable(lite-proxy ${LITEPROXY_SOURCE})

target_include_directories(lite-server  PUBLIC ${MHD_INCLUDE_DIR})
target_include_directories(lite-proxy PUBLIC ${MHD_INCLUDE_DIR})

target_link_libraries(lite-server overlay tdutils tdactor adnl tl_api dht
        rldp rldp2 catchain validatorsession full-node validator-disk validator ton_validator validator
        fift-lib memprof git ${JEMALLOC_LIBRARIES} cppkafka ${MHD_LIBRARY})

target_link_libraries(lite-proxy overlay tdutils tdactor adnl tl_api dht
        rldp rldp2 catchain validatorsession full-node validator-disk validator ton_validator validator
        fift-lib memprof git ${JEMALLOC_LIBRARIES} cppkafka ${MHD_LIBRARY})

install(TARGETS lite-server RUNTIME DESTINATION bin)