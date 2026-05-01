cmake_minimum_required(VERSION 3.16)
project(cct-cn LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CCT_CN_SOURCES
  src/main.cpp
  src/util/config.cpp
  src/util/paths.cpp
  src/util/json_minimal.cpp
  src/util/component_name_utf8.cpp
  src/template/loader.cpp
  src/context/scanner.cpp
  src/llm/mock_client.cpp
  src/llm/anthropic_client.cpp
  src/llm/zhipu_client.cpp
  src/generator/writer.cpp
)

if(WIN32)
  add_library(cct_crypto STATIC src/web/crypto.cpp)
  target_include_directories(cct_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(cct_crypto PRIVATE bcrypt)

  add_library(cct-storage SHARED src/storage/sql_bundle.cpp)
  target_include_directories(cct-storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(cct-storage PRIVATE cct_crypto odbc32)
  set_target_properties(cct-storage PROPERTIES OUTPUT_NAME "cct-storage")

  list(APPEND CCT_CN_SOURCES
    src/web/user_store.cpp
    src/web/http_util.cpp
    src/web/git_worker_client.cpp
    src/web/server.cpp
    src/storage/file_chat_store.cpp
    src/storage/file_component_store.cpp
    src/storage/file_token_billing.cpp
  )
endif()

add_executable(cct-cn ${CCT_CN_SOURCES})

target_include_directories(cct-cn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
  target_compile_options(cct-cn PRIVATE /utf-8)
endif()

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/templates/zh" _tpl_root)
target_compile_definitions(cct-cn PRIVATE
  CCT_CN_TEMPLATE_ROOT="${_tpl_root}"
  CCT_CN_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
  target_link_libraries(cct-cn PRIVATE winhttp ws2_32 cct_crypto cct-storage)
  if(MSVC)
    target_compile_options(cct-storage PRIVATE /utf-8)
  endif()
  # 可执行文件旁的 ui/ 才是浏览器实际访问的目录（与源码 ui/ 分离）
  add_custom_command(TARGET cct-cn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/ui"
            "$<TARGET_FILE_DIR:cct-cn>/ui"
    COMMENT "Copy ui/ next to executable")
  add_custom_command(TARGET cct-cn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:cct-storage>"
            "$<TARGET_FILE_DIR:cct-cn>/$<TARGET_FILE_NAME:cct-storage>"
    COMMENT "Copy cct-storage.dll next to cct-cn")
  add_custom_command(TARGET cct-cn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/git-worker"
            "$<TARGET_FILE_DIR:cct-cn>/git-worker"
    COMMENT "Copy git-worker next to executable (run npm install in git-worker once)")
  # 仅改动了 ui/*.html|css|js 时，VS 可能跳过链接，POST_BUILD 不会执行；此时请「生成 → ui-sync」或「重新生成 cct-cn」
  # Web IDE 打包：在 ide-app 目录执行 npm install && npm run build，生成 ui/ide/ide.js
  # 仓库同步 Monaco：在 repo-sync-app 目录执行 npm install && npm run build，生成 ui/repo-sync/
  add_custom_target(ui-sync
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/ui"
            "$<TARGET_FILE_DIR:cct-cn>/ui"
    COMMENT "Sync ui/ next to cct-cn (after editing static files)")
  add_dependencies(ui-sync cct-cn)
endif()
