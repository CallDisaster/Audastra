#include "ParticleMath.h"
#include "War3RenderCore/GameKeyRanges.h"
#include <bit>
#include <cstring>
#include <set>

namespace War3Map::Render::Particles {
namespace {
class Reader {
    std::span<const std::byte> bytes_;
    std::size_t at_=0,origin_=0;
    std::optional<Error>* localError_=nullptr;
public:
    explicit Reader(std::span<const std::byte> bytes,std::size_t origin=0):bytes_(bytes),origin_(origin){}
    void DeferLocalErrors(std::optional<Error>& error) { localError_=&error; }
    void LocalRequire(bool yes,const char* code,const char* message) {
        if(yes)return;
        if(!localError_)throw Error(code,message);
        if(!*localError_)localError_->emplace(code,message);
    }
    std::size_t Remaining() const { return bytes_.size()-at_; }
    std::size_t Offset() const { return origin_+at_; }
    void Need(std::size_t n) const { Require(n<=Remaining(),"PARTICLE_TRUNCATED","粒子块或轨迹越界。"); }
    std::uint32_t U32() { Need(4); std::uint32_t v=0; for(unsigned i=0;i<4;++i)v|=std::to_integer<std::uint32_t>(bytes_[at_++])<<(i*8); return v; }
    float Float() { const float v=std::bit_cast<float>(U32()); LocalRequire(std::isfinite(v),"PARTICLE_NUMBER_INVALID","粒子字段不是有限值。"); return v; }
    std::uint8_t Byte() { Need(1); return std::to_integer<std::uint8_t>(bytes_[at_++]); }
    Reader Take(std::size_t n) { Need(n); Reader r(bytes_.subspan(at_,n),Offset()); r.localError_=localError_; at_+=n; return r; }
    void Skip(std::size_t n) { (void)Take(n); }
    std::string Tag() { Need(4); const auto p=bytes_.data()+at_; at_+=4; return {reinterpret_cast<const char*>(p),4}; }
    Vec3 Vector() { return {Float(),Float(),Float()}; }
};
// gameOrder 非空表示预览模式：非全局轨迹的逆序键按原版逐动作区间接受，并记下第一条逆序边。
Track ReadTrack(Reader& r,std::uint32_t width,bool integer,std::uint32_t& keys,const ReadLimits& limits,
    RecoveredVisibilityTrack* recovery,std::optional<std::array<std::int32_t,2>>* gameOrder) {
    Track t;const auto n=r.U32();t.interpolation=r.U32();t.globalSequence=r.U32();t.width=width;
    Require(t.interpolation<=3 && (!integer||t.interpolation<=1),"PARTICLE_INTERPOLATION_UNSUPPORTED","未知粒子插值或整数槽位样条。");
    Require(n<=limits.maxKeys-keys,"PARTICLE_KEY_BUDGET","粒子轨迹总按键超限。");
    r.Need(static_cast<std::size_t>(n)*(4+width*4*(t.interpolation>1?3:1)));keys+=n;t.keys.reserve(n);
    const bool recoverSameFrame=recovery && !integer && width==1 && t.interpolation==0 && t.globalSequence==UINT32_MAX;
    const bool acceptOrder=gameOrder && t.globalSequence==UINT32_MAX;
    if(recovery)recovery->sourceKeyCount=n;
    for(std::uint32_t i=0;i<n;++i) {
        const auto frameOffset=r.Offset();
        Key k;k.frame=std::bit_cast<std::int32_t>(r.U32());
        const bool increasing=t.keys.empty()||k.frame>t.keys.back().frame;
        if(!recoverSameFrame&&!acceptOrder)r.LocalRequire(increasing,"PARTICLE_KEYS_INVALID","粒子按键必须严格递增。");
        const auto read=[&](Vec3& v){for(std::uint32_t c=0;c<width;++c) { if(integer) {const auto q=r.U32();r.LocalRequire(q<=16777216,"PARTICLE_SLOT_INVALID","整数贴图槽超过精确浮点范围。");v[c]=static_cast<float>(q);}else v[c]=r.Float(); }};
        read(k.value);if(t.interpolation>1){read(k.in);read(k.out);}
        if((recoverSameFrame||acceptOrder) && !increasing) {
            const auto& previous=t.keys.back();
            const bool same=recoverSameFrame && k.frame==previous.frame && std::isfinite(k.value[0]) && std::isfinite(previous.value[0]) &&
                std::bit_cast<std::uint32_t>(k.value[0])==std::bit_cast<std::uint32_t>(previous.value[0]);
            if(same)recovery->duplicateKeys.push_back({i,k.frame,frameOffset,std::bit_cast<std::uint32_t>(k.value[0])});
            else if(acceptOrder) { if(!*gameOrder)*gameOrder=std::array<std::int32_t,2>{previous.frame,k.frame}; }
            else r.LocalRequire(false,"PARTICLE_KEYS_INVALID","粒子按键必须严格递增。");
        }
        t.keys.push_back(k);
    }
    return t;
}
void Node(Reader& r,Emitter& e) {
    const auto n=r.U32();Require(n>=96,"PARTICLE_NODE_SIZE","公共节点头尺寸无效。");auto node=r.Take(n-4);
    node.Skip(80);e.nodeId=node.U32();node.U32();e.flags=node.U32();
    // Node的KGTR/KGRT/KGSC由已有MdxModel读取并验证；这里仅按包含式长度越过，禁止重复维护骨架公式。
}
Parameter Param(std::string_view tag,Kind kind) {
    if(kind==Kind::Particle2) {
        if(tag=="KP2E")return Parameter::Rate;
        if(tag=="KP2S")return Parameter::Speed;
        if(tag=="KP2R")return Parameter::Variation;
        if(tag=="KP2L")return Parameter::Latitude;
        if(tag=="KP2G")return Parameter::Gravity;
        if(tag=="KP2N")return Parameter::Length;
        if(tag=="KP2W")return Parameter::Width;
        if(tag=="KP2V")return Parameter::Visibility;
    } else {
        if(tag=="KRHA")return Parameter::Above;
        if(tag=="KRHB")return Parameter::Below;
        if(tag=="KRAL")return Parameter::Alpha;
        if(tag=="KRCO")return Parameter::Color;
        if(tag=="KRTX")return Parameter::TextureSlot;
        if(tag=="KRVS")return Parameter::Visibility;
    }
    throw Error("PARTICLE_TRACK_UNSUPPORTED","未知粒子/条带轨迹："+std::string(tag));
}
const char* TagName(Kind kind,Parameter p) {
    static constexpr std::array<const char*,static_cast<std::size_t>(Parameter::Count)> particle2{
        "KP2E","KP2S","KP2R","KP2L","KP2G","KP2N","KP2W","KP2V","","","","",""};
    static constexpr std::array<const char*,static_cast<std::size_t>(Parameter::Count)> ribbon{
        "","","","","","","","KRVS","KRHA","KRHB","KRAL","KRCO","KRTX"};
    return (kind==Kind::Particle2?particle2:ribbon)[static_cast<std::size_t>(p)];
}
void ReadEmitters(Reader r,Kind kind,Asset& out,std::uint32_t& keys,
    std::array<std::uint32_t,2>& sourceCounts,const ReadLimits& limits,ReadMode mode) {
    while(r.Remaining()) {
        Require(sourceCounts[0]+sourceCounts[1]<limits.maxEmitters,"PARTICLE_EMITTER_BUDGET","发射器数量超限。");
        const auto n=r.U32();Require(n>=4,"PARTICLE_SIZE_INVALID","发射器包含式长度无效。");auto v=r.Take(n-4);
        Emitter e;e.kind=kind;e.sourceIndex=sourceCounts[static_cast<std::size_t>(kind)]++;
        // 已安全取完整子块才允许局部内容隔离；继续消费已知结构，不退还key预算。
        std::optional<Error> localError;
        if(mode==ReadMode::PreviewSkipInvalidEmitters)v.DeferLocalErrors(localError);
        Node(v,e);
        if(kind==Kind::Particle2) {
            e.speed=v.Float();e.variation=v.Float();e.latitude=v.Float();e.gravity=v.Float();e.life=v.Float();
            e.rate=v.Float();e.length=v.Float();e.width=v.Float();const auto filter=v.U32();
            v.LocalRequire(filter<=4,"PARTICLE_FILTER_UNSUPPORTED","未知PRE2过滤模式。");e.filter=static_cast<Filter>(filter);
            e.rows=v.U32();e.columns=v.U32();e.headOrTail=v.U32();e.tailLength=v.Float();e.middle=v.Float();
            // 原版 PRE2 静态颜色是 RGB；见 docs/MDX_PARTICLE_NATIVE_2026-09-29.md，不套用 GEOA 顺序。
            for(auto& c:e.colors)c=v.Vector();
            for(auto& a:e.alphas)a=static_cast<float>(v.Byte())/255.0f;
            for(auto& s:e.scales)s=v.Float();
            for(auto& i:e.intervals){i.first=v.U32();i.last=v.U32();i.repeat=v.U32();}
            e.textureId=v.U32();e.squirt=v.U32();e.priority=std::bit_cast<std::int32_t>(v.U32());e.replaceableId=v.U32();
            // 分段时间可大于1：粒子到不了中点，整个寿命按 life/middle 走前半段（见 ParticleGeometry）。
            v.LocalRequire(e.headOrTail<=2&&e.squirt<=1&&std::isfinite(e.middle)&&e.middle>=0&&e.tailLength>=0,
                "PARTICLE_PARAMETER_UNSUPPORTED","PRE2头尾、squirt或分段时间无效。");
        } else {
            e.above=v.Float();e.below=v.Float();e.alpha=v.Float();e.ribbonColor=v.Vector();
            e.life=v.Float();e.textureSlot=v.U32();const auto rate=v.U32();
            Require(rate<=1000000,"PARTICLE_RATE_BUDGET","条带发射率超限。");e.rate=static_cast<float>(rate);
            e.rows=v.U32();e.columns=v.U32();e.materialId=v.U32();e.gravity=v.Float();
        }
        Require(e.rows<=256&&e.columns<=256,"PARTICLE_ATLAS_INVALID","粒子图集行列无效。");
        v.LocalRequire(e.rows&&e.columns,"PARTICLE_ATLAS_INVALID","粒子图集行列无效。");
        // 非有限字段已有局部诊断；有限字段的资源预算仍不能降级成单发射器跳过。
        Require((!std::isfinite(e.life)||(e.life>=0&&e.life<=600))&&
            (!std::isfinite(e.rate)||(e.rate>=0&&e.rate<=1000000))&&
            (!std::isfinite(e.variation)||e.variation>=0),
            "PARTICLE_PARAMETER_BUDGET","发射率、寿命或变化超出预览预算。");
        if(kind==Kind::Particle2) {
            const auto cells=e.rows*e.columns;
            for(auto& i:e.intervals) {
                // 1.27a 四区间任一端点 unsigned >N 时整对传入(0,0)，不是独立归零。
                // 依据 6F13E64E/672/699/6BD 的 push0,push0；==N 合法，repeat 原样保留。
                if(i.first>cells||i.last>cells) {
                    i.first=0;
                    i.last=0;
                }
                // Cell 使用非负跨度；归零后仍倒序的原版取格语义尚未实现，继续拒绝。
                v.LocalRequire(i.first<=i.last,"PARTICLE_ATLAS_RANGE","粒子图集端点归零后区间倒序，当前预览不支持负跨度。");
            }
        }
        std::set<Parameter> seen;
        std::vector<RecoveredVisibilityTrack> recoveries;
        std::vector<GameOrderTrack> orders;
        while(v.Remaining()) {
            const auto sourceByteOffset=v.Offset();
            const auto p=Param(v.Tag(),kind);v.LocalRequire(seen.insert(p).second,"PARTICLE_TRACK_DUPLICATE","重复粒子轨迹。");
            auto& track=e.tracks[static_cast<std::size_t>(p)];
            RecoveredVisibilityTrack recovery{kind,e.sourceIndex,e.nodeId,0,sourceByteOffset,{}};
            const bool recover=mode==ReadMode::PreviewSkipInvalidEmitters && kind==Kind::Particle2 &&
                p==Parameter::Visibility && e.squirt==0;
            std::optional<std::array<std::int32_t,2>> gameOrder;
            track=ReadTrack(v,p==Parameter::Color?3u:1u,p==Parameter::TextureSlot,keys,limits,recover?&recovery:nullptr,
                mode==ReadMode::PreviewSkipInvalidEmitters?&gameOrder:nullptr);
            // 同一轨迹既有同帧重复又有逆序边时，按原版区间取值已覆盖重复键，只记录逆序来源。
            if(gameOrder)orders.push_back({kind,p,e.sourceIndex,e.nodeId,sourceByteOffset,(*gameOrder)[0],(*gameOrder)[1]});
            else if(!recovery.duplicateKeys.empty())recoveries.push_back(std::move(recovery));
            // RIBB 静态字段是 RGB，KRCO 却是 BGR；统一入口转换键值和两条切线，采样器只处理 RGB。
            if(p==Parameter::Color)for(auto& k:track.keys){
                std::swap(k.value[0],k.value[2]);std::swap(k.in[0],k.in[2]);std::swap(k.out[0],k.out[2]);
            }
        }
        if(localError)out.skippedEmitters.push_back({kind,e.sourceIndex,e.nodeId,localError->code,localError->what()});
        else {
            out.emitters.push_back(std::move(e));
            for(auto& recovery:recoveries)out.recoveredVisibilityTracks.push_back(std::move(recovery));
            out.gameOrderTracks.insert(out.gameOrderTracks.end(),orders.begin(),orders.end());
        }
    }
}
}
Result<Asset> Read(std::span<const std::byte> bytes,ReadLimits limits,ReadMode mode) try {
    Require(mode==ReadMode::Strict||mode==ReadMode::PreviewSkipInvalidEmitters,
        "PARTICLE_READ_MODE_INVALID","未知粒子只读预览模式。");
    Require(limits.maxBytes<=32ull*1024*1024&&limits.maxEmitters<=256&&limits.maxKeys<=65536&&bytes.size()<=limits.maxBytes,
        "PARTICLE_READ_BUDGET","粒子读取超限，不能放宽硬上限。");
    Reader file(bytes);Require(file.Tag()=="MDLX","PARTICLE_MAGIC","缺少MDLX文件头。");
    Asset out;std::uint32_t keys=0;std::array<std::uint32_t,2> sourceCounts{};bool version=false;
    while(file.Remaining()) {
        const auto tag=file.Tag();auto r=file.Take(file.U32());
        if(tag=="VERS"){Require(r.U32()==800,"PARTICLE_VERSION_UNSUPPORTED","粒子仅支持经典MDX800。");version=true;}
        else if(tag=="PRE2")ReadEmitters(r,Kind::Particle2,out,keys,sourceCounts,limits,mode);
        else if(tag=="RIBB")ReadEmitters(r,Kind::Ribbon,out,keys,sourceCounts,limits,mode);
        else if(tag=="PREM")while(r.Remaining()) {
            const auto n=r.U32();Require(n>=100&&out.premCount<limits.maxEmitters,"PARTICLE_PREM_SIZE","PREM大小或数量无效。");
            r.Skip(n-4);++out.premCount;
        }
        else if(tag=="SEQS") {
            // 132 字节条目：名称80、起点、终点、其余字段不参与取键。
            Require(out.sequences.empty()&&r.Remaining()%132==0&&r.Remaining()/132<=65536,"PARTICLE_SEQUENCE_BUDGET","SEQS大小或数量无效。");
            while(r.Remaining()){r.Skip(80);SequenceSpan s;s.start=r.U32();s.end=r.U32();r.Skip(44);out.sequences.push_back(s);}
        }
        else if(tag=="GLBS") {Require(r.Remaining()%4==0&&r.Remaining()/4<=4096-out.globalSequences.size(),"PARTICLE_GLOBAL_BUDGET","GLBS大小或数量无效。");while(r.Remaining())out.globalSequences.push_back(r.U32());}
    }
    Require(version,"PARTICLE_VERSION_UNSUPPORTED","缺少VERS。");
    // GLBS/SEQS 可在发射器块之后；仍须解析完全部外层结构，再按同一源身份隔离坏引用。
    const auto retained=std::remove_if(out.emitters.begin(),out.emitters.end(),[&](const Emitter& e) {
        const bool invalid=std::any_of(e.tracks.begin(),e.tracks.end(),[&](const Track& t) {
            return t.globalSequence!=UINT32_MAX&&t.globalSequence>=out.globalSequences.size();
        });
        // 逆序键只能按 SEQS 的原版区间解释；没有 SEQS 时不猜区间。
        const bool unordered=!invalid&&out.sequences.empty()&&std::any_of(out.gameOrderTracks.begin(),out.gameOrderTracks.end(),
            [&](const GameOrderTrack& order){return order.kind==e.kind&&order.sourceIndex==e.sourceIndex;});
        if(!invalid&&!unordered)return false;
        const char* code=invalid?"PARTICLE_GLOBAL_INVALID":"PARTICLE_KEYS_INVALID";
        const char* message=invalid?"粒子轨迹全局序列无效。":"粒子按键含逆序帧且模型没有 SEQS，无法按原版动作区间取键。";
        Require(mode==ReadMode::PreviewSkipInvalidEmitters,code,message);
        out.skippedEmitters.push_back({e.kind,e.sourceIndex,e.nodeId,code,message});
        std::erase_if(out.recoveredVisibilityTracks,[&](const RecoveredVisibilityTrack& recovery) {
            return recovery.kind==e.kind && recovery.sourceIndex==e.sourceIndex;
        });
        std::erase_if(out.gameOrderTracks,[&](const GameOrderTrack& order) {
            return order.kind==e.kind && order.sourceIndex==e.sourceIndex;
        });
        return true;
    });
    out.emitters.erase(retained,out.emitters.end());
    if(!out.sequences.empty())for(auto& e:out.emitters)for(auto& t:e.tracks) {
        if(t.keys.empty()||t.globalSequence!=UINT32_MAX)continue;
        t.ranges.reserve(out.sequences.size());
        War3RenderCore::ForEachGameSequenceRange(t.keys,out.sequences,[&](std::size_t,std::size_t first,std::size_t count) {
            std::int64_t left=0,right=0;
            const bool descending=War3RenderCore::FirstDescendingEdge(t.keys,first,count,left,right);
            t.ranges.push_back({static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(count),!descending});
            return true;
        });
    }
    auto result=Result<Asset>::Success(std::move(out));
    if(result.value->premCount)result.diagnostics.push_back({DiagnosticSeverity::Warning,"Render.Particles","PARTICLE_PREM_UNSUPPORTED",
        "PREM旧式模型发射器未实现，数量="+std::to_string(result.value->premCount),std::nullopt});
    for(const auto& skipped:result.value->skippedEmitters)result.diagnostics.push_back({DiagnosticSeverity::Warning,
        "Render.Particles",skipped.code,(skipped.kind==Kind::Particle2?"PRE2":"RIBB")+
            std::string("源记录")+std::to_string(skipped.sourceIndex)+" 已隔离："+skipped.message,std::nullopt});
    for(const auto& recovery:result.value->recoveredVisibilityTracks)result.diagnostics.push_back({DiagnosticSeverity::Warning,
        "Render.Particles","PARTICLE_VISIBILITY_DUPLICATE_PREVIEW","PRE2源记录"+std::to_string(recovery.sourceIndex)+
            " 的KP2V按同帧逐位同值作预览恢复；保留"+std::to_string(recovery.sourceKeyCount)+"个原键、"+
            std::to_string(recovery.duplicateKeys.size())+"条重复边，尚无原版兼容证明。",
        OriginSpan{"",recovery.sourceByteOffset,4,{}}});
    for(const auto& order:result.value->gameOrderTracks)result.diagnostics.push_back({DiagnosticSeverity::Warning,
        "Render.Particles","PARTICLE_KEYS_GAME_ORDER_PREVIEW",(order.kind==Kind::Particle2?"PRE2":"RIBB")+
            std::string("源记录")+std::to_string(order.sourceIndex)+" 的"+TagName(order.kind,order.parameter)+" 在源帧 "+
            std::to_string(order.left)+"→"+std::to_string(order.right)+" 含逆序键；按 1.27a 原版逐动作区间与顺序查找取值。",
        OriginSpan{"",order.sourceByteOffset,4,{}}});
    return result;
} catch(const Error& e){return Result<Asset>::Failure("Render.Particles",e.code,e.what());}
catch(const std::bad_alloc&){return Result<Asset>::Failure("Render.Particles","PARTICLE_ALLOCATION","粒子资源分配失败。");}
} // namespace War3Map::Render::Particles
