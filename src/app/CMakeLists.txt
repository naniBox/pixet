find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Concurrent)

add_executable(pixet WIN32
    main.cpp
    MainWindow.h
    MainWindow.cpp
)

target_link_libraries(pixet PRIVATE
    pixet_core
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
)
