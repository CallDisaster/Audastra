#include "War3MapCore/Render/PremPreview.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <utility>

namespace War3Map::Render::Prem {
namespace {
constexpr std::uint32_t kMaximumKeys = 4096;
constexpr std::uint64_t kMaximumClock = UINT32_MAX;
struct Interval { std::uint64_t begin = 0, end = 0, prefix = 0; };
struct Schedule {
    std::vector<Interval> intervals;
    std::uint64_t duration = 0, active = 0;
    bool looping = false;
};

template<class T> Result<T> Failure(const char* code, const char* message) {
    return Result<T>::Failure("PremPreview", code, message);
}

bool SourceValid(const War3RenderCore::AssetSource& source) {
    return !source.virtualPath.empty() && source.virtualPath.size() <= 1024 &&
        !source.sourceLayer.empty() && source.sourceLayer.size() <= 256 &&
        source.contentDigest.size() == 64 && std::all_of(source.contentDigest.begin(), source.contentDigest.end(),
            [](unsigned char value) { return (value >= '0' && value <= '9') ||
                (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); });
}

bool LimitsValid(const Limits& limits, std::size_t requests) {
    if (!limits.maxRequests || limits.maxRequests > 4096 || requests > limits.maxRequests ||
        !limits.maxLiveChildren || limits.maxLiveChildren > 4096 ||
        !limits.maxBirths || limits.maxBirths > 16384 ||
        !limits.maxAnchorVisits || limits.maxAnchorVisits > 16384 ||
        !limits.maxVisibilityKeyVisits || limits.maxVisibilityKeyVisits > 1048576 ||
        !limits.maxFrameBytes || limits.maxFrameBytes > 1024 * 1024) return false;
    // 已有整数硬帽使乘加可证不溢出；计入 frame 两容器和最大一个 request 的区间暂存。
    const auto bytes = sizeof(Frame) + std::uint64_t(limits.maxLiveChildren) * sizeof(Child) +
        std::uint64_t(limits.maxRequests) * sizeof(Issue) + (kMaximumKeys + 1ull) * sizeof(Interval);
    return bytes <= limits.maxFrameBytes;
}

const char* MakeSchedule(const Request& request, const Limits& limits, Frame& frame, Schedule& schedule) {
    const auto& model = request.emitter->Model();
    if (request.sequenceIndex >= model.sequences.size()) return "PREM_SEQUENCE_INVALID";
    const auto& sequence = model.sequences[request.sequenceIndex];
    if (!sequence.Usable() || sequence.end <= sequence.start || sequence.end > INT32_MAX)
        return "PREM_SEQUENCE_UNSUPPORTED";
    schedule.duration = std::uint64_t(sequence.end) - sequence.start;
    schedule.looping = sequence.flags == 0;
    const auto add = [&](std::uint64_t begin, std::uint64_t end) {
        if (begin == end) return;
        if (!schedule.intervals.empty() && schedule.intervals.back().end == begin) {
            schedule.intervals.back().end = end;
        } else schedule.intervals.push_back({begin, end, schedule.active});
        schedule.active += end - begin;
    };
    const auto& optional = request.emitter->Source().tracks[static_cast<std::size_t>(MdxPremParameter::Visibility)];
    if (!optional || optional->keys.empty()) {
        add(0, schedule.duration);
        return nullptr;
    }
    if (optional->keys.size() > limits.maxVisibilityKeyVisits - frame.visibilityKeyVisits)
        return "PREM_VISIBILITY_VISIT_LIMIT";
    frame.visibilityKeyVisits += optional->keys.size();
    // 动作内 None 阶梯；有轨迹但动作中无键时不借用其它动作的键/可见状态。
    std::uint64_t cursor = 0;
    bool visible = false, found = false;
    for (const auto& key : optional->keys) {
        if (key.frame < static_cast<std::int32_t>(sequence.start) ||
            key.frame > static_cast<std::int32_t>(sequence.end)) continue;
        found = true;
        const auto at = std::uint64_t(key.frame) - sequence.start;
        if (visible) add(cursor, at);
        cursor = at;
        visible = key.value > 0;
    }
    if (!found) return "PREM_VISIBILITY_SEQUENCE_UNSUPPORTED";
    if (visible) add(cursor, schedule.duration);
    return nullptr;
}

std::uint64_t ActiveUntil(const Schedule& schedule, std::uint64_t exclusiveMs) {
    std::uint64_t active = 0;
    if (schedule.looping) {
        active = (exclusiveMs / schedule.duration) * schedule.active;
        exclusiveMs %= schedule.duration;
    } else exclusiveMs = std::min(exclusiveMs, schedule.duration);
    const auto found = std::upper_bound(schedule.intervals.begin(), schedule.intervals.end(), exclusiveMs,
        [](std::uint64_t value, const Interval& interval) { return value < interval.begin; });
    if (found != schedule.intervals.begin()) {
        const auto& interval = *std::prev(found);
        active += interval.prefix + std::min(exclusiveMs, interval.end) - interval.begin;
    }
    return active;
}

std::uint64_t BirthCount(const Schedule& schedule, std::uint64_t exclusiveMs, float rate) {
    // active <= UINT32_MAX+1、rate<=4096；积最大2^44，转换范围/精度有界。
    return static_cast<std::uint64_t>(std::floor(static_cast<long double>(ActiveUntil(schedule, exclusiveMs)) *
        static_cast<long double>(rate) / 1000.0L));
}

bool BirthTime(const Schedule& schedule, float rate, std::uint64_t ordinal, std::uint64_t& birth) {
    if (!schedule.active || rate <= 0 || !ordinal) return false;
    // 可见的1ms格在其起点发射；ceil(active milliseconds) 的末格是本次出生格。
    // 因此可见窗口 end 的键变为0时，不会在 end 误发射或在下一窗口补旧发射。
    const auto required = static_cast<std::uint64_t>(std::ceil(static_cast<long double>(ordinal) * 1000.0L /
        static_cast<long double>(rate)));
    if (!required) return false;
    const auto cycle = schedule.looping ? (required - 1) / schedule.active : 0;
    const auto within = required - cycle * schedule.active;
    if (within > schedule.active) return false;
    const auto found = std::lower_bound(schedule.intervals.begin(), schedule.intervals.end(), within,
        [](const Interval& interval, std::uint64_t value) { return interval.prefix + interval.end - interval.begin < value; });
    if (found == schedule.intervals.end() || within <= found->prefix) return false;
    birth = cycle * schedule.duration + found->begin + within - found->prefix - 1;
    return birth <= kMaximumClock;
}

std::uint64_t Mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

double Uniform(std::uint64_t value) {
    return static_cast<double>(Mix(value) >> 11) * (1.0 / 9007199254740992.0);
}

bool Number(float value) { return std::isfinite(value); }

bool Transform(const Request& request, const War3RenderCore::NodePose& node, std::uint64_t ordinal,
    std::uint64_t ageMs, std::array<float, 12>& output) {
    const auto& emitter = request.emitter->Source();
    if (node.objectId != emitter.nodeId || emitter.nodeId >= request.emitter->Model().pivots.size() ||
        node.pivot != request.emitter->Model().pivots[emitter.nodeId] ||
        !std::all_of(node.pivot.begin(), node.pivot.end(), Number) ||
        !std::all_of(node.worldMatrix.begin(), node.worldMatrix.end(), Number) ||
        node.worldMatrix[3] != 0 || node.worldMatrix[7] != 0 || node.worldMatrix[11] != 0 || node.worldMatrix[15] != 1)
        return false;
    const auto seed = Mix(request.identity.value) ^ Mix(request.identity.generation) ^
        Mix(request.emitter->Binding().parentAsset.value) ^ Mix(request.emitter->Binding().parentAsset.generation) ^
        Mix(emitter.sourceIndex) ^ Mix(request.sequenceIndex) ^ Mix(ordinal);
    const auto longitude = (Uniform(seed) - 0.5) * static_cast<double>(emitter.longitude);
    const auto latitude = Uniform(seed ^ 0x8ebc6af09c88c6e3ull) * static_cast<double>(emitter.latitude);
    const std::array<double, 3> direction{
        std::sin(latitude) * std::cos(longitude), std::sin(latitude) * std::sin(longitude), std::cos(latitude)};
    const double age = static_cast<double>(ageMs) / 1000.0;
    std::array<double, 3> modelPosition{};
    for (std::size_t row = 0; row < 3; ++row) {
        modelPosition[row] = node.worldMatrix[12 + row];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            modelPosition[row] += static_cast<double>(node.worldMatrix[axis * 4 + row]) *
                (static_cast<double>(node.pivot[axis]) + direction[axis] * emitter.speed * age);
        }
    }
    // 重力在父模型局部Z，不在世界Z；非均匀scale/旋转随父实例只应用一次。
    modelPosition[2] -= 0.5 * static_cast<double>(emitter.gravity) * age * age;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            double value = column == 3 ? request.parentTransform[row * 4 + 3] : 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                value += static_cast<double>(request.parentTransform[row * 4 + axis]) *
                    (column == 3 ? modelPosition[axis] : node.worldMatrix[column * 4 + axis]);
            }
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) return false;
            output[row * 4 + column] = static_cast<float>(value);
        }
    }
    return true;
}

