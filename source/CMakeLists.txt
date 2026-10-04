# ---- Library ----

add_library(cpp-starter_lib INTERFACE)

target_include_directories(
    cpp-starter_lib
    INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(cpp-starter_lib INTERFACE cxx_std_23)
