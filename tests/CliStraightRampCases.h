#pragma once

// 在 CliMapEditSaveTests.cpp 的测试命名空间内使用同一调用捕获器。
void StraightRampUsesItsOwnWorldPointCommand() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--straight-ramp", L"-992", L"-928", L"true"});
        return args;
    };
    for (const auto value : {L"true", L"false"}) {
        auto args = base(); args[11] = value;
        const auto result = Invoke(args);
        Require(result.exitCode != 0 && result.output.empty()
            && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos
            && result.error.find("SAVE_STROKE_KIND") == std::string::npos,
            "straight creation/removal must reach the real shared source gate");
    }
    for (const auto value : {L"nan", L"inf", L"16777217", L"-16777217"}) {
        for (const auto index : {9, 10}) {
            auto args = base(); args[index] = value;
            Rejected(Invoke(args), "requires finite world");
        }
    }
    for (const auto value : {L"1", L"0", L"True", L"falsex"}) {
        auto args = base(); args[11] = value;
        Rejected(Invoke(args), "requires finite world");
    }
    auto incomplete = base(); incomplete.pop_back();
    Rejected(Invoke(incomplete), "requires world x, world y");
    auto privateVersion = base();
    privateVersion.insert(privateVersion.end(), {L"--straight-ramp-algorithm", L"1"});
    Rejected(Invoke(privateVersion), "unknown map-edit-save option");

    auto mixed = base();
    mixed.insert(mixed.end(), {L"--corner-ramp", L"-992", L"-928", L"false"});
    mixed.insert(mixed.end(), {L"--stroke", L"ramp", L"circle", L"4", L"4", L"0", L"true"});
    Require(Invoke(mixed).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "new straight, corner and historical dry ramp retain one ordered save request");
    auto tooMany = Arguments();
    for (unsigned i = 0; i < 129; ++i)
        tooMany.insert(tooMany.end(), {i % 2 ? L"--straight-ramp" : L"--corner-ramp", L"-992", L"-928", L"true"});
    Rejected(Invoke(tooMany), "at most 128 ordered strokes");
}
