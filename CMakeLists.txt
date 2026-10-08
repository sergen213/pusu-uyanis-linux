cmake_minimum_required(VERSION 3.20)
project(pusu_linux LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(JSON REQUIRED IMPORTED_TARGET json-c)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

add_library(pusu_core STATIC src/assets.cpp src/script_vm.cpp src/resources.cpp)
target_compile_features(pusu_core PUBLIC cxx_std_20)
target_include_directories(pusu_core PUBLIC src)
target_link_libraries(pusu_core PUBLIC OpenSSL::Crypto ZLIB::ZLIB)

add_library(pusu_settings STATIC src/settings.cpp)
target_compile_features(pusu_settings PUBLIC cxx_std_20)
target_include_directories(pusu_settings PUBLIC src)

find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(Vulkan REQUIRED)
find_program(PUSU_GLSLANG NAMES glslangValidator glslang REQUIRED)
find_path(PUSU_SQUISH_INCLUDE squish.h REQUIRED)
find_library(PUSU_SQUISH_LIBRARY NAMES squish REQUIRED)
add_library(pusu_squish SHARED IMPORTED)
set_target_properties(pusu_squish PROPERTIES IMPORTED_LOCATION "${PUSU_SQUISH_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PUSU_SQUISH_INCLUDE}")

# Actual build-time SPIR-V, embedded into the ELF; no runtime compiler/source lookup.
set(PUSU_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/vulkan")
set(PUSU_SHADER_BINARY "${CMAKE_BINARY_DIR}/generated/vulkan")
file(MAKE_DIRECTORY "${PUSU_SHADER_BINARY}")
function(pusu_shader name source stage)
    set(dependencies "${PUSU_SHADER_SOURCE}/${source}")
    if(source MATCHES "^material\\.")
        list(APPEND dependencies "${PUSU_SHADER_SOURCE}/material_eval.glsl")
    endif()
    if(name STREQUAL "material_ray_fragment")
        list(APPEND dependencies "${PUSU_SHADER_SOURCE}/ray_query.glsl")
    endif()
    add_custom_command(OUTPUT "${PUSU_SHADER_BINARY}/${name}.spv"
        COMMAND "${PUSU_GLSLANG}" -V --target-env vulkan1.2 -S "${stage}"
            "-I${PUSU_SHADER_SOURCE}" ${ARGN}
            -o "${PUSU_SHADER_BINARY}/${name}.spv" "${PUSU_SHADER_SOURCE}/${source}"
        DEPENDS ${dependencies}
        VERBATIM)
endfunction()
pusu_shader(material_vertex material.vert vert)
pusu_shader(material_fragment material.frag frag)
pusu_shader(material_ray_fragment material.frag frag -DPUSU_RAY_QUERY=1)
pusu_shader(fullscreen_vertex fullscreen.vert vert)
pusu_shader(post_fragment post.frag frag)
pusu_shader(history_fragment history.frag frag)
set(PUSU_SHADER_NAMES material_vertex material_fragment material_ray_fragment
    fullscreen_vertex post_fragment history_fragment)
set(PUSU_SHADER_INPUTS)
set(PUSU_SHADER_ARGUMENTS)
foreach(shader IN LISTS PUSU_SHADER_NAMES)
    string(TOUPPER "${shader}" variable)
    list(APPEND PUSU_SHADER_INPUTS "${PUSU_SHADER_BINARY}/${shader}.spv")
    list(APPEND PUSU_SHADER_ARGUMENTS "-D${variable}=${PUSU_SHADER_BINARY}/${shader}.spv")
endforeach()
add_custom_command(OUTPUT "${PUSU_SHADER_BINARY}/renderer_vulkan_shaders.hpp"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${PUSU_SHADER_BINARY}/renderer_vulkan_shaders.hpp"
        ${PUSU_SHADER_ARGUMENTS} -P "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
    DEPENDS ${PUSU_SHADER_INPUTS} "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
    VERBATIM)
add_custom_target(pusu_vulkan_shaders DEPENDS "${PUSU_SHADER_BINARY}/renderer_vulkan_shaders.hpp")
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_image SDL2_mixer)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample libswscale)
add_library(pusu_runtime STATIC
    src/game.cpp src/game_hosts.cpp src/game_scene.cpp src/game_save.cpp
    src/game_actors.cpp src/game_audio.cpp src/game_effects.cpp
    src/game_materials.cpp src/game_props.cpp src/game_menu.cpp
    src/game_spatial.cpp src/game_triggers.cpp src/game_interactions.cpp
    src/renderer.cpp src/renderer_shared.cpp src/renderer_cube_cache.cpp
    src/renderer_vulkan.cpp src/renderer_vulkan_raster.cpp
    src/renderer_vulkan_quality.cpp src/renderer_vulkan_rt.cpp
    src/interface.cpp src/media.cpp src/scene_math.cpp)
add_dependencies(pusu_runtime pusu_vulkan_shaders)
target_include_directories(pusu_runtime PRIVATE "${PUSU_SHADER_BINARY}")
target_link_libraries(pusu_runtime PUBLIC pusu_core pusu_settings
    PkgConfig::SDL PkgConfig::FFMPEG GLEW::GLEW OpenGL::GL Vulkan::Vulkan pusu_squish Threads::Threads)
