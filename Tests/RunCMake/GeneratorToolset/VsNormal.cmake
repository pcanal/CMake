file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/defaults.cmake" "# VS Defaults
set(VsNormal_Platform [[${CMAKE_VS_PLATFORM_NAME}]])
set(VsNormal_Toolset [[${CMAKE_VS_PLATFORM_TOOLSET}]])
")
message(STATUS "CMAKE_VS_PLATFORM_NAME='${CMAKE_VS_PLATFORM_NAME}'")
message(STATUS "CMAKE_VS_PLATFORM_TOOLSET='${CMAKE_VS_PLATFORM_TOOLSET}'")