#pragma once
#include "Particles/ParticleTypes.h"
#include "MdxTestModel.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

namespace ParticleVisibilityPreviewTest {
using namespace War3Map;
using namespace War3Map::Render::Particles;
inline void Check(bool valid,const char* message) { if(!valid)throw std::runtime_error(message); }
template<class T> inline T Good(Result<T> result) {
    if(!result.Succeeded())throw std::runtime_error(result.diagnostics.empty()?"missing visibility result":
        result.diagnostics.front().code+": "+result.diagnostics.front().message);
    return std::move(*result.value);
}
struct SourceKey { std::int32_t frame; std::uint32_t bits; };
inline SourceKey KeyAt(std::int32_t frame,float value) { return {frame,std::bit_cast<std::uint32_t>(value)}; }
struct Options {
    std::string kind="PRE2",tag="KP2V";
    std::uint32_t interpolation=0,globalSequence=UINT32_MAX,squirt=0,filter=1;
    bool badTrailingTrack=false,invalidTrailingGlobal=false;
    std::vector<std::array<std::uint32_t,2>> sequences; // 非空时写 SEQS（文件顺序即动作序号）。
    std::vector<SourceKey> keys{KeyAt(0,1),KeyAt(90,0),KeyAt(90,0),KeyAt(120,.5f),KeyAt(120,.5f),
        KeyAt(210,1),KeyAt(280,0),KeyAt(280,0),KeyAt(500,1)};
};
struct BinaryFixture {
    std::vector<std::byte> bytes;
    std::size_t trackOffset=0;
    std::vector<std::size_t> frameOffsets;
};
inline BinaryFixture Binary(const Options& options={}) {
    MdxTest::Writer writer; BinaryFixture fixture;
    writer.Tag("MDLX");writer.Tag("VERS");writer.U32(4);writer.U32(800);
    if(!options.sequences.empty()) {
        writer.Tag("SEQS");writer.U32(static_cast<std::uint32_t>(options.sequences.size()*132));
        for(const auto& sequence:options.sequences) {
            writer.Fixed("Original sequence",80);writer.U32(sequence[0]);writer.U32(sequence[1]);
            writer.F32(0);writer.U32(0);writer.F32(0);writer.U32(0);for(unsigned i=0;i<7;++i)writer.F32(0);
        }
    }
    if(options.globalSequence!=UINT32_MAX) { writer.Tag("GLBS");writer.U32(4);writer.U32(100000); }
    for(unsigned record=0;record<2;++record) {
        const bool ribbon=record==0 && options.kind=="RIBB";
        writer.Tag(record==0?options.kind.c_str():"PRE2");
        const auto chunk=writer.Begin(),object=writer.Begin(),node=writer.Begin();
        writer.Fixed("Original visibility fixture",80);writer.U32(record);writer.U32(UINT32_MAX);
        writer.U32(ribbon?0x4000u:0x1000u);writer.EndInclusive(node);
        if(!ribbon) {
            for(const float value:{10.f,.2f,20.f,1.f,2.f,20.f,5.f,5.f})writer.F32(value);
            writer.U32(record==0?options.filter:1u);writer.U32(1);writer.U32(1);writer.U32(0);writer.F32(0);writer.F32(.5f);
            for(unsigned i=0;i<3;++i) { writer.F32(.2f);writer.F32(.4f);writer.F32(.8f); }
            const unsigned char alpha[]{255,128,0};writer.Raw(alpha,3);
            for(unsigned i=0;i<3;++i)writer.F32(2);
            for(unsigned i=0;i<4;++i) { writer.U32(0);writer.U32(0);writer.U32(1); }
            writer.U32(0);writer.U32(record==0?options.squirt:0u);writer.U32(0);writer.U32(0);
        } else {
            for(const float value:{3.f,2.f,.5f,.2f,.4f,.8f,2.f})writer.F32(value);
            writer.U32(0);writer.U32(20);writer.U32(1);writer.U32(1);writer.U32(0);writer.F32(1);
        }
        if(record==0) {
            fixture.trackOffset=writer.bytes.size();writer.Tag(options.tag.c_str());
            writer.U32(static_cast<std::uint32_t>(options.keys.size()));writer.U32(options.interpolation);writer.U32(options.globalSequence);
            for(const auto& key:options.keys) {
                fixture.frameOffsets.push_back(writer.bytes.size());writer.U32(std::bit_cast<std::uint32_t>(key.frame));writer.U32(key.bits);
                if(options.interpolation>1) { writer.U32(key.bits);writer.U32(key.bits); }
            }
            if(options.badTrailingTrack || options.invalidTrailingGlobal) {
                writer.Tag("KP2E");writer.U32(2);writer.U32(0);writer.U32(options.invalidTrailingGlobal?1u:UINT32_MAX);
                writer.U32(10);writer.F32(20);writer.U32(options.badTrailingTrack?5u:20u);writer.F32(20);
            }
        }
        writer.EndInclusive(object);writer.EndChunk(chunk);
    }
    fixture.bytes=std::move(writer.bytes);return fixture;
}
inline Options Quotient(Options value) {
    value.keys.erase(std::unique(value.keys.begin(),value.keys.end(),[](const auto& left,const auto& right) {
        Check(left.frame!=right.frame || left.bits==right.bits,"oracle cannot erase a differing duplicate");
        return left.frame==right.frame;
    }),value.keys.end());return value;
}
// 1.27a 逐动作区间：SEQS 文件顺序共享只进游标；区间内顺序查找，首键前接回末键（与节点轨迹共用核心实现）。
inline void GameOrderRanges() {
    Options options;options.sequences={{0,1000},{2000,3000},{100,400}};
    options.keys={KeyAt(0,1),KeyAt(500,0),KeyAt(2200,1),KeyAt(2800,0),KeyAt(2500,.5f)};
    const auto fixture=Binary(options);
    const auto strict=Read(fixture.bytes);
    Check(!strict.Succeeded() && strict.diagnostics.front().code=="PARTICLE_KEYS_INVALID","strict accepted descending KP2V");
    const auto read=Read(fixture.bytes,{},ReadMode::PreviewSkipInvalidEmitters);
    Check(read.Succeeded() && read.value->emitters.size()==2 && read.value->skippedEmitters.empty() &&
        read.value->gameOrderTracks.size()==1 && read.value->recoveredVisibilityTracks.empty(),"preview did not accept game-order keys");
    const auto& order=read.value->gameOrderTracks.front();
    Check(order.left==2800 && order.right==2500 && order.sourceByteOffset==fixture.trackOffset && order.parameter==Parameter::Visibility &&
        read.diagnostics.size()==1 && read.diagnostics.front().code=="PARTICLE_KEYS_GAME_ORDER_PREVIEW" &&
        read.diagnostics.front().origin && read.diagnostics.front().origin->offset==fixture.trackOffset,"game-order source location lost");
    const auto& asset=*read.value;
    const auto& track=asset.emitters[0].tracks[static_cast<std::size_t>(Parameter::Visibility)];
    Check(track.keys.size()==5 && track.keys[4].frame==2500,"source keys were sorted or removed");
    Check(track.ranges.size()==3 && track.ranges[0].first==0 && track.ranges[0].count==2 && track.ranges[0].ascending &&
        track.ranges[1].first==2 && track.ranges[1].count==3 && !track.ranges[1].ascending && track.ranges[2].count==0,
        "per-sequence game ranges differ from the shared forward cursor");
    const Vec3 fallback{.75f,0,0};
    const auto at=[&](std::uint32_t sequence,std::uint64_t ms) {
        Timeline clock;clock.start=options.sequences[sequence][0];clock.end=options.sequences[sequence][1];clock.sequenceIndex=sequence;
        return Sample(track,fallback,asset,clock,ms)[0];
    };
    Check(at(0,0)==1 && at(0,700)==0,"sorted range lookup changed");
    // 首键前按动作终点重查到末键（.5）；2800 时扫描越过 2800 与逆序键 2500，取 .5。
    Check(at(1,100)==.5f && at(1,300)==1 && at(1,600)==1 && at(1,800)==.5f && at(1,900)==.5f,"descending range ordered lookup");
    // 排在后面的早期动作：游标已越过，区间为空，取默认值。
    Check(at(2,50)==.75f,"empty game range must use the default");
    auto global=options;global.globalSequence=0;
    const auto rejected=Read(Binary(global).bytes,{},ReadMode::PreviewSkipInvalidEmitters);
    Check(rejected.Succeeded() && rejected.value->skippedEmitters.size()==1 &&
        rejected.value->skippedEmitters[0].code=="PARTICLE_KEYS_INVALID" && rejected.value->gameOrderTracks.empty(),
        "global descending keys must stay rejected");
    std::cout<<"game_order_ranges=1 shared_cursor=1 ordered_lookup=1 empty_range_default=1 global_still_rejected=1\n";
}
inline void ReadContracts() {
    const Options options;const auto fixture=Binary(options);const auto original=fixture.bytes;
    const auto strict=Read(fixture.bytes);
    Check(!strict.Succeeded() && strict.diagnostics.front().code=="PARTICLE_KEYS_INVALID","strict accepted duplicate KP2V frames");
    const auto result=Read(fixture.bytes,{},ReadMode::PreviewSkipInvalidEmitters);
    Check(result.Succeeded() && result.value->emitters.size()==2 && result.value->skippedEmitters.empty() &&
        result.value->recoveredVisibilityTracks.size()==1 && result.diagnostics.size()==1 && fixture.bytes==original,
        "preview recovery changed input, skipped a recovered emitter, or produced per-key diagnostics");
    const auto& asset=*result.value;const auto& recovery=asset.recoveredVisibilityTracks.front();
    const auto& keys=asset.emitters[0].tracks[static_cast<std::size_t>(Parameter::Visibility)].keys;
    Check(keys.size()==options.keys.size() && recovery.kind==Kind::Particle2 && recovery.sourceIndex==0u && recovery.nodeId==0u &&
        recovery.sourceKeyCount==options.keys.size() && recovery.sourceByteOffset==fixture.trackOffset && recovery.duplicateKeys.size()==3 &&
        result.diagnostics.front().code=="PARTICLE_VISIBILITY_DUPLICATE_PREVIEW" && result.diagnostics.front().origin &&
        result.diagnostics.front().origin->offset==fixture.trackOffset,
        "recovery lost source identity, original key count or absolute KP2V origin");
    for(std::size_t i=0;i<keys.size();++i)Check(keys[i].frame==options.keys[i].frame &&
        std::bit_cast<std::uint32_t>(keys[i].value[0])==options.keys[i].bits,"recovery sorted, removed or rewrote original keys");
    for(const auto& edge:recovery.duplicateKeys)Check(edge.keyIndex<keys.size() && edge.keyIndex>0 &&
        edge.frameByteOffset==fixture.frameOffsets[edge.keyIndex] && edge.frame==options.keys[edge.keyIndex].frame &&
        edge.valueBits==options.keys[edge.keyIndex].bits && options.keys[edge.keyIndex-1].frame==edge.frame,
        "recovered edge has no exact original frame/value location");
    const auto rejectLocal=[](const Options& rejected,std::string_view code="PARTICLE_KEYS_INVALID") {
        const auto parsed=Read(Binary(rejected).bytes,{},ReadMode::PreviewSkipInvalidEmitters);
        Check(parsed.Succeeded() && parsed.value->emitters.size()==1 && parsed.value->emitters[0].nodeId==1u &&
            parsed.value->skippedEmitters.size()==1 && parsed.value->skippedEmitters[0].nodeId==0u &&
            parsed.value->skippedEmitters[0].code==code && parsed.value->recoveredVisibilityTracks.empty(),
            "ineligible duplicate/other emitter error was hidden by visibility recovery");
    };
    auto bad=options;bad.keys[2].bits=std::bit_cast<std::uint32_t>(.25f);rejectLocal(bad);
    bad=options;bad.keys[2].bits=0x80000000u;rejectLocal(bad); // +0与-0不能按float ==合并。
    bad=options;bad.keys[2].frame=89;rejectLocal(bad);
    for(const std::uint32_t bits:{0x7fc00001u,0x7f800000u}) { bad=options;bad.keys[1].bits=bad.keys[2].bits=bits;rejectLocal(bad,"PARTICLE_NUMBER_INVALID"); }
    for(const std::uint32_t interpolation:{1u,2u,3u}) { bad=options;bad.interpolation=interpolation;rejectLocal(bad); }
    bad=options;bad.globalSequence=0;rejectLocal(bad);
    bad=options;bad.squirt=1;rejectLocal(bad);
    bad=options;bad.tag="KP2E";rejectLocal(bad);
    bad=options;bad.kind="RIBB";bad.tag="KRVS";rejectLocal(bad);
    bad=options;bad.filter=99;rejectLocal(bad,"PARTICLE_FILTER_UNSUPPORTED");
    bad=options;bad.badTrailingTrack=true;rejectLocal(bad);
    bad=options;bad.invalidTrailingGlobal=true;rejectLocal(bad,"PARTICLE_GLOBAL_INVALID");
    bad=options;bad.kind="PREM";
    const auto prem=Good(Read(Binary(bad).bytes,{},ReadMode::PreviewSkipInvalidEmitters));
    Check(prem.premCount==1 && prem.emitters.size()==1 && prem.recoveredVisibilityTracks.empty(),"PREM was silently restored as PRE2");
    ReadLimits limits;limits.maxKeys=static_cast<std::uint32_t>(options.keys.size()-1);
    const auto budget=Read(fixture.bytes,limits,ReadMode::PreviewSkipInvalidEmitters);
    Check(!budget.Succeeded() && budget.diagnostics.front().code=="PARTICLE_KEY_BUDGET" &&
        Read(Binary(Quotient(options)).bytes,limits).Succeeded(),"preview refunded original duplicate-key budget");
    auto truncated=fixture.bytes;truncated.pop_back();
    const auto broken=Read(truncated,{},ReadMode::PreviewSkipInvalidEmitters);
    Check(!broken.Succeeded() && broken.diagnostics.front().code=="PARTICLE_TRUNCATED","recovery swallowed a later structural truncation");
    std::cout<<"visibility_same_frame_preview=1 strict_still_rejects=1 original_keys_and_offsets=1 scoped_rejections=1 budgets_preserved=1\n";
    GameOrderRanges();
}
template<std::size_t N> inline bool SameBits(const std::array<float,N>& left,const std::array<float,N>& right) {
    for(std::size_t i=0;i<N;++i)if(std::bit_cast<std::uint32_t>(left[i])!=std::bit_cast<std::uint32_t>(right[i]))return false;
    return true;
}
inline void SameState(const State& left,const State& right) {
    Check(left.tickMs==right.tickMs && left.timeMs==right.timeMs && left.instance==right.instance && left.generation==right.generation &&
        left.sequenceIndex==right.sequenceIndex && left.globalStartMs==right.globalStartMs &&
        left.boundedWorldPreview==right.boundedWorldPreview && left.emitters.size()==right.emitters.size(),"visibility oracle state identity differs");
    for(std::size_t i=0;i<left.emitters.size();++i) {
        const auto& a=left.emitters[i];const auto& b=right.emitters[i];
        Check(a.random==b.random && a.spawned==b.spawned && a.dropped==b.dropped && a.quota==b.quota && a.lastBurst==b.lastBurst &&
            std::bit_cast<std::uint64_t>(a.accumulator)==std::bit_cast<std::uint64_t>(b.accumulator) &&
            a.particles.size()==b.particles.size() && a.segments.empty() && b.segments.empty(),"visibility changed RNG, quotas, emission remainder or live state");
        for(std::size_t p=0;p<a.particles.size();++p) {
            const auto& x=a.particles[p];const auto& y=b.particles[p];
            Check(SameBits(x.position,y.position) && SameBits(x.velocity,y.velocity) &&
                SameBits(std::array{x.age,x.life,x.gravity},std::array{y.age,y.life,y.gravity}),"visibility partition changed particle bits");
        }
    }
}
inline void SameGeometry(const Asset& leftAsset,const State& left,const Asset& rightAsset,const State& right,const Timeline& timeline) {
    const auto a=Good(Build(leftAsset,left,timeline,{0,-100,100},{0,0,1}));
    const auto b=Good(Build(rightAsset,right,timeline,{0,-100,100},{0,0,1}));
    Check(a.vertices.size()==b.vertices.size() && a.draws.size()==b.draws.size(),"visibility oracle geometry counts differ");
    for(std::size_t i=0;i<a.vertices.size();++i)Check(SameBits(a.vertices[i].position,b.vertices[i].position) &&
        SameBits(a.vertices[i].uv,b.vertices[i].uv) && SameBits(a.vertices[i].color,b.vertices[i].color),"visibility oracle geometry bits differ");
    for(std::size_t i=0;i<a.draws.size();++i)Check(a.draws[i].first==b.draws[i].first && a.draws[i].count==b.draws[i].count &&
        a.draws[i].emitter==b.draws[i].emitter && a.draws[i].filter==b.draws[i].filter,"visibility oracle draw routing differs");
}
inline void SamplesAndSimulation() {
    Options wide;wide.keys={KeyAt(0,1)};
    const std::array<std::int32_t,6> frames{29200,32833,36467,82333,85967,92933};
    const std::array<float,6> values{0,.5f,1,-0.f,.25f,0};
    for(std::size_t i=0;i<frames.size();++i) { wide.keys.push_back(KeyAt(frames[i],values[i]));wide.keys.push_back(wide.keys.back()); }
    wide.keys.push_back(KeyAt(96000,1));
    const auto recovered=Good(Read(Binary(wide).bytes,{},ReadMode::PreviewSkipInvalidEmitters));
    const auto oracle=Good(Read(Binary(Quotient(wide)).bytes));
    const auto& sourceTrack=recovered.emitters[0].tracks[static_cast<std::size_t>(Parameter::Visibility)];
    const auto& oracleTrack=oracle.emitters[0].tracks[static_cast<std::size_t>(Parameter::Visibility)];
    std::size_t comparisons=0;
    for(const auto range:std::array<std::array<std::uint32_t,2>,7>{{{0,100000},{29200,36467},{40000,45000},
        {29200,29200},{82333,92933},{90000,100000},{100000,100500}}})for(const bool loop:{false,true}) {
        Timeline clock;clock.start=range[0];clock.end=range[1];clock.loop=loop;
        const std::uint64_t length=clock.end-clock.start;
        std::set<std::uint64_t> times{0,1,length,length+1,length*2+3};
        for(const auto& key:wide.keys)if(key.frame>=static_cast<std::int32_t>(clock.start)) {
            const auto at=static_cast<std::uint64_t>(key.frame)-clock.start;
            if(at)times.insert(at-1);
            times.insert(at);times.insert(at+1);
        }
        for(const auto at:times) {
            const Vec3 fallback{-0.f,.125f,-.25f};
            const auto first=Sample(sourceTrack,fallback,recovered,clock,at);
            Check(SameBits(first,Sample(oracleTrack,fallback,oracle,clock,at)) &&
                SameBits(first,Sample(sourceTrack,fallback,recovered,clock,at)),"duplicate/quotient sampling or paused repeat differs by float bits");
            ++comparisons;
        }
    }
    const Options compact;const auto asset=Good(Read(Binary(compact).bytes,{},ReadMode::PreviewSkipInvalidEmitters));
    const auto compactOracle=Good(Read(Binary(Quotient(compact)).bytes));
    const std::array<std::uint32_t,2> quotas{7,5};
    const auto anchors=[](std::uint64_t ms,std::span<Anchor> out) {
        for(std::size_t i=0;i<out.size();++i) { out[i]=Anchor{};out[i].model[3]=static_cast<float>(ms)*.1f+static_cast<float>(i); }
        return Status::Ok();
    };
    for(const bool loop:{false,true}) {
        Timeline clock;clock.end=500;clock.loop=loop;
        auto chunked=Good(Create(asset,17,3,0,quotas)),expected=Good(Create(compactOracle,17,3,0,quotas));
        for(const std::uint64_t at:{1ull,9ull,10ull,89ull,90ull,91ull,120ull,211ull,281ull,499ull,500ull,501ull,777ull,1500ull,2317ull}) {
            Check(Advance(asset,chunked,clock,at,anchors).Succeeded() && Advance(compactOracle,expected,clock,at,anchors).Succeeded(),"visibility partition advance failed");
            SameState(chunked,expected);SameGeometry(asset,chunked,compactOracle,expected,clock);
        }
        auto oneShot=Good(Create(asset,17,3,0,quotas));
        Check(Advance(asset,oneShot,clock,2317,anchors).Succeeded(),"visibility one-shot advance failed");
        SameState(chunked,oneShot);SameGeometry(asset,chunked,asset,oneShot,clock);
        const auto paused=chunked;
        Check(Advance(asset,chunked,clock,2317,anchors).Succeeded(),"visibility pause failed");SameState(paused,chunked);
        Check(Count(chunked).suppressed>0 && Count(chunked).spawned>0,"visibility fixture did not exercise actual quotas and emission");
    }
    std::cout<<"visibility_quotient_bitwise_samples="<<comparisons<<" boundary_loop_pause=1 full_rng_remainder_state=1 partition_geometry=1 CPU_only=1\n";
}
} // namespace ParticleVisibilityPreviewTest
