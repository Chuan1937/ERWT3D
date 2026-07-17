cmake_minimum_required(VERSION 3.14)
project(generator VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Release)

if(WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 -w")
endif()

# 自动处理Qt的MOC、UIC、RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 查找Qt包
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# 添加源文件
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/DataGenerator.cpp
)

# 添加头文件
set(HEADERS
    include/MainWindow.h
    include/DataGenerator.h
)

# 创建可执行文件
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})
else()
    add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
endif()

# 包含头文件目录
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 链接Qt库
target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)

# Windows平台特定设置
if(WIN32)
    # 设置应用程序图标（可选）
    # set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_SOURCE_DIR}/resources/app.rc")
    # target_sources(${PROJECT_NAME} PRIVATE ${APP_ICON_RESOURCE_WINDOWS})
endif()

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# 输出构建信息
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Qt5 found: ${Qt5_FOUND}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