add_executable(pusu-game src/game_main.cpp)
target_link_libraries(pusu-game PRIVATE pusu_runtime)
install(TARGETS pusu-game RUNTIME DESTINATION bin)

set(PUSU_MSI_ROOT "${CMAKE_SOURCE_DIR}/.work/disc/tools/usr" CACHE PATH "Native libmsi development prefix")
find_path(PUSU_MSI_INCLUDE libmsi.h HINTS "${PUSU_MSI_ROOT}/include/libmsi-1.0" REQUIRED)
find_library(PUSU_MSI_LIBRARY NAMES msi-1.0 HINTS "${PUSU_MSI_ROOT}/lib" REQUIRED)
add_library(pusu_msi SHARED IMPORTED)
set_target_properties(pusu_msi PROPERTIES IMPORTED_LOCATION "${PUSU_MSI_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PUSU_MSI_INCLUDE}")
find_path(PUSU_GCAB_INCLUDE libgcab.h HINTS "${PUSU_MSI_ROOT}/include/libgcab-1.0" REQUIRED)
find_library(PUSU_GCAB_LIBRARY NAMES gcab-1.0 HINTS "${PUSU_MSI_ROOT}/lib" REQUIRED)
add_library(pusu_gcab SHARED IMPORTED)
set_target_properties(pusu_gcab PROPERTIES IMPORTED_LOCATION "${PUSU_GCAB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PUSU_GCAB_INCLUDE}")

add_library(pusu_importer STATIC src/importer.cpp)
target_link_libraries(pusu_importer PUBLIC pusu_settings OpenSSL::Crypto
    PkgConfig::ARCHIVE PkgConfig::GIO pusu_msi pusu_gcab)

# The workspace contains a colon; only relative $ORIGIN loader paths are safe.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(EXISTS "${PUSU_MSI_ROOT}/lib/libmsi-1.0.so.0.0.0")
    configure_file("${PUSU_MSI_ROOT}/lib/libmsi-1.0.so.0.0.0"
        "${CMAKE_BINARY_DIR}/lib/libmsi-1.0.so.0" COPYONLY)
    configure_file("${PUSU_MSI_ROOT}/lib/libgcab-1.0.so.0.3.0"
        "${CMAKE_BINARY_DIR}/lib/libgcab-1.0.so.0" COPYONLY)
    install(FILES "${CMAKE_BINARY_DIR}/lib/libmsi-1.0.so.0"
        "${CMAKE_BINARY_DIR}/lib/libgcab-1.0.so.0" DESTINATION lib)
endif()

add_executable(pusu-installer src/installer_main.cpp)
target_link_libraries(pusu-installer PRIVATE pusu_importer PkgConfig::GTK PNG::PNG Threads::Threads)
add_executable(pusu-launcher src/launcher_main.cpp)
target_link_libraries(pusu-launcher PRIVATE pusu_importer PkgConfig::GTK PkgConfig::JSON PNG::PNG)
foreach(art installer.png pusu.png)
    configure_file("packaging/art/${art}" "${CMAKE_BINARY_DIR}/share/pusu/${art}" COPYONLY)
endforeach()
install(TARGETS pusu-installer pusu-launcher RUNTIME DESTINATION bin)
install(FILES packaging/art/installer.png packaging/art/pusu.png DESTINATION share/pusu)

include(CTest)
if(BUILD_TESTING)
    add_executable(check_assets checks/assets.cpp)
    target_link_libraries(check_assets PRIVATE pusu_core)
    add_executable(check_script_vm checks/script_vm.cpp)
    target_link_libraries(check_script_vm PRIVATE pusu_core)
    add_executable(check_resources checks/resources.cpp)
    target_link_libraries(check_resources PRIVATE pusu_core)
    add_executable(check_settings checks/settings.cpp)
    target_link_libraries(check_settings PRIVATE pusu_settings)
    add_executable(check_importer checks/importer.cpp)
    target_link_libraries(check_importer PRIVATE pusu_importer)
    add_executable(check_scene_math checks/scene_math.cpp)
    target_link_libraries(check_scene_math PRIVATE pusu_runtime)
    add_executable(check_game_triggers checks/game_triggers.cpp)
    target_link_libraries(check_game_triggers PRIVATE pusu_runtime)
    add_executable(check_interface checks/interface.cpp src/game_save.cpp)
    target_compile_definitions(check_interface PRIVATE PUSU_GAME_SAVE_CHECK)
    target_link_libraries(check_interface PRIVATE pusu_runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(check_interface PRIVATE -UNDEBUG)
    endif()
    add_executable(check_renderer src/renderer_shared.cpp)
    target_compile_definitions(check_renderer PRIVATE PUSU_RENDERER_MATH_CHECK)
    target_link_libraries(check_renderer PRIVATE pusu_runtime)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME game_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/checks/game_cli.py" "$<TARGET_FILE:pusu-game>")
    foreach(check assets script_vm resources settings importer scene_math game_triggers renderer)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(check_${check} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${check} COMMAND check_${check})
    endforeach()
endif()
