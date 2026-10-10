#include "AnimationVisibility.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <numeric>

namespace War3Map::Render {
namespace {
using Point = std::array<double, 3>;
using Box = std::array<double, 6>;
constexpr double kMaximumEnvelope = 1e9;
constexpr std::uint32_t kNone = UINT32_MAX;

double Dot(const Point& a, const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double Length(const Point& a) { return std::sqrt(Dot(a,a)); }
Point Cross(const Point& a, const Point& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
bool FiniteBox(const Box& box) {
    for (unsigned a=0;a<3;++a) if (!std::isfinite(box[a]) || !std::isfinite(box[a+3]) || box[a]>box[a+3]) return false;
    return true;
}
void Include(Box& box, const Box& other) {
    for (unsigned a=0;a<3;++a) { box[a]=std::min(box[a],other[a]); box[a+3]=std::max(box[a+3],other[a+3]); }
}
void IncludeRadius(Box& box, double radius) { Include(box,{-radius,-radius,-radius,radius,radius,radius}); }
std::uint32_t Word(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value=0;
    for (unsigned k=0;k<4;++k) value|=std::to_integer<std::uint32_t>(bytes[at+k])<<(8*k);
    return value;
}
float Number(std::span<const std::byte> bytes, std::size_t at) { return std::bit_cast<float>(Word(bytes,at)); }

// 求值器在首末键接缝可做区间外插值。不能把只有声明盒或按键凸包当作已证动态边界。
// 对多键 TRS，先要求每个实际动作区间闭合；无法证明的接缝整资产保守放行。
bool ClosedTrack(const MdxNodeTrack<3>& track, const MdxModel& model) {
    if (track.keys.size()<2 || track.interpolation==MdxInterpolation::None) return true;
    if (!std::is_sorted(track.keys.begin(),track.keys.end(),[](const auto& a,const auto& b) { return a.frame<b.frame; })) return false;
    if (track.globalSequence!=UINT32_MAX) {
        if (track.globalSequence>=model.globalSequences.size()) return false;
        const auto period=model.globalSequences[track.globalSequence];
        return !period || std::int64_t(track.keys.back().frame)-track.keys.front().frame>=period;
    }
    for (const auto& sequence:model.sequences) {
        if (!sequence.Usable()) return false;
        const auto first=std::lower_bound(track.keys.begin(),track.keys.end(),std::int64_t(sequence.start),
            [](const auto& key,auto time) { return key.frame<time; });
        const auto end=std::upper_bound(first,track.keys.end(),std::int64_t(sequence.end),
            [](auto time,const auto& key) { return time<key.frame; });
        if (end-first>1 && (std::int64_t(first->frame)!=sequence.start || std::int64_t(std::prev(end)->frame)!=sequence.end)) return false;
    }
    return true;
}
std::optional<Point> TrackMagnitude(const std::optional<MdxNodeTrack<3>>& optional,
    Point fallback, const MdxModel& model) {
    Point out=fallback;
    if (!optional) return out;
    const auto& track=*optional;
    if (!ClosedTrack(track,model) || static_cast<unsigned>(track.interpolation)>3) return {};
    for (const auto& key:track.keys) for (unsigned a=0;a<3;++a) {
        double maximum=std::abs(double(key.value[a]));
        if (track.interpolation==MdxInterpolation::Hermite) {
            // Hermite -> Bezier 控制点；凸包含整条闭合区间，不漏切线造成的过冲。
            maximum=std::max({maximum,std::abs(double(key.value[a])+double(key.outTangent[a])/3),
                std::abs(double(key.value[a])-double(key.inTangent[a])/3)});
        } else if (track.interpolation==MdxInterpolation::Bezier)
            maximum=std::max({maximum,std::abs(double(key.outTangent[a])),std::abs(double(key.inTangent[a]))});
        if (!std::isfinite(maximum) || maximum>kMaximumEnvelope) return {};
        out[a]=std::max(out[a],maximum);
    }
    return out;
}
struct NodeEnvelope { double offset=0, scale=1; };
using NodeEntry = std::pair<std::uint32_t,NodeEnvelope>;
std::optional<std::vector<NodeEntry>> NodeEnvelopes(const MdxModel& model) {
    std::vector<const MdxNode*> nodes; nodes.reserve(MdxNodeCount(model));
    ForEachMdxNode(model,[&](const auto& node) { nodes.push_back(&node); });
    std::vector<std::pair<std::uint32_t,std::uint32_t>> byId; byId.reserve(nodes.size());
    for (std::uint32_t i=0;i<nodes.size();++i) byId.emplace_back(nodes[i]->objectId,i);
    std::sort(byId.begin(),byId.end());
    if (std::adjacent_find(byId.begin(),byId.end(),[](const auto& a,const auto& b) { return a.first==b.first; })!=byId.end()) return {};
    const auto find=[&](std::uint32_t id) {
        const auto entry=std::lower_bound(byId.begin(),byId.end(),id,[](const auto& a,auto b) { return a.first<b; });
        return entry!=byId.end() && entry->first==id ? entry->second : kNone;
    };
    std::vector<std::uint8_t> done(nodes.size());
    std::vector<NodeEnvelope> bounds(nodes.size());
    std::vector<std::uint32_t> chain; chain.reserve(nodes.size());
    for (std::uint32_t start=0;start<nodes.size();++start) {
        auto at=start;
        while (at!=kNone && !done[at]) {
            done[at]=1; chain.push_back(at);
            const auto& node=*nodes[at];
            if (node.parentId<0) at=kNone;
            else {
                at=find(static_cast<std::uint32_t>(node.parentId));
                if (at==kNone) return {};
            }
        }
        if (at!=kNone && done[at]==1) return {}; // 父链循环；不把默认矩阵当包络。
        while (!chain.empty()) {
            const auto current=chain.back(); chain.pop_back(); const auto& node=*nodes[current];
            if ((node.flags&~MdxNodeNonTransformFlags(node.kind))!=0 || node.objectId>=model.pivots.size()) return {};
            // 相机依赖/继承剥离可能改写父变换，未证明时由 unknown 放行。
            const auto translation=TrackMagnitude(node.translation,{0,0,0},model);
            const auto scaling=TrackMagnitude(node.scaling,{1,1,1},model);
            if (!translation || !scaling) return {};
            const auto& pivot=model.pivots[node.objectId];
            const double scale=std::max({(*scaling)[0],(*scaling)[1],(*scaling)[2]});
            const double local=Length(*translation)+Length({pivot[0],pivot[1],pivot[2]})*(1+scale);
            auto envelope=NodeEnvelope{local,scale};
            if (node.parentId>=0) {
                const auto& parent=bounds[find(static_cast<std::uint32_t>(node.parentId))];
                envelope={parent.offset+parent.scale*local,parent.scale*scale};
            }
            if (!std::isfinite(envelope.offset) || !std::isfinite(envelope.scale) ||
                envelope.offset>kMaximumEnvelope || envelope.scale>kMaximumEnvelope) return {};
            bounds[current]=envelope; done[current]=2;
        }
    }
    std::vector<NodeEntry> result; result.reserve(nodes.size());
    for (const auto& [id,index]:byId) result.emplace_back(id,bounds[index]);
    return result;
}
std::optional<double> ParticleMagnitude(const Particles::Emitter& emitter, Particles::Parameter parameter, float fallback) {
    const auto& track=emitter.tracks[static_cast<std::size_t>(parameter)];
    double maximum=std::abs(double(fallback));
    if (track.interpolation>3) return {};
    for (const auto& key:track.keys) {
        double value=std::abs(double(key.value[0]));
        if (track.interpolation==2) value=std::max({value,std::abs(double(key.value[0])+double(key.out[0])/3),
            std::abs(double(key.value[0])-double(key.in[0])/3)});
        else if (track.interpolation==3) value=std::max({value,std::abs(double(key.out[0])),std::abs(double(key.in[0]))});
        if (!std::isfinite(value)) return {};
        maximum=std::max(maximum,value);
        if (!std::isfinite(maximum) || maximum>kMaximumEnvelope) return {};
    }
    return maximum;
}
AnimationVisibilityBounds CompleteBounds(const AnimationAsset& asset, bool particlesEnabled) {
    auto output=asset.visibilityBounds;
    const auto unknown=[] { return AnimationVisibilityBounds{}; };
    if (asset.ownedStaticAsset) {
        // 封存的原创静态顶点就是实际marker边界，不是未知的native占地/骨架范围。
        if (asset.parsed || asset.compiled.Valid() || asset.particleCapability.available) return unknown();
        const auto maximum = std::numeric_limits<double>::max();
        output = {}; output.box = {maximum, maximum, maximum, -maximum, -maximum, -maximum};
        for (const auto& mesh : asset.ownedStaticAsset->Meshes()) for (const auto index : mesh.indices) {
            if (index >= mesh.vertices.size()) return unknown();
            const auto& vertex = mesh.vertices[index];
            const double xyz[]{vertex.x, vertex.y, vertex.z};
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(xyz[axis]) || std::abs(xyz[axis]) > kMaximumEnvelope) return unknown();
                output.box[axis] = std::min(output.box[axis], xyz[axis]);
                output.box[axis + 3] = std::max(output.box[axis + 3], xyz[axis]);
            }
        }
        output.available = output.box[0] <= output.box[3];
        return output.available ? output : unknown();
    }
    if (!output.available || !asset.parsed) return unknown();
    const auto& model=*asset.parsed;
    const auto nodes=NodeEnvelopes(model);
    if (!nodes) return unknown();
    double vertexRadius=0,geometryRadius=0;
    for (const auto& geoset:model.geosets) for (const auto& p:geoset.positions) {
        const auto radius=Length({p[0],p[1],p[2]});
        if (!std::isfinite(radius)) return unknown();
        vertexRadius=std::max(vertexRadius,radius);
    }
    geometryRadius=vertexRadius;
    // 组矩阵是成员矩阵的平均；逐节点球的最大值亦包住任何成员平均，含所有动画/全局相位。
    for (const auto& [id,node]:*nodes) {
        (void)id; geometryRadius=std::max(geometryRadius,node.offset+node.scale*vertexRadius);
    }
    if (!std::isfinite(geometryRadius) || geometryRadius>kMaximumEnvelope) return unknown();
    IncludeRadius(output.box,geometryRadius);
    if (particlesEnabled && asset.particleCapability.available) {
        if (!asset.particles) return unknown();
        for (const auto& emitter:asset.particles->emitters) {
            const auto node=std::lower_bound(nodes->begin(),nodes->end(),emitter.nodeId,
                [](const auto& entry,auto id) { return entry.first<id; });
            if (node==nodes->end() || node->first!=emitter.nodeId || emitter.nodeId>=model.pivots.size() ||
                !std::isfinite(emitter.life) || emitter.life<0) return unknown();
            const auto speed=ParticleMagnitude(emitter,Particles::Parameter::Speed,emitter.speed);
            const auto variation=ParticleMagnitude(emitter,Particles::Parameter::Variation,emitter.variation);
            const auto gravity=ParticleMagnitude(emitter,Particles::Parameter::Gravity,emitter.gravity);
            const auto width=ParticleMagnitude(emitter,Particles::Parameter::Width,emitter.width);
            const auto length=ParticleMagnitude(emitter,Particles::Parameter::Length,emitter.length);
            const auto above=ParticleMagnitude(emitter,Particles::Parameter::Above,emitter.above);
            const auto below=ParticleMagnitude(emitter,Particles::Parameter::Below,emitter.below);
            if (!speed || !variation || !gravity || !width || !length || !above || !below || !std::isfinite(emitter.tailLength)) return unknown();
            const auto& pivot=model.pivots[emitter.nodeId]; const double life=emitter.life;
            const double tail=std::abs(double(emitter.tailLength));
            const bool localGravity=emitter.kind==Particles::Kind::Particle2 && (emitter.flags&0x80000u);
            const double travel=emitter.kind==Particles::Kind::Particle2 ? *speed*(1+*variation)*(life+tail) : 0;
            const double spawn=emitter.kind==Particles::Kind::Particle2 ? std::hypot(*width,*length)*0.5 : std::max(*above,*below);
            const double fall=*gravity*(0.5*life*life+life*tail);
            const double radius=node->second.offset+node->second.scale*(Length({pivot[0],pivot[1],pivot[2]})+spawn+travel)+
                (localGravity ? fall : 0);
            double sprite=0;
            if (emitter.kind==Particles::Kind::Particle2) for (const auto scale:emitter.scales) {
                if (!std::isfinite(scale)) return unknown();
                sprite=std::max(sprite,std::abs(double(scale))*std::sqrt(2.0));
            }
            const double padding=sprite+(localGravity ? 0 : fall);
            if (!std::isfinite(radius) || !std::isfinite(padding) || radius>kMaximumEnvelope || padding>kMaximumEnvelope) return unknown();
            IncludeRadius(output.box,radius); output.worldPadding=std::max(output.worldPadding,padding);
        }
    }
    // 为长父链的 float 求值增加相对余量；六平面另外有相机/世界单位余量。
    for (unsigned a=0;a<3;++a) {
        const auto margin=(1+std::max(std::abs(output.box[a]),std::abs(output.box[a+3])))*
            64*std::numeric_limits<float>::epsilon()*(1+MdxNodeCount(model));
        output.box[a]-=margin; output.box[a+3]+=margin;
    }
    return output;
}
std::optional<Box> WorldBounds(const AnimationVisibilityBounds& local, const std::array<float,12>& transform) {
    if (!local.available) return {};
    Box out{};
    for (unsigned row=0;row<3;++row) {
        double low=transform[row*4+3],high=low;
        for (unsigned a=0;a<3;++a) {
            const auto x=double(transform[row*4+a])*local.box[a],y=double(transform[row*4+a])*local.box[a+3];
            low+=std::min(x,y); high+=std::max(x,y);
        }
        out[row]=low-local.worldPadding; out[row+3]=high+local.worldPadding;
    }
    return FiniteBox(out) ? std::optional(out) : std::nullopt;
}
struct Plane { Point normal{}; double offset=0; };
struct Frustum {
    std::array<Plane,6> planes;
    double margin=0;
    bool Outside(const Box& bounds) const {
        for (const auto& plane:planes) {
            Point corner{};
            for (unsigned a=0;a<3;++a) corner[a]=bounds[a+(plane.normal[a]>=0 ? 3 : 0)];
            const auto numeric=128*std::numeric_limits<float>::epsilon()*(1+std::abs(plane.offset)+Length(corner));
            if (Dot(plane.normal,corner)+plane.offset < -margin-numeric) return true;
        }
        return false;
    }
};
Frustum MakeFrustum(const AnimationView& view) {
    const auto camera=AnimationWorldCamera(view.camera);
    const Point eye{camera.eye[0],camera.eye[1],camera.eye[2]};
    Point forward{double(view.camera.targetX)-eye[0],double(view.camera.targetY)-eye[1],double(view.camera.targetZ)-eye[2]};
    const auto length=Length(forward); for (auto& component:forward) component/=length;
    auto right=Cross(forward,{0,0,1}); const auto rightLength=Length(right); for (auto& component:right) component/=rightLength;
    const auto up=Cross(right,forward);
    // 与 XMMatrixPerspectiveFovRH(XM_PIDIV4,width/height,1,1000000) 一致。
    const double vertical=std::tan(double(std::numbers::pi_v<float>/8));
    const double horizontal=vertical*double(float(view.physicalWidth)/float(view.physicalHeight));
    Frustum result; result.margin=view.conservativeMargin;
    result.planes[0]={forward,-Dot(forward,eye)-1};
    Point back=forward; for (auto& component:back) component=-component;
    // 深度矩阵的 float 舍入使实际远裁剪距离大于名义1e6；按 renderer 的 range/offset
    // 推导有效平面并向外取，避免剔除仍通过真实 clip 测试的极远实例。
    constexpr float nearDepth=1.0f,farDepth=1000000.0f;
    constexpr float range=farDepth/(nearDepth-farDepth),depthOffset=nearDepth*range;
    const double conservativeFar=std::max(double(farDepth),double(depthOffset)/(double(range)+1));
    result.planes[1]={back,-Dot(back,eye)+conservativeFar};
    for (unsigned side=0;side<4;++side) {
        const auto& axis=side<2 ? right : up; const double slope=side<2 ? horizontal : vertical;
        Point normal{};
        for (unsigned a=0;a<3;++a) normal[a]=forward[a]*slope+axis[a]*(side%2 ? -1 : 1);
        const auto norm=Length(normal); for (auto& component:normal) component/=norm;
        result.planes[side+2]={normal,-Dot(normal,eye)};
    }
    return result;
}
} // namespace

void ReadAnimationVisibilityBounds(AnimationAsset& asset, std::span<const std::byte> bytes) noexcept {
    asset.visibilityBounds={};
    if (!asset.parsed || asset.parsed->version!=800 || bytes.size()<4 || std::memcmp(bytes.data(),"MDLX",4)!=0) return;
    Box box{}; bool modelSeen=false; std::size_t sequences=0;
    for (std::size_t at=4;at<bytes.size();) {
        if (bytes.size()-at<8) return;
        const auto size=Word(bytes,at+4); const auto body=at+8;
        if (size>bytes.size()-body) return;
        if (std::memcmp(bytes.data()+at,"MODL",4)==0) {
            if (modelSeen || size<372) return;
            const double radius=Number(bytes,body+340);
            if (!std::isfinite(radius) || radius<0) return;
            for (unsigned a=0;a<3;++a) { box[a]=Number(bytes,body+344+a*4); box[a+3]=Number(bytes,body+356+a*4); }
            if (!FiniteBox(box)) return;
            IncludeRadius(box,radius); modelSeen=true;
        } else if (std::memcmp(bytes.data()+at,"SEQS",4)==0) {
            if (!modelSeen || size%132) return;
            for (std::size_t record=body;record<body+size;record+=132) {
                const double radius=Number(bytes,record+104); Box declared{};
                for (unsigned a=0;a<3;++a) { declared[a]=Number(bytes,record+108+a*4); declared[a+3]=Number(bytes,record+120+a*4); }
                if (!std::isfinite(radius) || radius<0 || !FiniteBox(declared)) return;
                Include(box,declared); IncludeRadius(box,radius); ++sequences;
            }
        }
        at=body+size;
    }
    if (modelSeen && sequences==asset.parsed->sequences.size()) asset.visibilityBounds={box,0,true};
}

Status CheckAnimationView(const AnimationView& view) {
    const auto& camera=view.camera;
    const float values[]{camera.targetX,camera.targetY,camera.targetZ,camera.distance,camera.yawRadians,camera.pitchRadians};
    if (camera.structSize!=sizeof(camera) || !view.physicalWidth || !view.physicalHeight ||
        view.physicalWidth>16384 || view.physicalHeight>16384 || !std::isfinite(view.conservativeMargin) ||
        view.conservativeMargin<0 || view.conservativeMargin>1e7f || !(camera.distance>0) ||
        !std::all_of(std::begin(values),std::end(values),[](float value) { return std::isfinite(value) && std::abs(value)<=1e7f; }) ||
        std::abs(std::cos(double(camera.pitchRadians)))<1e-5)
        return AnimationFailure("ANIMATION_VIEW_INVALID","视锥相机、物理绘制尺寸或保守余量无效。");
    const auto world=AnimationWorldCamera(camera);
    const Point direction{double(camera.targetX)-world.eye[0],double(camera.targetY)-world.eye[1],double(camera.targetZ)-world.eye[2]};
    if (!(Length(direction)>1e-6) || !(Length(Cross(direction,{0,0,1}))>1e-6))
        return AnimationFailure("ANIMATION_VIEW_INVALID","视锥相机视点/目标/上向量不能建立有限透视坐标系。");
    return Status::Ok();
}

struct AnimationVisibilityIndex {
    struct Node { Box box{}; std::uint32_t left=kNone,right=kNone,parent=kNone,request=kNone; };
    std::vector<AnimationVisibilityBounds> assets;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> leaves,unknown,changed;
    std::vector<std::array<float,12>> transforms;
    std::vector<std::pair<std::uint64_t,std::uint32_t>> identities;
    std::uint64_t sceneVersion=0;
    std::uint32_t root=kNone;

