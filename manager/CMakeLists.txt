find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/main_window.cpp
)
set(HEADERS
    include/main_window.h
)

qt_add_executable(manager ${SOURCES} ${HEADERS})
target_include_directories(manager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(manager PRIVATE
    vision_rpc
    vision_core
    Qt6::Core Qt6::Gui Qt6::Widgets
)
install(TARGETS manager RUNTIME DESTINATION bin)
