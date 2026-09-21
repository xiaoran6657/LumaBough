# M5-11：固定官方 WinPixEventRuntime；库和头文件仅进入 D3D12 边界。
include_guard(GLOBAL)
include(FetchContent)
FetchContent_Declare(miniengine_pix
    URL https://api.nuget.org/v3-flatcontainer/winpixeventruntime/1.0.240308001/winpixeventruntime.1.0.240308001.nupkg
    URL_HASH SHA256=726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4
    DOWNLOAD_NAME winpixeventruntime.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniengine_pix)
add_library(MiniEnginePixRuntime SHARED IMPORTED GLOBAL)
set_target_properties(MiniEnginePixRuntime PROPERTIES
    IMPORTED_IMPLIB "${miniengine_pix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.lib"
    IMPORTED_LOCATION "${miniengine_pix_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${miniengine_pix_SOURCE_DIR}/Include/WinPixEventRuntime"
    INTERFACE_COMPILE_DEFINITIONS "USE_PIX;MINIENGINE_ENABLE_PIX_RUNTIME")
add_library(MiniEngine::Pix ALIAS MiniEnginePixRuntime)
# Release 保留 markers；只复制对应架构的 runtime，不把 NuGet 产物放进源码树。
function(miniengine_use_pix target)
    target_link_libraries(${target} PRIVATE MiniEngine::Pix)
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:MiniEnginePixRuntime>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
