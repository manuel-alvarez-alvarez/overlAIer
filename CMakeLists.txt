cmake_minimum_required(VERSION 3.25.1)
project(overlAIer C)

add_subdirectory(pipeline)
add_subdirectory(processors)

# --- deploy target ---
# Builds everything and copies the executable + processor plugins to
# ${CMAKE_SOURCE_DIR}/build/ for easy deployment.
set(DEPLOY_DIR ${CMAKE_SOURCE_DIR}/build)

add_custom_target(deploy
    DEPENDS overlAIer ${ALL_PROCESSOR_TARGETS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEPLOY_DIR}/processors
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:overlAIer> ${DEPLOY_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/processors ${DEPLOY_DIR}/processors
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/installer/overlAIer.toml.example ${DEPLOY_DIR}/overlAIer.toml
    COMMENT "Deploying to ${DEPLOY_DIR}"
)