void IssueFor(Frame& frame, const Request& request, const char* code, std::uint64_t rejected = 0) {
    frame.issues.push_back({request.emitter, request.identity, request.requestIndex, code, rejected});
}
} // namespace

Result<std::shared_ptr<const Emitter>> CreateEmitter(std::shared_ptr<const MdxModel> model,
    std::uint32_t sourceIndex, std::uint32_t compiledNodeIndex, std::uint32_t childModelIndex, SourceBinding binding) {
    if (!model || sourceIndex >= model->premEmitters.size() || !binding.parentAsset.value ||
        !binding.parentAsset.generation || !SourceValid(binding.parentSource) || !SourceValid(binding.childSource) ||
        compiledNodeIndex >= MdxNodeCount(*model) || childModelIndex == UINT32_MAX)
        return Failure<std::shared_ptr<const Emitter>>("PREM_BINDING_INVALID", "模型发射器来源、节点或子资产绑定无效。");
    const auto& source = model->premEmitters[sourceIndex];
    if (source.sourceIndex != sourceIndex || source.otherNodeIndex >= model->otherNodes.size() ||
        model->otherNodes[source.otherNodeIndex].objectId != source.nodeId || source.nodeId >= model->pivots.size())
        return Failure<std::shared_ptr<const Emitter>>("PREM_NODE_MAPPING_INVALID", "模型发射器未关联真实公共节点和轴心。");
    if (source.support != MdxPremSupport::Supported || source.flags != 0x9000 ||
        !source.pathTerminated || source.path.empty() || source.path.size() >= source.rawPath.size() ||
        source.uninterpretedTailBytes)
        return Failure<std::shared_ptr<const Emitter>>("PREM_SOURCE_UNSUPPORTED", "当前仅支持完整、明确的0x9000模型发射器。");
    const std::array<float, 6> parameters{source.emissionRate, source.gravity, source.longitude,
        source.latitude, source.lifeSpan, source.speed};
    if (!std::all_of(parameters.begin(), parameters.end(), Number) || source.emissionRate < 0 || source.emissionRate > 4096 ||
        std::abs(source.gravity) > 100000 || source.longitude < 0 || source.longitude > 2 * std::numbers::pi_v<float> ||
        source.latitude < 0 || source.latitude > std::numbers::pi_v<float> ||
        source.lifeSpan <= 0 || source.lifeSpan > 30 || source.speed < 0 || source.speed > 100000)
        return Failure<std::shared_ptr<const Emitter>>("PREM_PARAMETERS_UNSUPPORTED", "模型发射器六个静态参数非有限或超过基础预览上限。");
    for (std::size_t parameter = 0; parameter < source.tracks.size(); ++parameter) {
        const auto& track = source.tracks[parameter];
        if (!track) continue;
        if (parameter != static_cast<std::size_t>(MdxPremParameter::Visibility) || track->interpolation != 0 ||
            track->globalSequence != UINT32_MAX || track->keys.size() > kMaximumKeys ||
            track->sourceKeyCount != track->keys.size())
            return Failure<std::shared_ptr<const Emitter>>("PREM_TRACK_UNSUPPORTED", "基础预览仅支持非全局None可见性，不支持动态发射参数。");
        std::int32_t last = -1;
        for (const auto& key : track->keys) {
            if (key.frame <= last || !Number(key.value) || key.value < 0 || key.value > 1)
                return Failure<std::shared_ptr<const Emitter>>("PREM_VISIBILITY_INVALID", "模型发射器可见性原键必须严格递增且为有限0到1数值。");
            last = key.frame;
        }
    }
    try {
        auto emitter = std::shared_ptr<Emitter>(new Emitter);
        emitter->source_ = source;
        emitter->model_ = std::move(model);
        emitter->binding_ = std::move(binding);
        emitter->nodeIndex_ = compiledNodeIndex;
        emitter->childIndex_ = childModelIndex;
        return Result<std::shared_ptr<const Emitter>>::Success(std::move(emitter));
    } catch (const std::bad_alloc&) {
        return Failure<std::shared_ptr<const Emitter>>("PREM_ALLOCATION_FAILED", "模型发射器绑定内存分配失败。");
    }
}

