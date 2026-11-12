# Plugins: features outside Nordend's core, one directory each. A plain build has none: the core
# alone. Packages choose them with NORDEND_PLUGINS ("all", or a list such as "remotes"), or one at
# a time with NORDEND_PLUGIN_<ID>=ON. A plugin built in can still be switched off at run time with
# [plugins] disabled = ["<id>"] in config.toml.
#
# A plugin's CMakeLists.txt calls nordend_add_plugin(<id> SUMMARY "<one line>" SOURCES ...
# [LINK ...]) and defines, in one of its sources,
#   std::unique_ptr<nordend::plugin::Plugin> nordend_plugin_make_<id>(nordend::plugin::Host&);
# which the generated list below calls. Its dependencies are its own (LINK).

set_property(GLOBAL PROPERTY NORDEND_PLUGIN_IDS "")

function(nordend_add_plugin id)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SUMMARY" "SOURCES;LINK")
    add_library(nordend_plugin_${id} STATIC ${ARG_SOURCES})
    target_include_directories(nordend_plugin_${id} PUBLIC include)
    target_link_libraries(nordend_plugin_${id}
        PUBLIC nordend::plugin
        PRIVATE nordend::warnings ${ARG_LINK})
    set_property(GLOBAL APPEND PROPERTY NORDEND_PLUGIN_IDS ${id})
    set_property(GLOBAL PROPERTY NORDEND_PLUGIN_SUMMARY_${id} "${ARG_SUMMARY}")
endfunction()

set(NORDEND_PLUGINS "" CACHE STRING
    "Plugins to build: empty for none (the default), \"all\", or a list such as \"remotes\"")

# When NORDEND_PLUGINS changes, it decides again for every plugin; otherwise a plugin's own
# NORDEND_PLUGIN_<ID> (from the command line or ccmake) stays as set.
set(NORDEND_PLUGINS_CHANGED OFF)
if(DEFINED NORDEND_PLUGINS_LAST AND NOT NORDEND_PLUGINS_LAST STREQUAL NORDEND_PLUGINS)
    set(NORDEND_PLUGINS_CHANGED ON)
endif()
set(NORDEND_PLUGINS_LAST "${NORDEND_PLUGINS}" CACHE INTERNAL "")

# Every directory here with a CMakeLists.txt is a plugin, built when its option is on.
file(GLOB plugin_dirs LIST_DIRECTORIES true RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)
list(SORT plugin_dirs)
foreach(dir IN LISTS plugin_dirs)
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${dir} AND
       EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
        string(TOUPPER ${dir} upper)
        set(wanted OFF)
        if(NORDEND_PLUGINS STREQUAL "all" OR dir IN_LIST NORDEND_PLUGINS)
            set(wanted ON)
        endif()
        if(NOT DEFINED NORDEND_PLUGIN_${upper} OR NORDEND_PLUGINS_CHANGED)
            set(NORDEND_PLUGIN_${upper} ${wanted} CACHE BOOL "Build the ${dir} plugin" FORCE)
        endif()
        if(NORDEND_PLUGIN_${upper})
            add_subdirectory(${dir})
        endif()
    endif()
endforeach()

# A typo in NORDEND_PLUGINS should not pass silently.
if(NOT NORDEND_PLUGINS STREQUAL "all")
    foreach(wanted_id IN LISTS NORDEND_PLUGINS)
        if(NOT wanted_id IN_LIST plugin_dirs)
            message(FATAL_ERROR "NORDEND_PLUGINS: there is no plugin '${wanted_id}' in plugins/")
        endif()
    endforeach()
endif()

# The list of plugins compiled in, for plugin::builtin().
get_property(ids GLOBAL PROPERTY NORDEND_PLUGIN_IDS)
set(NORDEND_PLUGIN_DECLARATIONS "")
set(NORDEND_PLUGIN_ENTRIES "")
list(LENGTH ids NORDEND_PLUGIN_COUNT)
foreach(id IN LISTS ids)
    get_property(summary GLOBAL PROPERTY NORDEND_PLUGIN_SUMMARY_${id})
    string(APPEND NORDEND_PLUGIN_DECLARATIONS
        "std::unique_ptr<nordend::plugin::Plugin> nordend_plugin_make_${id}(nordend::plugin::Host& host);\n")
    string(APPEND NORDEND_PLUGIN_ENTRIES
        "    {.id = \"${id}\", .summary = \"${summary}\", .make = &nordend_plugin_make_${id}},\n")
endforeach()
configure_file(${PROJECT_SOURCE_DIR}/src/plugin/builtin.cpp.in generated/builtin.cpp @ONLY)
message(STATUS "Plugins: ${ids}")

add_library(nordend_plugins STATIC ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin.cpp)
add_library(nordend::plugins ALIAS nordend_plugins)
target_link_libraries(nordend_plugins PUBLIC nordend::plugin PRIVATE nordend::warnings)
foreach(id IN LISTS ids)
    target_link_libraries(nordend_plugins PUBLIC nordend_plugin_${id})
endforeach()
