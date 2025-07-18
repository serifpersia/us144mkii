cmake_minimum_required(VERSION 3.16)
project(TascamControlPanel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.2 COMPONENTS Widgets REQUIRED)
find_package(ALSA REQUIRED)

add_executable(TascamControlPanel
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/alsacontroller.h
    src/alsacontroller.cpp
    resources/resources.qrc
)

target_link_libraries(TascamControlPanel PRIVATE
    Qt6::Widgets
    ALSA::ALSA
)

install(TARGETS TascamControlPanel
    RUNTIME DESTINATION bin
)
