cmake_minimum_required(VERSION 3.10)
project(TelegramForwardBot)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加编译选项来抑制 Boost 警告
add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)

# 查找必要的包
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(CURL REQUIRED)

# 添加 tgbot-cpp
include(FetchContent)
FetchContent_Declare(
    tgbot
    GIT_REPOSITORY https://github.com/reo7sp/tgbot-cpp.git
    GIT_TAG master
)
FetchContent_MakeAvailable(tgbot)

# 创建可执行文件
add_executable(telegram_forward_bot telegram_forward_bot.cpp)

# 链接库
target_link_libraries(telegram_forward_bot 
    TgBot 
    ${CMAKE_THREAD_LIBS_INIT} 
    ${OPENSSL_LIBRARIES} 
    ${Boost_LIBRARIES} 
    ${CURL_LIBRARIES}
)

# 包含目录
target_include_directories(telegram_forward_bot PRIVATE 
    ${OPENSSL_INCLUDE_DIR} 
    ${Boost_INCLUDE_DIR}
)
