// ============================================================================
// RenderQueueBuilder.cpp — RenderPacket 构建与 culling/排序数学实现
// 里程碑：M4（01 篇；06 篇追加 12 项统计、Intersecting 细分与 main/shadow 独立开关）
// 职责：实现 BuildLightViewProjection（固定正交光视锥）、ComputeMeshLocalBounds
//       （顶点位置 AABB 保守球）、NormalMatrixFromWorld（3x3 inverse-transpose）
//       与 BuildRenderPacket（主/光两次 culling + AssetId 稳定排序 + 守恒统计）。
//       06 篇起统计覆盖 culling/queue 全量：candidateObjects = mainVisible +
//       mainCulled，shadowCandidates = shadowVisible + shadowCulled，并给出
//       culling 与整体构建两段 CPU 耗时供 A/B 解释（不只报 FPS）。
// 关联：docs/architecture/README.md
//       docs/architecture/README.md
//       engine/world/src/Frustum.cpp
// ============================================================================

#include <MiniEngine/World/RenderQueueBuilder.h>

#include <MiniEngine/Assets/MeshAsset.h>
#include <MiniEngine/Profiling/Profile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace MiniEngine::World
{
namespace
{
// 光源眼点与场景原点的固定距离：落在 shadowNear=0.1 与 shadowFar=60 的深度段中部，
// 使围绕原点的固定测试场景始终位于光视锥深度范围内（01 篇"固定 shadow volume"）。
constexpr float kLightEyeDistance = 30.0F;

// 3x3 逆矩阵规模下可接受的退化判定阈值（行列式绝对值）。
constexpr float kSingularDeterminant = 1.0e-12F;

float At(const Matrix4& matrix, const int row, const int column)
{
    return matrix.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(column)];
}

Matrix4 Multiply(const Matrix4& left, const Matrix4& right)
{
    // row-vector 约定：p * (A * B) == (p * A) * B，标准矩阵乘法。
    Matrix4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0.0F;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += At(left, row, inner) * At(right, inner, column);
            }
            result.values[static_cast<std::size_t>(row) * 4U + static_cast<std::size_t>(column)] = sum;
        }
    }
    return result;
}

