#include "ScanOracle.h"
#include <algorithm>
#include <set>
namespace War3Map::Render::Scan {
namespace {
bool IsKey(const MdxModel& model, std::uint64_t frame, std::uint64_t global) {
    const auto has = [&](const auto& track) {
        if (!track) return false;
        const auto index = track->globalSequence;
        const auto time = index != UINT32_MAX && index < model.globalSequences.size() && model.globalSequences[index]
            ? global % model.globalSequences[index] : frame;
        return std::any_of(track->keys.begin(),track->keys.end(),[&](const auto& k) { return k.frame >= 0 && static_cast<std::uint64_t>(k.frame) == time; });
    };
    const auto node = [&](const auto& n) { return has(n.translation) || has(n.rotation) || has(n.scaling); };
    if (std::any_of(model.bones.begin(),model.bones.end(),node) || std::any_of(model.helpers.begin(),model.helpers.end(),node)
        || std::any_of(model.otherNodes.begin(),model.otherNodes.end(),node)) return true;
    for (const auto& g : model.geosets) if (g.animation) {
        const auto time = [&](std::uint32_t sequence) {
            return sequence != UINT32_MAX && sequence < model.globalSequences.size() && model.globalSequences[sequence]
                ? global % model.globalSequences[sequence] : frame;
        };
        for (const auto& key : g.animation->alphaKeys) if (key.frame == time(g.animation->globalSequence)) return true;
        for (const auto& key : g.animation->colorKeys) if (key.frame == time(g.animation->colorGlobalSequence)) return true;
    }
    return false;
}
}
std::vector<Sample> Samples(const MdxModel& model, Record& record, const Limits& limits, const Checkpoint& checkpoint) {
    std::vector<Sample> out;
    for (std::uint32_t s = 0; s < model.sequences.size(); ++s) {
        checkpoint();
        const auto& sequence = model.sequences[s];
        const auto duration = sequence.end >= sequence.start ? static_cast<std::uint64_t>(sequence.end) - sequence.start : 0;
        std::set<std::uint64_t> times{0,duration/2};
        bool nonKey = false;
        // 最多尝试 1024 个整数毫秒；没有找到就记录缺口，不能把按键帧冒充内插帧。
        for (std::uint64_t i = 1; i < duration && i <= 1024; ++i) {
            checkpoint();
            if (!IsKey(model,sequence.start+i,i+17)) { times.insert(i); nonKey = true; break; }
        }
        if (!nonKey) ++record.nonKeyUnavailable;
        for (const auto time : times) for (const auto camera : {false,true}) {
            if (out.size() >= limits.maxSamples) { record.state = "SCAN_SAMPLE_LIMIT"; return out; }
            out.push_back({s,time,time+17,camera});
        }
    }
    return out;
}
}
