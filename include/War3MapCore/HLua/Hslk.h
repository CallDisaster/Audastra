#pragma once

#include "War3MapCore/HLua/Project.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include <chrono>
#include <map>

namespace War3Map::HLua {

using FieldAtom = std::variant<double, std::string>;
struct HslkField { std::string name; std::vector<FieldAtom> values; bool array = false; };
struct HslkObject {
    std::string objectClass, id, parent, name, source;
    std::vector<HslkField> fields;
};
struct HslkLimits {
    std::size_t memoryBytes = 256u * 1024u * 1024u;
    std::size_t objects = 65536;
    std::size_t fieldBytes = 64u * 1024u * 1024u;
    std::uint64_t instructions = 100'000'000;
    std::chrono::milliseconds timeout{30000};
};
struct HslkOutput {
    std::vector<HslkObject> objects;
    std::vector<std::string> generatedIds;
    std::string console;
    std::size_t peakMemoryBytes = 0;
    std::uint64_t instructions = 0;
    bool replayPassed = false;
    std::string injectedSlk;
};

[[nodiscard]] Result<HslkOutput> EvaluateHslk(const FrameworkSnapshot& framework,
    const ProjectSnapshot& project, std::span<const std::string> existingIds, const HslkLimits& limits = {});

} // namespace War3Map::HLua
