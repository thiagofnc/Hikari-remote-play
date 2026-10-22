# Measurement / diagnostic tools. Each is a standalone console exe written to
# the common bin dir; they share tools/common (header-only helpers).

function(hikari_tool name dir)
    add_executable(${name} ${dir}/main.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

hikari_tool(hikari-capture-probe capture_probe hikari_host_core)
hikari_tool(hikari-encoder-probe encoder_probe hikari_host_core)
# See the file header: works around a missing ICodecAPI uuid under mingw.
target_sources(hikari-encoder-probe PRIVATE common/mingw_uuid_fix.cpp)
hikari_tool(hikari-latency-test latency_test hikari_host_core)
hikari_tool(hikari-pad pad_cli hikari_host_core)
# The impairment proxy only needs sockets and timers.
hikari_tool(hikari-netsim network_sim hikari_shared)
