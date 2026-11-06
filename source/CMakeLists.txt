# ---- Library ----

add_library(C1M_lib INTERFACE)

target_include_directories(
    C1M_lib
    INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(C1M_lib INTERFACE cxx_std_23)

find_package(Threads REQUIRED)
target_link_libraries(C1M_lib INTERFACE Threads::Threads uring)
