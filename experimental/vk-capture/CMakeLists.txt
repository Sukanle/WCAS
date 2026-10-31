# Experimental Vulkan capture layer
#
# This is intentionally separate from the production WCAS backend. It is a
# minimal, independent implementation of the swapchain-copy/DMA-BUF approach
# used by capture tools such as obs-vkcapture. It does not link against OBS.

add_library(wcas-vk-layer SHARED vk_layer.cpp)
target_link_libraries(wcas-vk-layer PRIVATE Vulkan::Vulkan)
target_compile_features(wcas-vk-layer PRIVATE cxx_std_20)
target_compile_options(wcas-vk-layer PRIVATE -Wall -Wextra)
set_target_properties(
    wcas-vk-layer
    PROPERTIES OUTPUT_NAME VkLayer_WCAS_vk_capture
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_WCAS_vk_capture.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_WCAS_vk_capture.json"
    @ONLY)

add_executable(wcas-vk-receiver receiver.cpp)
target_compile_features(wcas-vk-receiver PRIVATE cxx_std_20)
target_compile_options(wcas-vk-receiver PRIVATE -Wall -Wextra)
target_include_directories(wcas-vk-receiver PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(wcas-vk-receiver PRIVATE wcas-sdm)

install(TARGETS wcas-vk-layer LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/wcas-vk")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_WCAS_vk_capture.json"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/wcas-vk")
install(TARGETS wcas-vk-receiver RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
