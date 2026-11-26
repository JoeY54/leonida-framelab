cmake_minimum_required(VERSION 3.20)
project(FrameLabD3D12SrClient LANGUAGES C CXX)
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS D3D12SR)

add_executable(d3d12_sr main.cpp)
target_compile_features(d3d12_sr PRIVATE cxx_std_20)
target_link_libraries(d3d12_sr PRIVATE FrameLab::FrameLab d3d12 dxgi)
if(FrameLab_D3D12FG_FOUND)
  # Also checks that D3D12 FG refuses to start while an SR session owns NGX.
  target_compile_definitions(d3d12_sr PRIVATE FRAMELAB_SAMPLE_D3D12_FG=1)
endif()

add_executable(d3d12_sr_header_test header_test.c)
target_compile_features(d3d12_sr_header_test PRIVATE c_std_11)
target_link_libraries(d3d12_sr_header_test PRIVATE FrameLab::FrameLab)

foreach(client IN ITEMS d3d12_sr d3d12_sr_header_test)
  add_custom_command(TARGET ${client} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FrameLab::FrameLab>" "$<TARGET_FILE_DIR:${client}>"
    VERBATIM)
endforeach()

enable_testing()
add_test(NAME d3d12_sr_c_public_header COMMAND d3d12_sr_header_test)
