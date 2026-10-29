cmake_minimum_required(VERSION 3.10)
project(pure_doc2pdf CXX C)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 依赖查找 (zlib 与 freetype)
find_package(ZLIB REQUIRED)
find_package(Freetype REQUIRED)
add_definitions(-DHAVE_FREETYPE=1)

# 头文件路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ZLIB_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
)

# 纯源码跨平台核心库 (libpure_doc2pdf)
set(CORE_SOURCES
    src/utf_utils.cpp
    src/image_utils.cpp
    src/pole.cpp
    src/doc_parser.cpp
    src/docx_parser.cpp
    src/pdf_renderer.cpp
    src/md_renderer.cpp
    src/pure_doc2pdf.cpp
)

add_library(pure_doc2pdf_lib STATIC ${CORE_SOURCES})
target_link_libraries(pure_doc2pdf_lib PUBLIC ${ZLIB_LIBRARIES} ${FREETYPE_LIBRARIES})

# 主命令行程序
add_executable(pure_doc2pdf src/main.cpp)
target_link_libraries(pure_doc2pdf PRIVATE pure_doc2pdf_lib)

# 自动化测试程序
enable_testing()
add_executable(doc2pdf_tests tests/test_runner.cpp)
target_link_libraries(doc2pdf_tests PRIVATE pure_doc2pdf_lib)

add_test(
    NAME AllUnitTests
    COMMAND doc2pdf_tests ${CMAKE_CURRENT_SOURCE_DIR}/testdata ${CMAKE_CURRENT_BINARY_DIR}
)
