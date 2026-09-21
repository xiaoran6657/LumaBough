// ============================================================================
// StbImage.cpp — stb_image 的实现单点（STB_IMAGE_IMPLEMENTATION）
// 里程碑：M4（04 篇「HDR source contract」）
// 职责：stb_image.h 是 header-only 库，实现宏必须且只能在一个翻译单元展开——
//       本文件是该宏的唯一宿主（重复展开会引发链接期符号冲突）。只编入
//       AssetCooker（CMakeLists 显式列出，且对第三方源码不套 /W4 /WX）。
//       runtime 不允许 include 本头文件（ADR-0004：runtime 不解析 source format）。
// 关联：tools/asset_cooker/deps/stb/README.txt（版本 pin 与使用约束）
//       tools/asset_cooker/src/HdrImage.cpp（唯一消费方）
// ============================================================================

// 必须先定义实现宏再 include（stb 的 header-only 约定）；禁用自带的负载安全
// 检查开关会引入未定义行为，保持默认。解码错误统一走 stbi_failure_reason。
#define STB_IMAGE_IMPLEMENTATION

#include "stb_image.h"
