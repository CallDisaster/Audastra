#include "War3MapCore/Render/ModelPicking.h"
#include "../Animation/AnimationInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <tuple>

namespace War3Map::Render {
namespace {
using Point = std::array<double, 3>;
using Affine = std::array<double, 12>; // 行式，与实例变换相同。
using War3RenderCore::Range32;

bool Fits(Range32 range, std::size_t size) {
    return range.offset <= size && range.count <= size - range.offset;
}
bool OwnedMapping(const AnimatedModelScene& scene, std::uint32_t modelIndex, const AnimationPickModel& input) {
    if (!input.ownedStaticAsset || input.source || input.collisionNodes || modelIndex >= scene.models.size() ||
        modelIndex >= scene.geometry.models.size() || modelIndex >= scene.geometry.modelAssetKeys.size()) return false;
    ModelInstanceRequest request; request.ownedStaticAsset = input.ownedStaticAsset;
    const auto& model = scene.models[modelIndex]; const auto& raw = scene.geometry.models[modelIndex];
    const auto owned = input.ownedStaticAsset->Meshes();
    if (scene.geometry.modelAssetKeys[modelIndex] != ModelAssetKey(request) || model.model.firstMesh != raw.firstMesh ||
        model.model.meshCount != raw.meshCount || raw.meshCount != owned.size() || model.groupCount != raw.meshCount ||
        !Fits({raw.firstMesh, raw.meshCount}, scene.meshes.size()) || !Fits({raw.firstMesh, raw.meshCount}, scene.geometry.meshes.size()) ||
        !Fits({raw.firstMesh, raw.meshCount}, scene.geometry.meshGeosets.size())) return false;
    for (std::uint32_t local = 0; local < raw.meshCount; ++local) {
        const auto at = raw.firstMesh + local; const auto& mesh = scene.meshes[at]; const auto& geometry = scene.geometry.meshes[at];
        if (scene.geometry.meshGeosets[at] != UINT32_MAX || mesh.firstGroup != local || mesh.groupCount != 1 ||
            mesh.mesh.firstIndex != geometry.firstIndex || mesh.mesh.indexCount != geometry.indexCount ||
            mesh.mesh.baseVertex != geometry.baseVertex || mesh.mesh.firstLayer != geometry.firstLayer || mesh.mesh.layerCount != 1 ||
            geometry.layerCount != 1 || geometry.indexCount != owned[local].indices.size() ||
            !Fits({geometry.firstIndex, geometry.indexCount}, scene.geometry.indices.size()) ||
            !Fits({geometry.baseVertex, static_cast<std::uint32_t>(owned[local].vertices.size())}, scene.vertices.size()) ||
            !Fits({geometry.firstLayer, 1}, scene.geometry.layers.size())) return false;
    }
    return true;
}
Point Sub(const Point& a, const Point& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
Point Cross(const Point& a, const Point& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double Dot(const Point& a, const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Point Apply(const Affine& m, const Point& p, bool direction = false) {
    Point out{};
    for (unsigned row=0;row<3;++row) out[row]=m[row*4]*p[0]+m[row*4+1]*p[1]+m[row*4+2]*p[2]+(direction ? 0 : m[row*4+3]);
    return out;
}
Affine Compose(const float* instance, const War3RenderGroupMatrixV2& node) {
    Affine out{};
    for (unsigned row=0;row<3;++row) {
        for (unsigned column=0;column<3;++column)
            for (unsigned k=0;k<3;++k) out[row*4+column]+=double(instance[row*4+k])*node.values[column*3+k];
        out[row*4+3]=instance[row*4+3];
        for (unsigned k=0;k<3;++k) out[row*4+3]+=double(instance[row*4+k])*node.values[9+k];
    }
    return out;
}
Affine ComposeMagnitude(const float* instance, const War3RenderGroupMatrixV2& node) {
    Affine out{};
    for (unsigned row=0;row<3;++row) {
        for (unsigned column=0;column<3;++column)
            for (unsigned k=0;k<3;++k) out[row*4+column]+=std::abs(double(instance[row*4+k])*node.values[column*3+k]);
        out[row*4+3]=std::abs(double(instance[row*4+3]));
        for (unsigned k=0;k<3;++k) out[row*4+3]+=std::abs(double(instance[row*4+k])*node.values[9+k]);
    }
    return out;
}
Point SkinnedPoint(const float* instance, const War3RenderGroupMatrixV2& group, const Point& vertex) {
    Point local{};
    for (unsigned row=0;row<3;++row)
        local[row]=group.values[row]*vertex[0]+group.values[3+row]*vertex[1]+group.values[6+row]*vertex[2]+group.values[9+row];
    Point world{};
    for (unsigned row=0;row<3;++row)
        world[row]=instance[row*4]*local[0]+instance[row*4+1]*local[1]+instance[row*4+2]*local[2]+instance[row*4+3];
    return world;
}
std::optional<Affine> Inverse(const Affine& m) {
    const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
    const double determinant=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if (!std::isfinite(determinant) || determinant==0) return {};
    const double r=1/determinant;
    Affine out{(e*i-f*h)*r,(c*h-b*i)*r,(b*f-c*e)*r,0,
        (f*g-d*i)*r,(a*i-c*g)*r,(c*d-a*f)*r,0,(d*h-e*g)*r,(b*g-a*h)*r,(a*e-b*d)*r,0};
    for (unsigned row=0;row<3;++row) out[row*4+3]=-(out[row*4]*m[3]+out[row*4+1]*m[7]+out[row*4+2]*m[11]);
    if (!std::all_of(out.begin(),out.end(),[](double x) { return std::isfinite(x); })) return {};
    return out;
}
std::optional<double> RayBox(const Point& origin, const Point& direction, const std::array<double,6>& box, double maximum) {
    double low=0,high=maximum;
    for (unsigned axis=0;axis<3;++axis) {
        if (direction[axis]==0) { if (origin[axis]<box[axis] || origin[axis]>box[axis+3]) return {}; }
        else {
            double a=(box[axis]-origin[axis])/direction[axis],b=(box[axis+3]-origin[axis])/direction[axis];
            if (a>b) std::swap(a,b);
            low=std::max(low,a); high=std::min(high,b);
            if (low>high) return {};
        }
    }
    return low;
}
bool IncludeBox(AnimationPickBounds& output, const Affine& m, const std::array<double,6>& local, const Affine& magnitudeMatrix) {
    for (unsigned corner=0;corner<8;++corner) {
        const Point p{local[(corner&1) ? 3 : 0],local[(corner&2) ? 4 : 1],local[(corner&4) ? 5 : 2]};
        const auto world=Apply(m,p);
        if (!std::all_of(world.begin(),world.end(),[](double x) { return std::isfinite(x); })) return false;
        for (unsigned axis=0;axis<3;++axis) {
            // 只填数值计算余量，绝不是最低世界尺寸/最低像素点选兜底。
            const double magnitude=magnitudeMatrix[axis*4]*std::abs(p[0])+magnitudeMatrix[axis*4+1]*std::abs(p[1])+
                magnitudeMatrix[axis*4+2]*std::abs(p[2])+magnitudeMatrix[axis*4+3];
            const double margin=64*std::numeric_limits<float>::epsilon()*(1+magnitude);
            if (!output.available) { output.box[axis]=world[axis]-margin; output.box[axis+3]=world[axis]+margin; }
            else { output.box[axis]=std::min(output.box[axis],world[axis]-margin); output.box[axis+3]=std::max(output.box[axis+3],world[axis]+margin); }
        }
        output.available=true;
    }
    return true;
}
std::array<double,6> CollisionBox(const MdxCollisionShape& shape) {
    std::array<double,6> box{};
    for (unsigned axis=0;axis<3;++axis) {
        if (shape.sourceType==2) { box[axis]=double(shape.vertices[0][axis])-shape.radius; box[axis+3]=double(shape.vertices[0][axis])+shape.radius; }
        else { box[axis]=std::min(shape.vertices[0][axis],shape.vertices[1][axis]); box[axis+3]=std::max(shape.vertices[0][axis],shape.vertices[1][axis]); }
    }
    return box;
}
bool Supported(const MdxCollisionShape& shape) {
    const auto finite=[](const auto& vertex) { return std::all_of(vertex.begin(),vertex.end(),[](float x) { return std::isfinite(x); }); };
    return finite(shape.vertices[0]) && (shape.sourceType==2 || finite(shape.vertices[1])) &&
        std::isfinite(shape.radius) && shape.support==MdxCollisionSupport::Supported &&
        ((shape.sourceType==0 && shape.layout==MdxCollisionLayout::BoxVertices24) ||
         (shape.sourceType==2 && shape.layout==MdxCollisionLayout::SphereCenterRadius16 && shape.hasRadius && shape.radius>=0));
}
bool VisibleMesh(const AnimatedModelScene& scene, const AnimationFrame& frame,
    const War3RenderSkinnedModelInstanceV2& instance, std::uint32_t meshIndex, std::uint32_t localMesh,
    std::uint32_t* traversedLayers=nullptr, bool* layerBudgetExceeded=nullptr) {
    const auto alpha=static_cast<std::uint64_t>(instance.firstAlpha)+localMesh;
    if (alpha>=frame.alphas.size() || !(frame.alphas[static_cast<std::size_t>(alpha)]>0)) return false;
    const auto& mesh=scene.meshes[meshIndex].mesh;
    if (!Fits({mesh.firstLayer,mesh.layerCount},scene.geometry.layers.size())) return false;
    for (std::uint32_t layer=0;layer<mesh.layerCount;++layer) {
        if (traversedLayers) {
            if (*traversedLayers==131072) { *layerBudgetExceeded=true; return false; }
            ++*traversedLayers;
        }
        if (scene.geometry.layers[mesh.firstLayer+layer].alpha>0) return true;
    }
    return false;
}
std::optional<double> RayTriangle(const Point& origin, const Point& direction,
    const Point& a, const Point& b, const Point& c, double maximum) {
    const auto e1=Sub(b,a),e2=Sub(c,a),h=Cross(direction,e2);
    const double determinant=Dot(e1,h),scale=std::sqrt(Dot(e1,e1)*Dot(h,h));
    if (!std::isfinite(determinant) || std::abs(determinant)<=std::numeric_limits<double>::epsilon()*32*scale) return {};
    const double inverse=1/determinant;
    const auto s=Sub(origin,a); const double u=Dot(s,h)*inverse;
    if (u<0 || u>1) return {};
    const auto q=Cross(s,e1); const double v=Dot(direction,q)*inverse;
    if (v<0 || u+v>1) return {};
    const double distance=Dot(e2,q)*inverse;
    return std::isfinite(distance) && distance>=0 && distance<=maximum ? std::optional(distance) : std::nullopt;
}
std::optional<double> RaySphere(const Point& origin, const Point& direction, const MdxCollisionShape& shape, double maximum) {
    const Point center{shape.vertices[0][0],shape.vertices[0][1],shape.vertices[0][2]};
    const auto offset=Sub(origin,center); const double a=Dot(direction,direction),b=Dot(offset,direction);
    const double c=Dot(offset,offset)-double(shape.radius)*shape.radius;
    if (c<=0) return 0;
    const double discriminant=b*b-a*c;
    if (!(a>0) || discriminant<0 || !std::isfinite(discriminant)) return {};
    const double distance=(-b-std::sqrt(discriminant))/a;
    return distance>=0 && distance<=maximum ? std::optional(distance) : std::nullopt;
}
} // namespace

Status PrepareAnimationPickBounds(const AnimatedModelScene& scene, AnimationFrame& frame) {
    const auto fail=[] { return AnimationFailure("ANIMATION_PICK_MAPPING","点选来源、实际网格或 owned 帧片段不一致。"); };
    if (scene.pickModels.size()!=scene.models.size() || scene.pickMeshGroups.size()!=scene.meshes.size() ||
        scene.geometry.meshGeosets.size()!=scene.meshes.size() || frame.poses.size()!=frame.instances.size()) return fail();
    frame.pickBounds.assign(frame.poses.size(),{});
    for (std::size_t i=0;i<frame.poses.size();++i) {
        const auto& pose=frame.poses[i]; const auto& instance=frame.instances[i];
        if (pose.hidden || pose.outsideView || pose.requestIndex==kBakedInstance) continue;
        if (instance.instance.model>=scene.models.size()) return fail();
        if (frame.sharedPoseRanges && !pose.animated) {
            if (pose.standPoseIndex>=scene.standPickBounds.size()) return fail();
            const auto& cached=scene.standPickBounds[pose.standPoseIndex]; auto& output=frame.pickBounds[i];
            output.collisionIncomplete=cached.collisionIncomplete;
            if (cached.available) {
                Affine matrix{},magnitude{};
                for (unsigned q=0;q<12;++q) { matrix[q]=instance.instance.transform[q]; magnitude[q]=std::abs(matrix[q]); }
                if (!IncludeBox(output,matrix,cached.box,magnitude)) return fail();
            }
            continue;
        }
        const auto& model=scene.models[instance.instance.model].model;
        const auto& input=scene.pickModels[instance.instance.model]; auto& output=frame.pickBounds[i];
        const bool owned = bool(input.ownedStaticAsset);
        if ((owned ? !OwnedMapping(scene, instance.instance.model, input) : !input.source) || !Fits({model.firstMesh,model.meshCount},scene.meshes.size()) ||
            !Fits(pose.collisionMatrices,frame.collisionMatrices.size()) ||
            pose.collisionMatrices.count!=(owned ? 0 : input.source->collisionShapes.size())) return fail();
        for (std::uint32_t local=0;local<model.meshCount;++local) {
            const auto meshIndex=model.firstMesh+local,geoset=scene.geometry.meshGeosets[meshIndex];
            if (!owned && geoset>=input.source->geosets.size()) return fail();
            if ((!owned && (input.source->geosets[geoset].selectionFlags&4u)) || !VisibleMesh(scene,frame,instance,meshIndex,local)) continue;
            const auto groups=scene.pickMeshGroups[meshIndex]; const auto& mesh=scene.meshes[meshIndex];
            if (!Fits(groups,scene.pickGroupBounds.size())) return fail();
            for (std::uint32_t k=0;k<groups.count;++k) {
                const auto& bounds=scene.pickGroupBounds[groups.offset+k];
                const auto group=static_cast<std::uint64_t>(instance.firstGroup)+mesh.firstGroup+bounds.group;
                if (bounds.group>=mesh.groupCount || group>=frame.groups.size()) return fail();
                std::array<double,6> box{}; std::copy(bounds.box.begin(),bounds.box.end(),box.begin());
                const auto& matrix=frame.groups[static_cast<std::size_t>(group)];
                if (!IncludeBox(output,Compose(instance.instance.transform,matrix),box,ComposeMagnitude(instance.instance.transform,matrix))) return fail();
            }
        }
        for (std::uint32_t k=0;k<pose.collisionMatrices.count;++k) {
            const auto& shape=input.source->collisionShapes[k]; const auto& node=frame.collisionMatrices[pose.collisionMatrices.offset+k];
            if (!Supported(shape) || !node.available) { output.collisionIncomplete=true; continue; }
            const auto matrix=Compose(instance.instance.transform,node.matrix);
            if (!Inverse(matrix)) { output.collisionIncomplete=true; continue; }
            if (!IncludeBox(output,matrix,CollisionBox(shape),ComposeMagnitude(instance.instance.transform,node.matrix))) { output.collisionIncomplete=true; }
        }
    }
    return Status::Ok();
}

Result<ModelPickResult> PickAnimatedModelInstances(const AnimatedModelScene& scene, const AnimationFrame& frame,
    const ModelPickVisibilitySnapshot& visibility, const ModelPickBinding& expected,
    const TerrainRay& ray, double maxDistance, const ModelPickLimits& limits) try {
    using Output=Result<ModelPickResult>;
    const auto fail=[](const char* code,const char* message) { return Output::Failure("Render.Picking",code,message); };
    const ModelPickLimits maximum;
    if (limits.maxCandidates>maximum.maxCandidates ||
        limits.maxTriangles>maximum.maxTriangles || limits.maxShapes>maximum.maxShapes ||
        limits.maxHits>maximum.maxHits || limits.maxIssues>maximum.maxIssues)
        return fail("MODEL_PICK_LIMIT_INVALID","精确点选预算只能收紧，不能超过硬上限。");
    if (!std::isfinite(maxDistance) || maxDistance<0 || maxDistance>1e9 ||
        !std::all_of(ray.origin.begin(),ray.origin.end(),[](double x) { return std::isfinite(x) && std::abs(x)<=1e9; }) ||
        !std::all_of(ray.direction.begin(),ray.direction.end(),[](double x) { return std::isfinite(x) && std::abs(x)<=1e9; }))
        return fail("MODEL_PICK_RAY_INVALID","精确点选射线或距离不在有限范围内。");
    const double length=std::sqrt(Dot(ray.direction,ray.direction));
    if (!(length>1e-12)) return fail("MODEL_PICK_RAY_INVALID","精确点选射线方向为空。");
    Point direction=ray.direction; for (auto& x:direction) x/=length;
    if (visibility.binding!=expected || !expected.sceneVersion || !expected.contentVersion || !expected.generation ||
        !expected.modelEpoch || !expected.presentationRevision || !expected.view.surfaceEpoch || !expected.view.sceneEpoch ||
        !expected.view.viewEpoch || !expected.view.frameCount || scene.sceneVersion!=expected.sceneVersion ||
        frame.sceneVersion!=expected.sceneVersion || scene.generation!=expected.generation ||
        scene.documentRevision!=expected.documentRevision || frame.documentRevision!=expected.documentRevision ||
        frame.contentVersion!=expected.contentVersion)
        return fail("MODEL_PICK_FRAME_STALE","点选输入不属于同一次已完成场景、姿势、呈现与视图身份。");
    if (visibility.visible.size()!=scene.specs.size() || visibility.selectable.size()!=scene.specs.size() ||
        frame.states.size()!=scene.specs.size() || frame.poses.size()!=frame.instances.size() ||
        frame.pickBounds.size()!=frame.instances.size() || frame.instances.size()!=scene.geometry.instances.size() ||
        frame.instances.size()!=scene.geometry.instanceRequests.size() || scene.pickModels.size()!=scene.models.size() ||
        scene.pickMeshGroups.size()!=scene.meshes.size() || scene.geometry.meshGeosets.size()!=scene.meshes.size() ||
        scene.specs.size()>=UINT32_MAX || frame.instances.size()>=UINT32_MAX)
        return fail("MODEL_PICK_MAPPING_INVALID","精确点选的请求、实例和实际网格映射不一致或超出硬预算。");
    ModelPickResult output; output.binding=expected;
    const auto issue=[&](const AnimationPoseLink& pose,const char* code,const char* message,
        std::optional<std::uint32_t> geoset={},std::optional<std::uint32_t> shape={}) {
        ++output.issueTotal;
        if (output.issues.size()<limits.maxIssues) output.issues.push_back({pose.identity,pose.requestIndex,geoset,shape,code,message});
    };
    const auto budget=[&](const AnimationPoseLink& pose,const char* message) {
        output.budgetExceeded=true; output.meshComplete=false; output.collisionComplete=false;
        issue(pose,"MODEL_PICK_BUDGET_EXCEEDED",message);
    };
    std::uint32_t traversedMeshes=0,traversedLayers=0;
    for (std::uint32_t i=0;i<frame.instances.size();++i) {
        const auto& pose=frame.poses[i]; const auto& instance=frame.instances[i];
        if (pose.requestIndex==kBakedInstance) continue;
        if (pose.requestIndex>=scene.specs.size() || pose.renderedInstance!=i ||
            scene.geometry.instanceRequests[i]!=pose.requestIndex || pose.identity!=scene.specs[pose.requestIndex].identity ||
            pose.identity!=frame.states[pose.requestIndex].identity || !pose.identity.value || !pose.identity.generation ||
            visibility.visible[pose.requestIndex]>1 || visibility.selectable[pose.requestIndex]>1)
            return fail("MODEL_PICK_MAPPING_INVALID","点选实例身份或可见/可选快照非法。");
        if (pose.hidden || pose.outsideView || !visibility.visible[pose.requestIndex] || !visibility.selectable[pose.requestIndex]) continue;
        if (!pose.evaluationEpoch || !pose.poseVersion || instance.instance.model>=scene.models.size() ||
            instance.instance.model!=scene.geometry.instances[i].model ||
            !Fits({pose.firstGroup,pose.groupCount},frame.groups.size()) || !Fits({pose.firstAlpha,pose.alphaCount},frame.alphas.size()) ||
            pose.firstGroup!=instance.firstGroup || pose.firstAlpha!=instance.firstAlpha)
            return fail("MODEL_PICK_MAPPING_INVALID","当前实例没有绑定的实际姿势片段。");
        if (limits.maxInstances && output.testedInstances==limits.maxInstances) { budget(pose,"点选实例扫描超过本次工作预算。"); break; }
        ++output.testedInstances;
        const auto& bound=frame.pickBounds[i];
        if (bound.available && (!std::all_of(bound.box.begin(),bound.box.end(),[](double x) { return std::isfinite(x); }) ||
            bound.box[0]>bound.box[3] || bound.box[1]>bound.box[4] || bound.box[2]>bound.box[5]))
            return fail("MODEL_PICK_MAPPING_INVALID","当前姿势粗筛边界非法。");
        if (bound.collisionIncomplete) {
            output.collisionComplete=false;
            issue(pose,"MODEL_PICK_COLLISION_INCOMPLETE","部分碰撞没有可证明的当前节点形状；已知 mesh 仍独立查询。");
        }
        if (!bound.available || !RayBox(ray.origin,direction,bound.box,maxDistance)) continue;
        if (output.candidates==limits.maxCandidates) { budget(pose,"粗筛候选数超过本次预算。"); break; }
        ++output.candidates;
        const auto& model=scene.models[instance.instance.model]; const auto& input=scene.pickModels[instance.instance.model];
        const bool owned = bool(input.ownedStaticAsset);
        if ((owned ? !OwnedMapping(scene, instance.instance.model, input) : !input.source) || !Fits({model.model.firstMesh,model.model.meshCount},scene.meshes.size()) ||
            model.groupCount!=pose.groupCount || model.model.meshCount!=pose.alphaCount ||
            !Fits(pose.collisionMatrices,frame.collisionMatrices.size()) || pose.collisionMatrices.count!=(owned ? 0 : input.source->collisionShapes.size()))
            return fail("MODEL_PICK_MAPPING_INVALID","候选没有实际模型来源或完整 owned 片段。");
        std::optional<PreciseModelPickHit> nearestMesh,nearestCollision;
        for (std::uint32_t k=0;k<pose.collisionMatrices.count;++k) {
            if (output.shapes==limits.maxShapes) { budget(pose,"碰撞形状数超过本次预算。"); break; }
            ++output.shapes;
            const auto& shape=input.source->collisionShapes[k]; const auto& node=frame.collisionMatrices[pose.collisionMatrices.offset+k];
            if (!Supported(shape)) {
                output.collisionComplete=false;
                issue(pose,"MODEL_PICK_COLLISION_LAYOUT_UNVERIFIED","碰撞原值保留；此布局或数值尚未获准精确求交。",{},shape.sourceIndex); continue;
            }
            const auto inverse=node.available ? Inverse(Compose(instance.instance.transform,node.matrix)) : std::nullopt;
            if (!inverse) {
                output.collisionComplete=false;
                issue(pose,"MODEL_PICK_COLLISION_NODE_UNAVAILABLE","碰撞没有可逆的当前单节点矩阵，不拿平均组或 identity 代替。",{},shape.sourceIndex); continue;
            }
            const auto localOrigin=Apply(*inverse,ray.origin),localDirection=Apply(*inverse,direction,true);
            const auto distance=shape.sourceType==0 ? RayBox(localOrigin,localDirection,CollisionBox(shape),maxDistance) : RaySphere(localOrigin,localDirection,shape,maxDistance);
            if (distance && (!nearestCollision || *distance<nearestCollision->distance))
                nearestCollision=PreciseModelPickHit{pose.identity,pose.requestIndex,i,4,*distance,{},{},shape.sourceIndex};
        }
        if (output.budgetExceeded) break;
        for (std::uint32_t local=0;local<model.model.meshCount;++local) {
            if (traversedMeshes==maximum.maxTriangles) { budget(pose,"候选 mesh 遍历超过硬预算。"); break; }
            ++traversedMeshes;
            const auto meshIndex=model.model.firstMesh+local,geoset=scene.geometry.meshGeosets[meshIndex];
            const auto& mesh=scene.meshes[meshIndex];
            if ((!owned && geoset>=input.source->geosets.size()) || !Fits({mesh.mesh.firstIndex,mesh.mesh.indexCount},scene.geometry.indices.size()) ||
                !Fits({mesh.mesh.firstLayer,mesh.mesh.layerCount},scene.geometry.layers.size()) || mesh.mesh.indexCount%3 ||
                mesh.firstGroup>pose.groupCount || mesh.groupCount>pose.groupCount-mesh.firstGroup)
                return fail("MODEL_PICK_MAPPING_INVALID","候选网格实际索引、geoset 或组片段非法。");
            bool layerBudgetExceeded=false;
            const bool visible=VisibleMesh(scene,frame,instance,meshIndex,local,&traversedLayers,&layerBudgetExceeded);
            if (layerBudgetExceeded) { budget(pose,"候选材质层遍历超过硬预算。"); break; }
            if (!visible) continue;
            const auto flags=owned ? 0u : input.source->geosets[geoset].selectionFlags;
            if (flags&4u) continue;
            if (flags&~4u) {
                output.meshComplete=false;
                issue(pose,"MODEL_PICK_SELECTION_FLAGS_UNVERIFIED","未知选择标志没有静默当成可选网格。",geoset); continue;
            }
            for (std::uint32_t q=0;q<mesh.mesh.indexCount;q+=3) {
                if (output.triangles==limits.maxTriangles) { budget(pose,"真实三角形数超过本次预算。"); break; }
                ++output.triangles; std::array<Point,3> points{};
                for (unsigned vertex=0;vertex<3;++vertex) {
                    const auto index=static_cast<std::uint64_t>(mesh.mesh.baseVertex)+scene.geometry.indices[mesh.mesh.firstIndex+q+vertex];
                    if (index>=scene.vertices.size()) return fail("MODEL_PICK_MAPPING_INVALID","真实三角形顶点越界。");
                    const auto& value=scene.vertices[static_cast<std::size_t>(index)];
                    const auto group=static_cast<std::uint64_t>(instance.firstGroup)+mesh.firstGroup+value.group;
                    if (value.group>=mesh.groupCount || group>=frame.groups.size())
                        return fail("MODEL_PICK_MAPPING_INVALID","真实三角形 shader 组索引越界。");
                    points[vertex]=SkinnedPoint(instance.instance.transform,frame.groups[static_cast<std::size_t>(group)],{value.vertex.x,value.vertex.y,value.vertex.z});
                    if (!std::all_of(points[vertex].begin(),points[vertex].end(),[](double x) { return std::isfinite(x); }))
                        return fail("MODEL_PICK_GEOMETRY_INVALID","实际姿势三角形不是有限坐标。");
                }
                const auto distance=RayTriangle(ray.origin,direction,points[0],points[1],points[2],maxDistance);
                if (distance && (!nearestMesh || *distance<nearestMesh->distance))
                    nearestMesh=PreciseModelPickHit{pose.identity,pose.requestIndex,i,8,*distance,geoset,q/3,{}};
            }
            if (output.budgetExceeded) break;
        }
        if (nearestMesh) output.hits.push_back(*nearestMesh);
        else if (nearestCollision) output.hits.push_back(*nearestCollision);
        if (output.budgetExceeded) break;
    }
    std::sort(output.hits.begin(),output.hits.end(),[](const auto& a,const auto& b) {
        if (a.grade!=b.grade) return a.grade>b.grade;
        return std::tie(a.distance,a.identity.value,a.identity.generation,a.requestIndex,a.geoset,a.triangle,a.collisionShape)<
            std::tie(b.distance,b.identity.value,b.identity.generation,b.requestIndex,b.geoset,b.triangle,b.collisionShape);
    });
    if (output.hits.size()>limits.maxHits) { output.hits.resize(limits.maxHits); output.hitsTruncated=true; }
    return Output::Success(std::move(output));
} catch (const std::bad_alloc&) {
    return Result<ModelPickResult>::Failure("Render.Picking","MODEL_PICK_ALLOCATION_FAILED","精确点选内存分配失败。");
}
} // namespace War3Map::Render
