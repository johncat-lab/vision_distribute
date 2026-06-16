add_library(vision_logger STATIC
    src/logger.cpp
)

target_include_directories(vision_logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vision_logger PRIVATE
    ${OpenCV_LIBS}
)

install(TARGETS vision_logger ARCHIVE DESTINATION ../lib)
