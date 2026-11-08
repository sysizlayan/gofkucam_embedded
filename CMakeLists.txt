# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Generate compile_commands.json for clangd and language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(gofkucam_embedded)

# Symlink compile_commands.json to the project root for clangd
add_custom_target(compile_commands_symlink ALL
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
    BYPRODUCTS "${CMAKE_SOURCE_DIR}/compile_commands.json"
    COMMENT "Symlinking compile_commands.json to project root for clangd"
)
