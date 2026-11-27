cmake_minimum_required(VERSION 3.20)
project(FrameLabD3D12SrFgClient LANGUAGES CXX)
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS D3D12SR D3D12FG)

add_executable(d3d12_sr_fg main.cpp)
target_compile_features(d3d12_sr_fg PRIVATE cxx_std_20)
target_link_libraries(d3d12_sr_fg PRIVATE FrameLab::FrameLab d3d12 dxgi)
add_custom_command(TARGET d3d12_sr_fg POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FrameLab::FrameLab>" "$<TARGET_FILE_DIR:d3d12_sr_fg>"
  VERBATIM)
