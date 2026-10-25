#pragma once

void PathingRestrictionUsesTypedSharedSave() {
    const auto base = [] {
        auto args = Arguments();
        args.insert(args.end(), {L"--pathing-stroke", L"walk", L"circle", L"-16", L"16", L"1", L"true"});
        return args;
    };
    for (const auto type : {L"walk", L"fly", L"build"})
        for (const auto shape : {L"circle", L"square"})
            for (const auto value : {L"true", L"false"}) {
                auto args = base(); args[9] = type; args[10] = shape; args[14] = value;
                const auto result = Invoke(args);
                Require(result.exitCode != 0 && result.output.empty()
                    && result.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
                    "typed pathing set/clear must reach the actual shared source gate");
            }
    for (const auto type : {L"2", L"all", L"Walk", L""}) {
        auto args = base(); args[9] = type; Rejected(Invoke(args), "blocked type must be walk");
    }
    auto args = base(); args[10] = L"triangle"; Rejected(Invoke(args), "shape must be circle or square");
    for (const auto [index, value] : std::vector<std::pair<std::size_t, std::wstring>>{
            {11, L"nan"}, {12, L"inf"}, {11, L"16777217"}, {12, L"-16777217"},
            {13, L"0"}, {13, L"9"}, {13, L"1.5"}, {13, L"-1"},
            {14, L"1"}, {14, L"falsex"}}) {
        args = base(); args[index] = value; Rejected(Invoke(args), "finite world x/y");
    }
    args = base(); args.pop_back(); Rejected(Invoke(args), "requires blocked type");
    args = base(); args.push_back(L"2"); Rejected(Invoke(args), "unknown map-edit-save option");
    args = Arguments();
    for (unsigned i = 0; i < 129; ++i)
        args.insert(args.end(), {L"--pathing-stroke", L"walk", L"square", L"16", L"16", L"1", L"true"});
    Rejected(Invoke(args), "at most 128 ordered strokes");
    args = base();
    args.insert(args.end(), {L"--pathing-stroke", L"walk", L"circle", L"-16", L"16", L"1", L"false"});
    args.insert(args.end(), {L"--stroke", L"raise", L"circle", L"4", L"4", L"0", L"8"});
    Require(Invoke(args).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "set/clear and height use one bounded ordered terrain request");
}
