set(
    FORMAT_PATTERNS
    source/*.cpp source/*.hpp
    test/*.cpp test/*.hpp
    CACHE STRING
    "Patterns to format"
)

# Find all files matching the patterns
file(GLOB_RECURSE FORMAT_FILES ${FORMAT_PATTERNS})

add_custom_target(
    format-check
    COMMAND clang-format --dry-run --Werror ${FORMAT_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking code formatting"
    VERBATIM
)

add_custom_target(
    format-fix
    COMMAND clang-format -i ${FORMAT_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Fixing code formatting"
    VERBATIM
)
