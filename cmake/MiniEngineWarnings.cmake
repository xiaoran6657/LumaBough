function(miniengine_set_project_warnings target_name)
    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                # E5：把 __FILE__ 里的工程根前缀裁掉，避免绝对构建路径进入二进制。
                # 未识别的 /d1 选项只会给出 D9002 警告，不会让构建失败。
                /d1trimfile:${CMAKE_SOURCE_DIR}/
        )
    else()
        target_compile_options(
            ${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
        )
    endif()
endfunction()