float Dot3(const Float3& left, const Float3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Float3 Cross(const Float3& left, const Float3& right)
{
    return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

Float3 Normalize3(const Float3& value)
{
    const float length = std::sqrt(Dot3(value, value));
    if (!std::isfinite(length) || length <= 1.0e-8F)
    {
        throw std::runtime_error("Degenerate direction vector");
    }
    const float inverse = 1.0F / length;
    return {value.x * inverse, value.y * inverse, value.z * inverse};
}

bool IsFinite(const Float3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

// 主队列排序键：materialId → meshId → mirrored → entityIndex（全部字节/值确定）。
// 定义在匿名命名空间之外：M7-05 的并行 Builder 必须复用同一比较器（RenderQueueBuilder.h）。
bool StableOpaqueLess(const RenderDraw& left, const RenderDraw& right)
{
    return std::tie(left.materialId.bytes, left.meshId.bytes, left.mirrored, left.entityIndex) <
           std::tie(right.materialId.bytes, right.meshId.bytes, right.mirrored, right.entityIndex);
}

// 阴影队列排序键：material 不参与 shadow pass 状态切换，因此跳过 materialId。
bool StableShadowLess(const RenderDraw& left, const RenderDraw& right)
{
    return std::tie(left.meshId.bytes, left.mirrored, left.entityIndex) <
           std::tie(right.meshId.bytes, right.mirrored, right.entityIndex);
}

Matrix4 BuildLightViewProjection(const DirectionalLight& light)
{
    // 输入防御：与 Frustum 的非有限拒绝口径一致，坏输入在数学前失败。
    // DirectionalLight 以 std::array<float,3> 存储方向，先转成本层数学类型。
    const Float3 lightDirection{light.directionToLight[0], light.directionToLight[1], light.directionToLight[2]};
    if (!IsFinite(lightDirection))
    {
        throw std::runtime_error("Non-finite light direction");
    }

    // directionToLight 是"着色点指向光源"的方向；光看向场景即其反向。
    const Float3 toLight = Normalize3(lightDirection);
    const Float3 forward = {-toLight.x, -toLight.y, -toLight.z};
    const Float3 eye = {toLight.x * kLightEyeDistance, toLight.y * kLightEyeDistance, toLight.z * kLightEyeDistance};

    // LH LookAt（row-vector）：view 空间前向为 +Z。
    // zAxis = forward；xAxis = normalize(cross(up, z))；yAxis = cross(z, x)。
    const Float3 up = {0.0F, 1.0F, 0.0F};
    const Float3 zAxis = Normalize3(forward);
    const Float3 xAxis = Normalize3(Cross(up, zAxis));
    const Float3 yAxis = Cross(zAxis, xAxis);

    Matrix4 view{};
    view.values[0] = xAxis.x;
    view.values[1] = yAxis.x;
    view.values[2] = zAxis.x;
    view.values[4] = xAxis.y;
    view.values[5] = yAxis.y;
    view.values[6] = zAxis.y;
    view.values[8] = xAxis.z;
    view.values[9] = yAxis.z;
    view.values[10] = zAxis.z;
    view.values[12] = -Dot3(eye, xAxis);
    view.values[13] = -Dot3(eye, yAxis);
    view.values[14] = -Dot3(eye, zAxis);
    view.values[15] = 1.0F;

    // LH off-center 正交投影到 D3D 深度 0..1：
    //   x' = 2x/(r-l) - (l+r)/(r-l)   → m00 = 2/(r-l)，m30 = (l+r)/(l-r)
    //   y' = 2y/(t-b) - (t+b)/(t-b)   → m11 = 2/(t-b)，m31 = (t+b)/(b-t)
    //   z' = z/(f-n) - n/(f-n)        → m22 = 1/(f-n)，m32 = -n/(f-n)
    // 偏移项（m30/m31）不可省略：shadowLeft/Right/Bottom/Top 是对外可配置字段，
    // 非对称盒（如 0..40）下缺偏移会让视锥不居中、阴影静默偏移；
    // 对称盒时偏移恰为 0，因此对称默认值无法暴露该缺失。
    const float width = light.shadowRight - light.shadowLeft;
    const float height = light.shadowTop - light.shadowBottom;
    const float depthRange = light.shadowFar - light.shadowNear;
    if (!(width > 0.0F) || !(height > 0.0F) || !(depthRange > 0.0F) || !std::isfinite(width) ||
        !std::isfinite(height) || !std::isfinite(depthRange))
    {
        throw std::runtime_error("Degenerate light shadow volume");
    }

    Matrix4 projection{};
    projection.values[0] = 2.0F / width;
    projection.values[5] = 2.0F / height;
    projection.values[10] = 1.0F / depthRange;
    projection.values[12] = (light.shadowLeft + light.shadowRight) / (light.shadowLeft - light.shadowRight);
    projection.values[13] = (light.shadowTop + light.shadowBottom) / (light.shadowBottom - light.shadowTop);
    projection.values[14] = -light.shadowNear / depthRange;
    projection.values[15] = 1.0F;

    return Multiply(view, projection);
}

std::optional<Sphere> ComputeMeshLocalBounds(const Assets::MeshAsset& mesh)
{
    // 顶点必须至少容纳 position（12 字节）；stride 不足说明格式契约被破坏。
    constexpr std::uint32_t kMinStride = 12U;
    if (mesh.vertexCount == 0 || mesh.vertexStride < kMinStride || mesh.vertexData.empty())
    {
        return std::nullopt;
    }

    const std::size_t requiredBytes = static_cast<std::size_t>(mesh.vertexCount) * mesh.vertexStride;
    if (mesh.vertexData.size() < requiredBytes)
    {
        return std::nullopt;
    }

    // 只扫描 position（每个 stride 的前 12 字节，IEEE-754 float×3），不依赖顶点格式版本。
    Float3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    Float3 maximum{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                   -std::numeric_limits<float>::max()};

    for (std::uint32_t index = 0; index < mesh.vertexCount; ++index)
    {
        const auto* base = mesh.vertexData.data() + static_cast<std::size_t>(index) * mesh.vertexStride;
        float positionX = 0.0F;
        float positionY = 0.0F;
        float positionZ = 0.0F;
        std::memcpy(&positionX, base, sizeof(float));
        std::memcpy(&positionY, base + sizeof(float), sizeof(float));
        std::memcpy(&positionZ, base + 2U * sizeof(float), sizeof(float));
        if (!std::isfinite(positionX) || !std::isfinite(positionY) || !std::isfinite(positionZ))
        {
            return std::nullopt;
        }
        minimum.x = std::min(minimum.x, positionX);
        minimum.y = std::min(minimum.y, positionY);
        minimum.z = std::min(minimum.z, positionZ);
        maximum.x = std::max(maximum.x, positionX);
        maximum.y = std::max(maximum.y, positionY);
        maximum.z = std::max(maximum.z, positionZ);
    }

    // AABB 中心 + 半对角线：对任意方向的网格内容都是保守包围球。
    Sphere result{};
    result.center = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F, (minimum.z + maximum.z) * 0.5F};
    const Float3 halfExtent{(maximum.x - minimum.x) * 0.5F, (maximum.y - minimum.y) * 0.5F,
                            (maximum.z - minimum.z) * 0.5F};
    result.radius = std::sqrt(Dot3(halfExtent, halfExtent));
    if (!std::isfinite(result.radius) || result.radius < 0.0F)
    {
        return std::nullopt;
    }
    return result;
}

Matrix4 NormalMatrixFromWorld(const Matrix4& world)
{
    // 取 3x3 部分（rows/cols 0..2），求逆后转置：N = (M3x3^-1)^T。
    const float a00 = At(world, 0, 0);
    const float a01 = At(world, 0, 1);
    const float a02 = At(world, 0, 2);
    const float a10 = At(world, 1, 0);
    const float a11 = At(world, 1, 1);
    const float a12 = At(world, 1, 2);
    const float a20 = At(world, 2, 0);
    const float a21 = At(world, 2, 1);
    const float a22 = At(world, 2, 2);

    const float determinant =
        a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    if (!std::isfinite(determinant) || std::abs(determinant) < kSingularDeterminant)
    {
        throw std::runtime_error("Singular world matrix cannot produce a normal matrix");
    }
    const float inverse = 1.0F / determinant;

    // inverse[i][j] = adjugate[j][i] / det；直接写成转置形式 N[i][j] = inverse[j][i]
    // = cofactor(i,j) / det（转置与伴随在此相互抵消，见任意线性代数教材）。
    // n_ij = cofactor(j,i) / det = A⁻¹[i][j]（伴随矩阵除以行列式，即逆矩阵元素）。
    const float n00 = (a11 * a22 - a12 * a21) * inverse;
    const float n01 = (a02 * a21 - a01 * a22) * inverse;
    const float n02 = (a01 * a12 - a02 * a11) * inverse;
    const float n10 = (a12 * a20 - a10 * a22) * inverse;
    const float n11 = (a00 * a22 - a02 * a20) * inverse;
    const float n12 = (a02 * a10 - a00 * a12) * inverse;
    const float n20 = (a10 * a21 - a11 * a20) * inverse;
    const float n21 = (a01 * a20 - a00 * a21) * inverse;
    const float n22 = (a00 * a11 - a01 * a10) * inverse;

    if (!std::isfinite(n00) || !std::isfinite(n11) || !std::isfinite(n22))
    {
        throw std::runtime_error("Non-finite normal matrix");
    }

    Matrix4 result{};
    // 关键：normal matrix 是 N = (A⁻¹)ᵀ，即 N[i][j] = A⁻¹[j][i] = n_ji——
    // 存储时行列下标互换（转置）。若按 [i][j] 原样存储得到的是 A⁻¹ 本身，
    // 对剪切矩阵会产生不垂直于变换后切线的法线（静默的光照错误）。
    // 反例（world row0=[1,2,0,0]）：A⁻¹[0][1]=-2，转置后应落在 N[1][0]；
    // t=(1,0,0) → t'=(1,2,0)，n=(0,1,0) → n'=(-2,1,0)，n'·t'=0 ✓。
    result.values[0] = n00;
    result.values[1] = n10;
    result.values[2] = n20;
    result.values[4] = n01;
    result.values[5] = n11;
    result.values[6] = n21;
    result.values[8] = n02;
    result.values[9] = n12;
    result.values[10] = n22;
    // 平移列与最后一行保持 affine 约定：0、0、0、1。
    return result;
}

RenderPacket BuildRenderPacket(const std::span<const RenderItem> items, const RenderPacketCamera& camera,
                               const DirectionalLight& light, const MeshInfoLookup& meshInfo,
                               const CullingOptions& options, const MaterialInfoLookup& materialInfo)
{
    if (!meshInfo)
    {
        throw std::runtime_error("BuildRenderPacket requires a mesh info lookup");
    }

    // 06 篇：统计项含 CPU 耗时，用 steady_clock（单调、不受系统时钟调整影响）。
    // 两个计时段分别对应"culling 分类"与"整个 queue 构建（含排序）"。
    const auto buildStart = std::chrono::steady_clock::now();

    RenderPacket packet{};
    packet.view = camera.view;
    packet.projection = camera.projection;
    packet.viewProjection = Multiply(camera.view, camera.projection);
    packet.cameraWorldPosition = camera.worldPosition;
    packet.directionalLight = light;

    const Frustum cameraFrustum = Frustum::FromRowVectorDirect3D(packet.viewProjection);
    const Frustum lightFrustum = Frustum::FromRowVectorDirect3D(BuildLightViewProjection(light));

    // culling 段计时只包含两次分类，不含 normal matrix / 包围球变换 / 排序，
    // 以便 A/B 时把"剔除收益"与"queue 构建固定开销"分开看。
    const auto cullingStart = std::chrono::steady_clock::now();

    std::uint32_t entityIndex = 0;
    {
        // detail zone：把 cull/逐项提取与 stable sort 分开，粗粒度构建里不细分。
        ME_PROFILE_ZONE_NAMED_DETAIL("CullClassify");
        for (const RenderItem& item : items)
        {
            RenderDraw draw{};
            draw.mesh = item.mesh;
            draw.material = item.material;

            // 材质句柄属于帧快照，AssetId 由调用方查询提供稳定排序身份。
            // 查询失败时保留句柄并维持全零 materialId，避免把无效材质误认成有效资产。
            if (materialInfo)
            {
                const std::optional<Assets::AssetId> materialId = materialInfo(item.material);
                if (materialId.has_value())
                {
                    draw.materialId = *materialId;
                }
            }

            draw.world = item.world;
            draw.normal = NormalMatrixFromWorld(item.world);
            draw.entityIndex = entityIndex++;
            draw.mirrored = item.mirrored;
            draw.castsShadow = item.castsShadow;
            draw.receivesShadow = item.receivesShadow;

            // 包围球：能查询到 mesh 信息则做保守变换；查询失败（句柄失效/payload 未就绪）
            // 时保守地跳过 culling，绝不因数据缺失而静默丢物体。
            const std::optional<MeshDrawInfo> info = meshInfo(item.mesh);
            bool hasBounds = false;
            std::uint32_t indexCount = 0;
            if (info.has_value())
            {
                draw.meshId = info->id;
                draw.worldBounds = TransformSphereConservative(info->localBounds, item.world);
                indexCount = info->indexCount;
                hasBounds = true;
            }
            else
            {
                draw.worldBounds = {Float3{item.world.values[12], item.world.values[13], item.world.values[14]}, 0.0F};
            }

            ++packet.stats.candidateObjects;

            // 主视锥：开关关闭或包围球缺失时恒可见（culling off 仍构建 bounds 与统计，
            // 保证 A/B 两侧除"是否剔除"外完全一致——06 篇「CLI」）。
            const VolumeRelation mainRelation =
                hasBounds && options.mainCulling ? cameraFrustum.Classify(draw.worldBounds) : VolumeRelation::Inside;
            if (mainRelation == VolumeRelation::Outside)
            {
                ++packet.stats.mainCulled;
            }
            else
            {
                packet.mainOpaque.push_back(draw);
                ++packet.stats.mainVisible;
                packet.stats.trianglesSubmitted += indexCount / 3U;
                if (mainRelation == VolumeRelation::Inside)
                {
                    ++packet.stats.mainInside;
                }
                else
                {
                    ++packet.stats.mainIntersecting;
                }
            }

            // 光视锥：独立于 main（屏幕外 caster 仍可能投影到可见 receiver——
            // 06 篇明确禁止复用 main visible list 作为 shadow casters）。
            if (draw.castsShadow)
            {
                ++packet.stats.shadowCandidates;
                const VolumeRelation shadowRelation = hasBounds && options.shadowCulling
                                                          ? lightFrustum.Classify(draw.worldBounds)
                                                          : VolumeRelation::Inside;
                if (shadowRelation == VolumeRelation::Outside)
                {
                    ++packet.stats.shadowCulled;
                }
                else
                {
                    packet.shadowCasters.push_back(draw);
                    ++packet.stats.shadowVisible;
                }
            }
        }
    }
    const auto cullingEnd = std::chrono::steady_clock::now();

    // 稳定排序保证截图与统计可复现：排序键全部来自内容身份与输入顺序，
    // 与 Handle 槽位分配、reload 顺序无关（01 篇 render queue 契约）。
    const auto sortStart = std::chrono::steady_clock::now();
    {
        ME_PROFILE_ZONE_NAMED_DETAIL("StableSort");
        std::sort(packet.mainOpaque.begin(), packet.mainOpaque.end(), StableOpaqueLess);
        std::sort(packet.shadowCasters.begin(), packet.shadowCasters.end(), StableShadowLess);
    }
    const auto sortEnd = std::chrono::steady_clock::now();

    packet.stats.opaqueDrawCalls = static_cast<std::uint32_t>(packet.mainOpaque.size());
    packet.stats.shadowDrawCalls = static_cast<std::uint32_t>(packet.shadowCasters.size());
    packet.stats.cullingCpuMicroseconds = std::chrono::duration<double, std::micro>(cullingEnd - cullingStart).count();
    packet.stats.sortCpuMicroseconds = std::chrono::duration<double, std::micro>(sortEnd - sortStart).count();
    packet.stats.queueBuildCpuMicroseconds =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - buildStart).count();
    return packet;
}
} // namespace MiniEngine::World
