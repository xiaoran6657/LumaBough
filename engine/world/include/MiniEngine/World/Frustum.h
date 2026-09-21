// ============================================================================
// Frustum.h — 视锥体（六平面）与保守包围球变换
// 里程碑：M4（01 篇架构边界与渲染顺序；数学与单元测试对应手抄清单第 3 条）
// 职责：从 row-major row-vector 约定的 view-projection 提取 D3D 裁剪体的六个
//       平面，对包围球做 Inside/Intersecting/Outside 分类；并把本地包围球经
//       world 矩阵保守变换到世界空间。全部为 CPU 纯数学，无 D3D/平台类型。
// 关联：docs/architecture/README.md（culling 契约）
//       engine/world/src/Frustum.cpp（实现与数学推导注释）
//       tests/world/FrustumTests.cpp（平面归一化/分类/负向用例）
// ============================================================================

#pragma once

#include <MiniEngine/World/WorldTypes.h>

#include <array>

namespace MiniEngine::World
{
// Float4 复用 WorldTypes.h 的定义；本头文件只补充 Sphere / VolumeRelation。

// 本地空间包围球：culling 的最小几何输入（06 篇 culling baseline 的分类单元）。
struct Sphere final
{
    Float3 center{};
    float radius{};
};

// 包围球与视锥的关系：Outside = 至少被一个平面整体剔除（可安全跳过绘制）；
// Intersecting = 与某平面相交（保守保留）；Inside = 全部平面内侧。
enum class VolumeRelation
{
    Outside,
    Intersecting,
    Inside
};

// D3D 裁剪体视锥：由六个归一化平面构成，法线指向体内侧。
class Frustum final
{
  public:
    // 从未转置的 view-projection（row-major row-vector，引擎 Matrix4 原样存储）
    // 提取六平面。D3D 裁剪体约定：-w ≤ x ≤ w、-w ≤ y ≤ w、0 ≤ z ≤ w。
    // 失败：矩阵含非有限值或某平面退化（长度趋零）时抛 std::runtime_error。
    static Frustum FromRowVectorDirect3D(const Matrix4& untransposedViewProjection);

    // 对世界空间包围球分类。epsilon 随半径缩放（1e-4 * max(1, r)），
    // 吸收切平面浮点残差：贴近平面的球会保守地判为 Intersecting 而非剔除。
    // 失败：球含非有限值、负半径或距离计算非有限时抛 std::runtime_error。
    VolumeRelation Classify(const Sphere& sphere) const;

    // 只读访问六个平面（x,y,z 为归一化法线，w 为平面偏移）。
    const std::array<Float4, 6>& Planes() const noexcept;

  private:
    // 顺序固定为 Left, Right, Bottom, Top, Near, Far；
    // 内侧判定：dot(plane.xyz, p) + plane.w >= 0。
    std::array<Float4, 6> m_planes{};
};

// 把本地空间包围球经 world 矩阵保守变换到世界空间。
// 纯 TRS（正交基）取最大基长的精确缩放；检测到剪切/非正交时退化为
// Frobenius 范数上界——culling 宁可多保留，绝不漏剔除。
// 失败：球或矩阵含非有限值、负半径、结果半径非有限时抛 std::runtime_error。
Sphere TransformSphereConservative(const Sphere& local, const Matrix4& world);
} // namespace MiniEngine::World
