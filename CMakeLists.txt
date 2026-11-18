cmake_minimum_required(VERSION 3.20)
project(GeoVoiceStudio VERSION 1.0.0 LANGUAGES C RC)

if(NOT WIN32)
    message(FATAL_ERROR "GeoVoice Studio benötigt eine Windows-Toolchain.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0601)
if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:-fstack-protector-strong>")
    add_link_options(-fstack-protector-strong)
endif()

add_library(geovoice_routes STATIC
    route_io.c vendor/yyjson/yyjson.c
    vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c vendor/miniz/miniz_zip.c)
target_include_directories(geovoice_routes PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(geovoice_routes PRIVATE YYJSON_READER_DEPTH_LIMIT=64)
target_link_libraries(geovoice_routes PRIVATE bcrypt ole32)

add_library(geovoice_raster STATIC map_corridor.c map_image.c mbtiles.c)
target_include_directories(geovoice_raster PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(geovoice_raster PRIVATE gdiplus gdi32)

add_library(geovoice_maps STATIC map.c map_download.c map_vector.c map_orthofoto.c)
target_link_libraries(geovoice_maps PUBLIC geovoice_raster PRIVATE comctl32 comdlg32 shell32 shlwapi
    gdiplus winhttp ole32 advapi32 bcrypt gdi32 user32 uuid)

add_executable(GeoVoiceStudio WIN32
    main.c studio_dialogs.c studio_access.c studio_transfer.c studio_style.c studio_aerial.c studio_auto.c studio.rc
    map_dialog.rc)
target_link_libraries(GeoVoiceStudio PRIVATE geovoice_routes geovoice_maps comctl32 comdlg32 shell32 shlwapi winmm
    gdiplus winhttp ole32 advapi32 bcrypt gdi32 user32 uuid)
if(MINGW)
    target_link_options(GeoVoiceStudio PRIVATE -municode -static "$<$<CONFIG:Release>:-s>")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(auto_worker tests/auto_worker.c studio_auto.c)
    target_link_libraries(auto_worker PRIVATE bcrypt gdiplus ole32)
    if(MINGW)
        target_link_options(auto_worker PRIVATE -static)
    endif()
    add_test(NAME auto_worker COMMAND auto_worker)

    add_executable(route_roundtrip tests/route_roundtrip.c)
    target_link_libraries(route_roundtrip PRIVATE geovoice_routes shell32)
    if(MINGW)
        target_link_options(route_roundtrip PRIVATE -static)
    endif()
    add_test(NAME route_roundtrip COMMAND route_roundtrip)
    add_executable(mbtiles_import tests/mbtiles_import.c)
    target_link_libraries(mbtiles_import PRIVATE geovoice_raster ole32 shell32)
    if(MINGW)
        target_link_options(mbtiles_import PRIVATE -static)
    endif()
    add_test(NAME mbtiles_import COMMAND mbtiles_import)

    add_executable(orthofoto_publish tests/orthofoto_publish.c map_orthofoto.c)
    target_link_libraries(orthofoto_publish PRIVATE ole32 shell32)
    if(MINGW)
        target_link_options(orthofoto_publish PRIVATE -static)
    endif()
    add_test(NAME orthofoto_publish COMMAND orthofoto_publish)

    add_executable(orthofoto_cache tests/orthofoto_cache.c)
    target_link_libraries(orthofoto_cache PRIVATE geovoice_maps ole32 shell32)
    if(MINGW)
        target_link_options(orthofoto_cache PRIVATE -static)
    endif()
    add_test(NAME orthofoto_cache COMMAND orthofoto_cache)
endif()

install(TARGETS GeoVoiceStudio RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(FILES vendor/miniz/LICENSE DESTINATION licenses RENAME miniz.txt)
install(FILES vendor/yyjson/LICENSE DESTINATION licenses RENAME yyjson.txt)