    std::uint32_t FindIdentity(std::uint64_t identity) const {
        const auto found=std::lower_bound(identities.begin(),identities.end(),identity,
            [](const auto& entry,auto value) { return entry.first<value; });
        return found!=identities.end() && found->first==identity ? found->second : kNone;
    }

    std::uint32_t Build(std::vector<std::uint32_t>& requests, std::size_t begin, std::size_t end, std::uint32_t parent) {
        const auto node=static_cast<std::uint32_t>(nodes.size()); nodes.emplace_back(); nodes[node].parent=parent;
        auto box=nodes[leaves[requests[begin]]].box;
        for (std::size_t i=begin+1;i<end;++i) Include(box,nodes[leaves[requests[i]]].box);
        nodes[node].box=box;
        if (end-begin==1) {
            // 叶子已有实际世界盒，树构造只接父节点，不复制一次请求表。
            const auto leaf=leaves[requests[begin]]; nodes[leaf].parent=parent;
            nodes.pop_back(); return leaf;
        }
        unsigned axis=0;
        for (unsigned a=1;a<3;++a) if (box[a+3]-box[a]>box[axis+3]-box[axis]) axis=a;
        const auto middle=begin+(end-begin)/2;
        std::nth_element(requests.begin()+static_cast<std::ptrdiff_t>(begin),requests.begin()+static_cast<std::ptrdiff_t>(middle),
            requests.begin()+static_cast<std::ptrdiff_t>(end),[&](auto a,auto b) {
                const auto& x=nodes[leaves[a]].box; const auto& y=nodes[leaves[b]].box;
                const auto centerX=x[axis]+x[axis+3],centerY=y[axis]+y[axis+3];
                return centerX!=centerY ? centerX<centerY : a<b;
            });
        const auto left=Build(requests,begin,middle,node); const auto right=Build(requests,middle,end,node);
        nodes[node].left=left; nodes[node].right=right; return node;
    }
    void Refit(std::uint32_t node) {
        for (auto parent=nodes[node].parent;parent!=kNone;parent=nodes[parent].parent) {
            nodes[parent].box=nodes[nodes[parent].left].box;
            Include(nodes[parent].box,nodes[nodes[parent].right].box);
        }
    }
    void Query(std::uint32_t node, const Frustum& view, AnimationFrame& frame) const {
        ++frame.timings.visibilityNodesVisited;
        if (nodes[node].request!=kNone) ++frame.timings.visibilityBoundsTests;
        if (view.Outside(nodes[node].box)) return;
        if (nodes[node].request!=kNone) {
            frame.requestVisibility[nodes[node].request]=1; return;
        }
        Query(nodes[node].left,view,frame); Query(nodes[node].right,view,frame);
    }
};

Status PrepareAnimationVisibility(std::shared_ptr<AnimationVisibilityIndex>& index,
    const AnimatedModelScene& scene, std::span<const AnimationAsset> assets,
    std::span<const std::uint32_t> requestAssets, AnimationFrame& frame, std::span<const AnimationEdit> edits,
    std::uint64_t maxVisibilityBytes) {
    if (!frame.worldView) {
        // 无过滤帧的变换不经过叶子更新。以后重新启用视图时从真实状态重新建一次索引。
        index.reset(); return Status::Ok();
    }
    if (auto status=CheckAnimationView(*frame.worldView); !status.Succeeded()) return status;
    if (requestAssets.size()!=frame.states.size() || frame.states.size()!=scene.specs.size())
        return AnimationFailure("ANIMATION_VISIBILITY_MAPPING","视锥请求、状态与资产映射不一致。");
    // vector 容量在 reserve 前计入；无每实例散列节点，身份用已排序紧凑表。
    // 同时核算 BVH、变换、未知列表、refit 临时2N及候选帧mask，不依靠实例数魔法上限。
    constexpr std::uint64_t perRequest=2*sizeof(AnimationVisibilityIndex::Node)+sizeof(std::array<float,12>)+
        sizeof(std::pair<std::uint64_t,std::uint32_t>)+6*sizeof(std::uint32_t)+sizeof(std::uint8_t);
    const auto assetBytes=std::uint64_t(assets.size())*sizeof(AnimationVisibilityBounds);
    std::uint64_t maximumNodeBytes=0;
    for (const auto& asset:assets) if (asset.parsed) maximumNodeBytes=std::max(maximumNodeBytes,
        std::uint64_t(MdxNodeCount(*asset.parsed))*(sizeof(const MdxNode*)+sizeof(std::pair<std::uint32_t,std::uint32_t>)+
            sizeof(std::uint8_t)+sizeof(NodeEnvelope)+sizeof(std::uint32_t)+sizeof(NodeEntry)));
    if (maxVisibilityBytes<sizeof(AnimationVisibilityIndex) || assetBytes>maxVisibilityBytes-sizeof(AnimationVisibilityIndex) ||
        maximumNodeBytes>maxVisibilityBytes-sizeof(AnimationVisibilityIndex)-assetBytes ||
        frame.states.size()>(maxVisibilityBytes-sizeof(AnimationVisibilityIndex)-assetBytes-maximumNodeBytes)/perRequest)
        return AnimationFailure("ANIMATION_VISIBILITY_BUDGET","真实视锥缓存/构建内存超过独立容量；未截断实例或偷偷关闭过滤。");
    if (!index) {
        auto created=std::make_shared<AnimationVisibilityIndex>(); created->sceneVersion=scene.sceneVersion;
        created->assets.reserve(assets.size());
        for (const auto& asset:assets) created->assets.push_back(CompleteBounds(asset,scene.particlesEnabled));
        created->leaves.assign(frame.states.size(),kNone); created->transforms.reserve(frame.states.size());
        created->identities.reserve(frame.states.size()); created->unknown.reserve(frame.states.size()); created->changed.reserve(frame.states.size());
        created->nodes.reserve(frame.states.size()*2); std::vector<std::uint32_t> known; known.reserve(frame.states.size());
        for (std::uint32_t r=0;r<frame.states.size();++r) {
            if (requestAssets[r]>=created->assets.size()) return AnimationFailure("ANIMATION_VISIBILITY_MAPPING","视锥请求资产索引越界。");
            created->identities.emplace_back(frame.states[r].identity.value,r); created->transforms.push_back(frame.states[r].transform);
            const auto bounds=WorldBounds(created->assets[requestAssets[r]],frame.states[r].transform);
            if (!bounds) created->unknown.push_back(r);
            else {
                created->leaves[r]=static_cast<std::uint32_t>(created->nodes.size());
                created->nodes.push_back({*bounds,kNone,kNone,kNone,r}); known.push_back(r);
            }
        }
        std::sort(created->identities.begin(),created->identities.end());
        if (!known.empty()) created->root=created->Build(known,0,known.size(),kNone);
        for (const auto& edit:edits) if (edit.transform) {
            const auto found=created->FindIdentity(edit.identity.value);
            if (found!=kNone) created->changed.push_back(found);
        }
        index=std::move(created);
    } else {
        if (index->sceneVersion!=scene.sceneVersion || index->leaves.size()!=frame.states.size())
            return AnimationFailure("ANIMATION_VISIBILITY_STALE","视锥空间索引不属于当前动画场景。");
        std::vector<std::uint32_t> pending; pending.reserve(index->changed.size()+edits.size());
        pending.insert(pending.end(),index->changed.begin(),index->changed.end()); index->changed.clear();
        for (const auto& edit:edits) if (edit.transform) {
            const auto found=index->FindIdentity(edit.identity.value);
            if (found==kNone) return AnimationFailure("ANIMATION_VISIBILITY_STALE","视锥变换编辑身份不存在。");
            pending.push_back(found);
        }
        std::sort(pending.begin(),pending.end()); pending.erase(std::unique(pending.begin(),pending.end()),pending.end());
        for (const auto r:pending) if (index->transforms[r]!=frame.states[r].transform) {
            index->transforms[r]=frame.states[r].transform; index->changed.push_back(r);
            if (index->leaves[r]!=kNone) {
                const auto bounds=WorldBounds(index->assets[requestAssets[r]],frame.states[r].transform);
                if (!bounds) return AnimationFailure("ANIMATION_VISIBILITY_BOUNDS_INVALID","已知视锥包络的有限变换失败。");
                index->nodes[index->leaves[r]].box=*bounds; index->Refit(index->leaves[r]);
            }
        }
    }
    frame.requestVisibility.assign(frame.states.size(),0);
    for (const auto r:index->unknown) frame.requestVisibility[r]=1;
    frame.visibilityUnknownInstances=index->unknown.size();
    if (index->root!=kNone) index->Query(index->root,MakeFrustum(*frame.worldView),frame);
    frame.offscreenInstances=static_cast<std::size_t>(std::count(frame.requestVisibility.begin(),frame.requestVisibility.end(),std::uint8_t{0}));
    return Status::Ok();
}
} // namespace War3Map::Render
