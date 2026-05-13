cmake_minimum_required(VERSION 3.10)
project(netmon C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

add_executable(netmon main.c nft_monitor.c database.c)
target_link_libraries(netmon ubox blobmsg_json ubus json-c nftnl mnl sqlite3)

install(TARGETS netmon RUNTIME DESTINATION bin)