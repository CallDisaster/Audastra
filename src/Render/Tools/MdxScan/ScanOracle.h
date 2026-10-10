#pragma once
#include "ScanTypes.h"
#include "War3RenderCore/PoseBatch.h"
namespace War3Map::Render::Scan {
struct Sample {
    std::uint32_t sequence = 0;
    std::uint64_t elapsed = 0, global = 0;
    bool camera = false;
};
std::vector<Sample> Samples(const MdxModel& model, Record& record, const Limits& limits, const Checkpoint& checkpoint);
void CompareSample(const MdxModel& model, const War3RenderCore::CompiledModel& asset,
    War3RenderCore::PoseBatchEvaluator& evaluator, const Sample& sample, Record& record, const Checkpoint& checkpoint);
}
