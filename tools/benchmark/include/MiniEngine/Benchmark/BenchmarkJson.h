// ============================================================================
// BenchmarkJson.h — BenchmarkRun 的确定性 JSON 序列化
// 里程碑：M7-01
// 职责：把 BenchmarkRun 按固定字段顺序写为 raw JSON。序列化只做"写出"：字段顺序、
//       转义与精度在此唯一决定；解析由 tools/performance/compare_m7_results.ps1 与验收脚本承担，
//       避免在运行时引入第二套 JSON 解析器。
// 关联：docs/architecture/README.md（raw schema）
// ============================================================================

#pragma once

#include <MiniEngine/Benchmark/BenchmarkSchema.h>

#include <string>

namespace MiniEngine::Benchmark
{
// 固定字段顺序、双精度 17 位有效数字（与 M6 metadata 口径一致，可逐字节复算）。
// artifactSha256 一类自引用哈希不写入本文件，由 sidecar 索引绑定。
[[nodiscard]] std::string SerializeRun(const BenchmarkRun& run);
} // namespace MiniEngine::Benchmark
