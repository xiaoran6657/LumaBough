# M6-02：纯 CPU 公共实现；两个 concrete renderer 继续独立运行。
add_library(MiniEngineRhiPublic STATIC src/HandleRegistry.cpp src/RhiValidation.cpp src/DeviceLifetime.cpp src/RhiPipeline.cpp src/CommandValidation.cpp src/RhiShaderPackage.cpp src/ShaderRevisionTransaction.cpp src/CommandRecording.cpp src/RhiResults.cpp)
add_library(MiniEngine::RhiPublic ALIAS MiniEngineRhiPublic)
target_include_directories(MiniEngineRhiPublic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(MiniEngineRhiPublic PUBLIC cxx_std_20)
set_target_properties(MiniEngineRhiPublic PROPERTIES CXX_EXTENSIONS OFF)
miniengine_set_project_warnings(MiniEngineRhiPublic)

add_subdirectory(d3d11)
add_subdirectory(d3d12)

# M6-06：CPU adapter 组合层仍不链接任何 native SDK。
target_sources(MiniEngineRhiPublic PRIVATE src/NativeDevice.cpp src/RhiSelection.cpp)

add_subdirectory(factory)
