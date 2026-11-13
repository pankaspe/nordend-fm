# Nordend as the desktop's file chooser (ROADMAP 4): the FileChooser backend for
# xdg-desktop-portal, and nordend-portal, the small service that runs it.
add_library(nordend_portal src/file_chooser.cpp)
add_library(nordend::portal ALIAS nordend_portal)
target_include_directories(nordend_portal PUBLIC include)
target_link_libraries(nordend_portal
    PUBLIC nordend::core nordend::platform PkgConfig::SDBUSCPP
    PRIVATE nordend::warnings)

add_executable(nordend-portal src/main.cpp)
target_link_libraries(nordend-portal PRIVATE nordend::portal nordend::warnings)
# Next to nordend in the build tree, where default_picker() looks.
set_target_properties(nordend-portal PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
