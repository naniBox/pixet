add_library(pixet_core STATIC
    version.h
    version.cpp
)

target_include_directories(pixet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
