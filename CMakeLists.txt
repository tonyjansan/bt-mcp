# CMakeLists.txt —— BT-MCP 主构建（v0.8 B5）
#
# 三种接入方式（都在 README 里给了最小示例）：
#   1) add_subdirectory(bt-mcp)      → 目标名 bt-mcp::bt_mcp
#   2) find_package(bt_mcp CONFIG)   → 安装后用（见 install 段）
#   3) pkg-config --cflags --libs bt-mcp
#
# 设计取舍：
#   - 库本身是**纯 C99、无 OS 依赖、无动态分配**，所以没有链接选项、
#     没有线程依赖、没有 libc 格式化依赖（writer 已自研整数格式化）。
#   - 配置项全部通过 target_compile_definitions 暴露，宿主可覆盖
#     （例如 BT_MCP_TIER、BT_MCP_CFG_FCS16），而不是让用户改头文件。
cmake_minimum_required(VERSION 3.16)
# 版本号单一来源：读 VERSION 文件（tools/check_docs.py 也读它）
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BT_MCP_VERSION_RAW)
string(STRIP "${BT_MCP_VERSION_RAW}" BT_MCP_VERSION)
if(NOT BT_MCP_VERSION)
    message(FATAL_ERROR "VERSION 文件缺失或为空")
endif()
project(bt_mcp VERSION ${BT_MCP_VERSION} LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BT_MCP_BUILD_TESTS "构建单元测试" OFF)
option(BT_MCP_BUILD_TOOLS "构建 mock / oracle 辅助工具" OFF)

# ---- 配置项（默认全部走 core/include/bt_mcp/config.h 的默认值）----
set(BT_MCP_TIER "" CACHE STRING "内存档位：COMPACT / STANDARD / FULL（空=默认 STANDARD）")
set(BT_MCP_FCS16 OFF CACHE BOOL "启用 FCS16 链路校验（通道无校验时才需要）")
set(BT_MCP_SHARED_REASM OFF CACHE BOOL "共享重组缓冲（省 RAM，牺牲并发重组）")
set(BT_MCP_MAX_MSG "" CACHE STRING "单条 MCP 消息上限（空=按档位默认）")

# ---- 核心库 ----
add_library(bt_mcp STATIC
    core/src/frame.c
    core/src/session.c
    core/src/json.c
    core/src/writer.c
    core/src/mcp_server.c
    core/src/mcp_client.c
    core/src/err.c
    core/src/trace.c
)
add_library(bt-mcp::bt_mcp ALIAS bt_mcp)

target_include_directories(bt_mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(bt_mcp PUBLIC c_std_99)
# 严格告警：库自身零告警是项目约定，消费者不受影响（PRIVATE）
target_compile_options(bt_mcp PRIVATE -Wall -Wextra)

if(BT_MCP_TIER)
    target_compile_definitions(bt_mcp PUBLIC BT_MCP_TIER=BT_MCP_TIER_${BT_MCP_TIER})
endif()
if(BT_MCP_FCS16)
    target_compile_definitions(bt_mcp PUBLIC BT_MCP_CFG_FCS16=1)
endif()
if(BT_MCP_SHARED_REASM)
    target_compile_definitions(bt_mcp PUBLIC BT_MCP_CFG_SHARED_REASM=1)
endif()
if(BT_MCP_MAX_MSG)
    target_compile_definitions(bt_mcp PUBLIC BT_MCP_CFG_MAX_MSG=${BT_MCP_MAX_MSG})
endif()

# ---- 测试（可选）----
if(BT_MCP_BUILD_TESTS)
    enable_testing()
    set(BT_MCP_TESTS test_m2 test_reliability test_lossy test_fcs16
        test_conformance test_client test_edges test_l5_edges test_json_corpus
        test_fault test_matrix test_branches test_multisession test_reentrancy)
    foreach(t ${BT_MCP_TESTS})
        add_executable(${t} tests/${t}.c)
        target_link_libraries(${t} PRIVATE bt_mcp)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    # 多线程压测单独处理（需要 pthread）
    find_package(Threads REQUIRED)
    add_executable(test_stress tests/test_stress.c)
    target_link_libraries(test_stress PRIVATE bt_mcp Threads::Threads)
    add_test(NAME test_stress COMMAND test_stress)
endif()

# ---- 安装与 pkg-config（`find_package` / `pkg-config` 两条路）----
install(TARGETS bt_mcp EXPORT bt_mcpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY core/include/bt_mcp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY port/bt_mcp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT bt_mcpTargets
        FILE bt_mcpTargets.cmake
        NAMESPACE bt-mcp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bt_mcp)

configure_package_config_file(
    cmake/bt_mcpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/bt_mcpConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bt_mcp)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/bt_mcpConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/bt_mcpConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/bt_mcpConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bt_mcp)

configure_file(cmake/bt-mcp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/bt-mcp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bt-mcp.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
