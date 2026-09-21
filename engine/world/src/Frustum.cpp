// ============================================================================
// Frustum.cpp — 视锥六平面提取、球分类与保守包围球变换的实现
// 里程碑：M4（01 篇手抄清单第 3 条）
// 职责：实现 Frustum.h 声明的三项能力。数学约定为引擎的 row-major
//       row-vector（clip = p * M），与 Gribb-Hartmann 的列向量提取互为转置，
//       推导见 FromRowVectorDirect3D 注释。
// 关联：docs/architecture/README.md、tests/world/FrustumTests.cpp
// ============================================================================
#include <MiniEngine/World/Frustum.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace MiniEngine::World
{
namespace
{
float At(const Matrix4& matrix, int row, int column)
{
    return matrix.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(column)];
}

Float4 AddColumn(const Matrix4& m, int a, int b)
{
    return {At(m, 0, a) + At(m, 0, b), At(m, 1, a) + At(m, 1, b), At(m, 2, a) + At(m, 2, b), At(m, 3, a) + At(m, 3, b)};
}

Float4 SubtractColumn(const Matrix4& m, int a, int b)
{
    return {At(m, 0, a) - At(m, 0, b), At(m, 1, a) - At(m, 1, b), At(m, 2, a) - At(m, 2, b), At(m, 3, a) - At(m, 3, b)};
}

Float4 Column(const Matrix4& m, int column)
{
    return {At(m, 0, column), At(m, 1, column), At(m, 2, column), At(m, 3, column)};
}

// 归一化平面：法线化为单位长度（w 同除），使 dot(n,p)+w 即真实欧氏距离。
// 长度趋零说明矩阵退化（如零投影），fail-fast 而不是产出垃圾 culling 结果。
Float4 NormalizePlane(Float4 plane)
{
    const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
    if (!std::isfinite(length) || length <= 1.0e-8F)
    {
        throw std::runtime_error("Degenerate frustum plane");
    }
    const float inverse = 1.0F / length;
    return {plane.x * inverse, plane.y * inverse, plane.z * inverse, plane.w * inverse};
}

float Distance(Float4 plane, Float3 point)
{
    return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
}
} // namespace

Frustum Frustum::FromRowVectorDirect3D(const Matrix4& untransposedViewProjection)
{
    // Gribb-Hartmann 平面提取的 row-vector 形式。
    //
    // D3D 裁剪体：-w ≤ x ≤ w、-w ≤ y ≤ w、0 ≤ z ≤ w。row-vector 约定下
    // clip = p * M，因此 clip 的每个分量 = p 与 M 一列的点积——列即隐式平面：
    //   x + w ≥ 0 → plane = col3 + colX（内法线指向体内）
    //   w - x ≥ 0 → plane = col3 - colX
    // D3D 与 GL 的差异只在近/远平面：D3D 深度 0..w，near 平面是 z 自身
    // （col2），far 平面是 col3 - col2；GL 的 -w ≤ z ≤ w 则是 col3±col2 两个。
    // 六平面顺序 = 枚举注释的 Left/Right/Bottom/Top/Near/Far。
    Frustum result{};
    result.m_planes = {NormalizePlane(AddColumn(untransposedViewProjection, 3, 0)),
                       NormalizePlane(SubtractColumn(untransposedViewProjection, 3, 0)),
                       NormalizePlane(AddColumn(untransposedViewProjection, 3, 1)),
                       NormalizePlane(SubtractColumn(untransposedViewProjection, 3, 1)),
                       NormalizePlane(Column(untransposedViewProjection, 2)),
                       NormalizePlane(SubtractColumn(untransposedViewProjection, 3, 2))};
    return result;
}

