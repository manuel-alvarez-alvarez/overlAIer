# Each processor builds as a shared library (.so) plugin.
# To add a new processor: create a subdirectory with a .c file and a header
# that includes processor.h, then add an add_processor() call below.
#
# Output goes to a processors/ directory in the build folder.

set(PROCESSOR_OUTPUT_DIR ${CMAKE_BINARY_DIR}/processors)
file(MAKE_DIRECTORY ${PROCESSOR_OUTPUT_DIR})

# Common include path so processors can #include "processor/processor.h" etc.
include_directories(${CMAKE_SOURCE_DIR}/pipeline)

# Helper: add_processor(name source_files...)
# Creates a .so target named proc_<name> that outputs <name>.so
macro(add_processor NAME)
    set(TARGET_NAME proc_${NAME})
    add_library(${TARGET_NAME} SHARED ${ARGN})
    set_target_properties(${TARGET_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${NAME}"
        LIBRARY_OUTPUT_DIRECTORY ${PROCESSOR_OUTPUT_DIR}
    )
    list(APPEND ALL_PROCESSOR_TARGETS ${TARGET_NAME})
endmacro()

# --- Processors ---
add_processor(fps_counter fps_counter/fps_counter.c)

# Export all processor targets so the top-level CMakeLists can depend on them
set(ALL_PROCESSOR_TARGETS ${ALL_PROCESSOR_TARGETS} PARENT_SCOPE)