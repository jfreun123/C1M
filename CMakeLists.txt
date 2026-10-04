cmake_minimum_required(VERSION 3.14)

# ---- In-source guard ----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. Please create a build directory.")
endif()

project(
    C1M
    VERSION 0.1.0
    DESCRIPTION "Short description"
    HOMEPAGE_URL "https://example.com/"
    LANGUAGES CXX
)

# ---- Developer mode ----

option(C1M_DEVELOPER_MODE "Enable developer mode" ON)

# ---- Build source ----

add_subdirectory(source)

# ---- Developer mode targets ----

if(C1M_DEVELOPER_MODE)
  include(cmake/dev-mode.cmake)
endif()