VolumeRelation Frustum::Classify(const Sphere& sphere) const
{
    if (!std::isfinite(sphere.center.x) || !std::isfinite(sphere.center.y) || !std::isfinite(sphere.center.z) ||
        !std::isfinite(sphere.radius) || sphere.radius < 0.0F)
    {
        throw std::runtime_error("Invalid sphere");
    }

    // epsilon 随半径缩放：小半径用绝对容差、大半径按比例放大，
    // 吸收切平面处的浮点残差（贴近平面的球保守判为 Intersecting，见 Frustum.h）。
    const float epsilon = 1.0e-4F * std::max(1.0F, sphere.radius);
    bool intersects = false;
    for (const Float4 plane : m_planes)
    {
        const float distance = Distance(plane, sphere.center);
        if (!std::isfinite(distance))
        {
            throw std::runtime_error("Non-finite sphere-plane distance");
        }
        if (distance < -sphere.radius - epsilon)
        {
            return VolumeRelation::Outside;
        }
        intersects = intersects || distance <= sphere.radius + epsilon;
    }
    return intersects ? VolumeRelation::Intersecting : VolumeRelation::Inside;
}

const std::array<Float4, 6>& Frustum::Planes() const noexcept
{
    return m_planes;
}

Sphere TransformSphereConservative(const Sphere& local, const Matrix4& world)
{
    if (!std::isfinite(local.center.x) || !std::isfinite(local.center.y) || !std::isfinite(local.center.z) ||
        !std::isfinite(local.radius) || local.radius < 0.0F ||
        !std::all_of(world.values.begin(), world.values.end(), [](float value) { return std::isfinite(value); }))
    {
        throw std::runtime_error("Invalid sphere transform input");
    }

    // 球心：row-vector 变换 p * world（w=1 的仿射部分，含平移）。
    const Float3 center{local.center.x * At(world, 0, 0) + local.center.y * At(world, 1, 0) +
                            local.center.z * At(world, 2, 0) + At(world, 3, 0),
                        local.center.x * At(world, 0, 1) + local.center.y * At(world, 1, 1) +
                            local.center.z * At(world, 2, 1) + At(world, 3, 1),
                        local.center.x * At(world, 0, 2) + local.center.y * At(world, 1, 2) +
                            local.center.z * At(world, 2, 2) + At(world, 3, 2)};

    const auto rowLengthSquared = [&](int row)
    {
        return At(world, row, 0) * At(world, row, 0) + At(world, row, 1) * At(world, row, 1) +
               At(world, row, 2) * At(world, row, 2);
    };
    const auto rowDot = [&](int a, int b)
    {
        return At(world, a, 0) * At(world, b, 0) + At(world, a, 1) * At(world, b, 1) +
               At(world, a, 2) * At(world, b, 2);
    };

    const float l0 = rowLengthSquared(0);
    const float l1 = rowLengthSquared(1);
    const float l2 = rowLengthSquared(2);
    // 正交性检测：基向量两两点积相对基长平方是否可忽略。
    // epsilon 随基长平方缩放，避免大基长矩阵因绝对容差被误判为剪切。
    const float scaleReference = std::max({l0, l1, l2, 1.0F});
    const float epsilon = scaleReference * 1.0e-5F;
    const bool pureTrs =
        std::abs(rowDot(0, 1)) <= epsilon && std::abs(rowDot(0, 2)) <= epsilon && std::abs(rowDot(1, 2)) <= epsilon;

    // 纯 TRS（正交基）：半径缩放 = 最大基长（精确）。
    // 含剪切/非正交：任意向量长度的上界是基的 Frobenius 范数 √(l0+l1+l2)，
    // 保守但绝不漏剔除——culling 的正确性优先于紧凑性。
    const float scale = pureTrs ? std::sqrt(std::max({l0, l1, l2})) : std::sqrt(l0 + l1 + l2);
    const float radius = local.radius * scale;
    if (!std::isfinite(radius))
    {
        throw std::runtime_error("Non-finite transformed sphere radius");
    }
    return {center, radius};
}
} // namespace MiniEngine::World