Result<Frame> Build(std::span<const Request> requests, const NodeSampler& sampler, const Limits& limits, std::stop_token stop) {
    if (!sampler || !LimitsValid(limits, requests.size()))
        return Failure<Frame>("PREM_FRAME_LIMIT", "模型粒子请求数、帧字节或工作预算无效；硬上限不能放宽。");
    if (stop.stop_requested()) return Failure<Frame>("PREM_CANCELLED", "模型粒子候选已取消，没有发布部分结果。");
    try {
        Frame frame;
        frame.children.reserve(limits.maxLiveChildren);
        frame.issues.reserve(requests.size());
        for (const auto& request : requests) {
            if (stop.stop_requested()) return Failure<Frame>("PREM_CANCELLED", "模型粒子候选已取消，没有发布部分结果。");
            if (!request.emitter || !request.identity.value || !request.identity.generation || request.requestIndex == UINT32_MAX ||
                request.elapsedMs > kMaximumClock || request.globalMs > kMaximumClock ||
                !std::all_of(request.parentTransform.begin(), request.parentTransform.end(), Number)) {
                ++frame.unsupportedRequests;
                IssueFor(frame, request, "PREM_REQUEST_INVALID");
                continue;
            }
            Schedule schedule;
            schedule.intervals.reserve(kMaximumKeys + 1ull);
            if (const auto* code = MakeSchedule(request, limits, frame, schedule)) {
                ++frame.unsupportedRequests;
                IssueFor(frame, request, code);
                continue;
            }
            const auto& emitter = request.emitter->Source();
            const auto life = static_cast<std::uint64_t>(std::ceil(static_cast<double>(emitter.lifeSpan) * 1000.0));
            const auto first = request.elapsedMs + 1 > life ? request.elapsedMs + 1 - life : 0;
            const auto before = BirthCount(schedule, first, emitter.emissionRate);
            const auto last = BirthCount(schedule, request.elapsedMs + 1, emitter.emissionRate);
            const auto candidates = last - before;
            frame.candidateBirths += candidates;
            const auto initialSize = frame.children.size();
            const char* rejection = nullptr;
            std::uint64_t clockRejected = 0, suppressed = 0;
            for (std::uint64_t ordinal = before + 1; ordinal <= last; ++ordinal) {
                if (stop.stop_requested()) return Failure<Frame>("PREM_CANCELLED", "模型粒子候选已取消，没有发布部分结果。");
                if (frame.evaluatedBirths == limits.maxBirths || frame.anchorVisits == limits.maxAnchorVisits ||
                    frame.children.size() == limits.maxLiveChildren) {
                    suppressed += last - ordinal + 1;
                    if (!rejection) rejection = "PREM_CHILD_BUDGET";
                    break;
                }
                ++frame.evaluatedBirths;
                std::uint64_t birth = 0;
                if (!BirthTime(schedule, emitter.emissionRate, ordinal, birth) || birth > request.elapsedMs ||
                    request.elapsedMs - birth >= life) {
                    rejection = "PREM_BIRTH_CLOCK_INVALID";
                    break;
                }
                const auto age = request.elapsedMs - birth;
                if (request.globalMs < age) {
                    ++clockRejected;
                    ++suppressed;
                    if (!rejection) rejection = "PREM_BIRTH_GLOBAL_UNDERFLOW";
                    continue;
                }
                const auto global = request.globalMs - age;
                ++frame.anchorVisits;
                const auto pose = sampler(request, birth, global);
                Child child;
                child.emitter = request.emitter;
                child.parentIdentity = request.identity;
                child.parentRequestIndex = request.requestIndex;
                child.parentSequenceIndex = request.sequenceIndex;
                child.birthOrdinal = ordinal;
                child.birthElapsedMs = birth;
                child.birthGlobalMs = global;
                child.ageMs = age;
                if (!pose.Succeeded() || !Transform(request, *pose.value, ordinal, age, child.transform)) {
                    // 失败发射器整条局部省略；不能把错误锚点后的部分池当完整成功。
                    rejection = pose.Succeeded() ? "PREM_NODE_TRANSFORM_INVALID" : "PREM_NODE_SAMPLE_FAILED";
                    break;
                }
                frame.children.push_back(std::move(child));
            }
            if (rejection && std::string_view(rejection) != "PREM_CHILD_BUDGET" &&
                std::string_view(rejection) != "PREM_BIRTH_GLOBAL_UNDERFLOW") {
                frame.children.resize(initialSize);
                ++frame.unsupportedRequests;
                // 单request候选最大4096*30000/1000；总request<=4096，累计远低于uint64上限。
                suppressed = candidates;
            }
            frame.suppressedChildren += suppressed;
            frame.clockRejectedChildren += clockRejected;
            if (rejection) IssueFor(frame, request, rejection, suppressed);
        }
        if (stop.stop_requested()) return Failure<Frame>("PREM_CANCELLED", "模型粒子候选已取消，没有发布部分结果。");
        return Result<Frame>::Success(std::move(frame));
    } catch (const std::bad_alloc&) {
        return Failure<Frame>("PREM_ALLOCATION_FAILED", "模型粒子帧内存分配失败，没有发布部分结果。");
    } catch (...) {
        return Failure<Frame>("PREM_SAMPLE_EXCEPTION", "模型粒子采样回调发生异常，没有发布部分结果。");
    }
}

} // namespace War3Map::Render::Prem
