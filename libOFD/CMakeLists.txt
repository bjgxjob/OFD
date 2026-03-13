cmake_minimum_required(VERSION 3.16)
project(libOFD VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LIBOFD_BUILD_TESTS "Build libOFD tests" ON)
find_package(ZLIB)

add_library(libofd
    src/crypto_provider.cpp
    src/libofd_c_api.cpp
    src/document.cpp
    src/package_reader.cpp
    src/pdf_engine/pdf_engine.cpp
    src/signature.cpp
    src/xml_utils.cpp
)

target_include_directories(libofd
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(libofd PRIVATE LIBOFD_VERSION="${PROJECT_VERSION}")
if (ZLIB_FOUND)
    target_link_libraries(libofd PRIVATE ZLIB::ZLIB)
    target_compile_definitions(libofd PRIVATE LIBOFD_HAVE_ZLIB=1)
endif()

if (MSVC)
    target_compile_options(libofd PRIVATE /W4)
else()
    target_compile_options(libofd PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(libofd_convert_cli examples/ofd_pdf_convert_cli.cpp)
target_link_libraries(libofd_convert_cli PRIVATE libofd)

if (LIBOFD_BUILD_TESTS)
    enable_testing()
    add_executable(libofd_smoke_test tests/smoke_test.cpp)
    target_link_libraries(libofd_smoke_test PRIVATE libofd)
    target_compile_definitions(
        libofd_smoke_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_smoke_test COMMAND libofd_smoke_test)

    add_executable(libofd_api_test tests/api_contract_test.cpp)
    target_link_libraries(libofd_api_test PRIVATE libofd)
    target_compile_definitions(
        libofd_api_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_api_test COMMAND libofd_api_test)

    add_executable(libofd_lifecycle_test tests/lifecycle_test.cpp)
    target_link_libraries(libofd_lifecycle_test PRIVATE libofd)
    target_compile_definitions(
        libofd_lifecycle_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_lifecycle_test COMMAND libofd_lifecycle_test)

    add_executable(libofd_external_provider_test tests/external_provider_test.cpp)
    target_link_libraries(libofd_external_provider_test PRIVATE libofd)
    add_test(NAME libofd_external_provider_test COMMAND libofd_external_provider_test)

    add_executable(libofd_convert_provider_test tests/convert_provider_test.cpp)
    target_link_libraries(libofd_convert_provider_test PRIVATE libofd)
    target_compile_definitions(
        libofd_convert_provider_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_convert_provider_test COMMAND libofd_convert_provider_test)

    add_executable(libofd_builtin_convert_roundtrip_test tests/builtin_convert_roundtrip_test.cpp)
    target_link_libraries(libofd_builtin_convert_roundtrip_test PRIVATE libofd)
    target_compile_definitions(
        libofd_builtin_convert_roundtrip_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_builtin_convert_roundtrip_test COMMAND libofd_builtin_convert_roundtrip_test)

    add_executable(libofd_layout_pdf_engine_test tests/layout_pdf_engine_test.cpp)
    target_link_libraries(libofd_layout_pdf_engine_test PRIVATE libofd)
    target_compile_definitions(
        libofd_layout_pdf_engine_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_layout_pdf_engine_test COMMAND libofd_layout_pdf_engine_test)

    add_executable(libofd_pdf_engine_image_font_test tests/pdf_engine_image_font_test.cpp)
    target_link_libraries(libofd_pdf_engine_image_font_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_image_font_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_image_font_test COMMAND libofd_pdf_engine_image_font_test)

    add_executable(libofd_pdf_engine_path_object_test tests/pdf_engine_path_object_test.cpp)
    target_link_libraries(libofd_pdf_engine_path_object_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_path_object_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_path_object_test COMMAND libofd_pdf_engine_path_object_test)

    add_executable(libofd_pdf_engine_ofd_to_pdf_path_color_test tests/pdf_engine_ofd_to_pdf_path_color_test.cpp)
    target_link_libraries(libofd_pdf_engine_ofd_to_pdf_path_color_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_ofd_to_pdf_path_color_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_ofd_to_pdf_path_color_test COMMAND libofd_pdf_engine_ofd_to_pdf_path_color_test)

    add_executable(libofd_pdf_engine_png_bmp_test tests/pdf_engine_png_bmp_test.cpp)
    target_link_libraries(libofd_pdf_engine_png_bmp_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_png_bmp_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_png_bmp_test COMMAND libofd_pdf_engine_png_bmp_test)

    add_executable(libofd_pdf_engine_external_image_decoder_test tests/pdf_engine_external_image_decoder_test.cpp)
    target_link_libraries(libofd_pdf_engine_external_image_decoder_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_external_image_decoder_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_external_image_decoder_test COMMAND libofd_pdf_engine_external_image_decoder_test)

    add_executable(libofd_pdf_engine_pdf_to_ofd_graphics_state_test tests/pdf_engine_pdf_to_ofd_graphics_state_test.cpp)
    target_link_libraries(libofd_pdf_engine_pdf_to_ofd_graphics_state_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_pdf_to_ofd_graphics_state_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_pdf_to_ofd_graphics_state_test COMMAND libofd_pdf_engine_pdf_to_ofd_graphics_state_test)

    add_executable(libofd_pdf_engine_pdf_to_ofd_text_geometry_test tests/pdf_engine_pdf_to_ofd_text_geometry_test.cpp)
    target_link_libraries(libofd_pdf_engine_pdf_to_ofd_text_geometry_test PRIVATE libofd)
    target_compile_definitions(
        libofd_pdf_engine_pdf_to_ofd_text_geometry_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_pdf_engine_pdf_to_ofd_text_geometry_test COMMAND libofd_pdf_engine_pdf_to_ofd_text_geometry_test)

    add_executable(libofd_real_world_convert_quality_test tests/real_world_convert_quality_test.cpp)
    target_link_libraries(libofd_real_world_convert_quality_test PRIVATE libofd)
    target_compile_definitions(
        libofd_real_world_convert_quality_test PRIVATE LIBOFD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME libofd_real_world_convert_quality_test COMMAND libofd_real_world_convert_quality_test)
endif()

