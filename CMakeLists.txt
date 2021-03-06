CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

SET(CMAKE_C_FLAGS "-Wall -Wextra -Wno-unused-parameter -g -O3 -std=gnu11 -march=native")
SET(CMAKE_CXX_FLAGS "-Wall -Wextra -Wno-unused-parameter -g -O3 -std=gnu++11 -march=native")

SET(DPDK_LIBS
	rte_ethdev     rte_mbuf    rte_eal     rte_kvargs rte_ring  rte_mempool
	rte_pmd_virtio rte_cfgfile rte_hash    rte_meter  rte_sched rte_cmdline
	rte_port       rte_net     rte_ip_frag rte_mempool_ring
)
SET(LINKER_OPTS -Wl,--whole-archive -Wl,--start-group ${DPDK_LIBS} -Wl,--end-group pthread dl rt m -Wl,--no-whole-archive)
INCLUDE_DIRECTORIES(
	./dpdk/build/include
)
LINK_DIRECTORIES(
	${CMAKE_CURRENT_SOURCE_DIR}/dpdk/build/lib
)

# router
SET(PRJ router)
SET(SOURCES dpdk_init.c router.c routing_table.c ethernet_stack.c arp_stack.c ipv4_stack.c)
ADD_EXECUTABLE(${PRJ} ${SOURCES} main.c)
TARGET_LINK_LIBRARIES(${PRJ} ${LINKER_OPTS})

# forwarder
SET(PRJ fwd)
ADD_EXECUTABLE(${PRJ} dpdk_init.c forwarder/fwd.c)
TARGET_LINK_LIBRARIES(${PRJ} ${LINKER_OPTS})

# test
SET(PRJ-TEST table-test)
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})
ADD_EXECUTABLE(${PRJ-TEST} ${SOURCES} test/test.cc)
TARGET_LINK_LIBRARIES(${PRJ-TEST} -Wl,--start-group ${DPDK_LIBS} ${GTEST_LIBRARIES} -Wl,--end-group pthread dl rt)

