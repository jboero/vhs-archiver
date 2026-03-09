cmake_minimum_required(VERSION 3.20)
project(vhs-codec-ui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    Charts
    Multimedia
)

add_executable(vhs-codec-ui
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

target_link_libraries(vhs-codec-ui PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Charts
    Qt6::Multimedia
)

install(TARGETS vhs-codec-ui DESTINATION bin)
