add_library(nordend_fs
    src/directory.cpp
    src/directory_model.cpp
    src/directory_watcher.cpp
    src/erase.cpp
    src/file_filter.cpp
    src/icon.cpp
    src/mime.cpp
    src/operations.cpp
    src/places.cpp
    src/sort.cpp
    src/transfer.cpp
    src/transfer_queue.cpp
    src/trash.cpp
    src/walk.cpp)
add_library(nordend::fs ALIAS nordend_fs)

target_include_directories(nordend_fs PUBLIC include)
target_link_libraries(nordend_fs PUBLIC nordend::core PRIVATE nordend::warnings)
