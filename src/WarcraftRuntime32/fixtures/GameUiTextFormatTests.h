void TextFormatContracts() {
    std::array<TextFormatArgument, 8> arguments{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        arguments[index].present = true;
        arguments[index].integer = true;
        arguments[index].value = static_cast<std::int32_t>(index);
    }
    const auto all = CompiledTextFormat::Parse("{0}/{1}/{2}/{3}/{4}/{5}/{6}/{7}");
    Check(all.has_value() && all->RequiredMask() == 0xffu, "format accepts exactly eight numeric slots");
    Check(all && all->Render(arguments) == "0/1/2/3/4/5/6/7", "all eight slots render in order");
    arguments[0].integer = false;
    arguments[0].real = 1.25f;
    const auto repeated = CompiledTextFormat::Parse("{0}|{0:0}|{0:1}|{0:2}|{0:6}");
    Check(repeated && repeated->RequiredMask() == 1u
        && repeated->Render(arguments) == "1.25|1|1.3|1.25|1.250000", "one slot can use repeated distinct precisions");
    const auto braces = CompiledTextFormat::Parse("{{{0:1}}} {{{{ }}}}");
    Check(braces && braces->Render(arguments) == "{1.3} {{ }}", "escaped braces retain their literal meaning");
    const auto color = CompiledTextFormat::Parse("|cffff0000生命 {0:2}|r");
    Check(color && color->Render(arguments) == "|cffff0000生命 1.25|r", "UTF-8 literals and color codes remain unchanged");
    const auto constant = CompiledTextFormat::Parse("literal text");
    const auto empty = CompiledTextFormat::Parse("");
    Check(constant && constant->RequiredMask() == 0 && constant->Render({}) == "literal text", "constant format needs no arguments");
    Check(empty && empty->RequiredMask() == 0 && empty->Render({}) == "", "empty format clears text without arguments");

    auto original = CompiledTextFormat::Parse("x{0}");
    Check(original.has_value(), "short format compilation succeeds");
    if (original) {
        auto copied = *original;
        original.reset();
        auto moved = std::move(copied);
        Check(moved.Render(arguments) == "x1.25", "copy and move keep short-string literal offsets valid");
    }
    auto longOriginal = CompiledTextFormat::Parse(std::string(100, 'x') + "{0}");
    Check(longOriginal.has_value(), "long format compilation succeeds");
    if (longOriginal) {
        auto copied = *longOriginal;
        longOriginal.reset();
        auto moved = std::move(copied);
        Check(moved.Render(arguments) == std::string(100, 'x') + "1.25", "copy and move keep long-string offsets valid");
    }

    auto missing = arguments;
    missing[0].present = false;
    Check(repeated && !repeated->Render(missing), "missing referenced numeric slot rejects the entire result");
    missing = arguments;
    missing[7].present = false;
    Check(repeated && repeated->Render(missing) == "1.25|1|1.3|1.25|1.250000", "unreferenced missing slots are irrelevant");
    const auto single = CompiledTextFormat::Parse("{0}");
    Check(single.has_value(), "single-slot format compiles");
    auto invalidReal = arguments;
    invalidReal[0].real = std::numeric_limits<float>::quiet_NaN();
    Check(single && !single->Render(invalidReal), "NaN cannot render as numeric text");
    invalidReal[0].real = std::numeric_limits<float>::infinity();
    Check(single && !single->Render(invalidReal), "infinity cannot render as numeric text");
    invalidReal[0].real = -std::numeric_limits<float>::infinity();
    Check(single && !single->Render(invalidReal), "negative infinity cannot render as numeric text");
    auto integerArguments = arguments;
    integerArguments[0].integer = true;
    integerArguments[0].value = (std::numeric_limits<std::int32_t>::min)();
    Check(single && single->Render(integerArguments) == "-2147483648", "format preserves int32 minimum without a real conversion");
    integerArguments[0].value = (std::numeric_limits<std::int32_t>::max)();
    const auto fixedInteger = CompiledTextFormat::Parse("{0:6}");
    Check(fixedInteger && fixedInteger->Render(integerArguments) == "2147483647.000000", "fixed integer precision preserves int32 maximum");
    integerArguments[0].value = 16777217;
    Check(single && single->Render(integerArguments) == "16777217", "integer text remains exact above binary32 integer precision");

    const auto plainLimit = CompiledTextFormat::Parse(std::string(384, 'x'));
    Check(plainLimit && plainLimit->Render({}) == std::string(384, 'x'), "384 ordinary encoded bytes are accepted");
    Check(!CompiledTextFormat::Parse(std::string(385, 'x')), "385 ordinary encoded bytes are rejected without truncation");
    const auto percentLimit = CompiledTextFormat::Parse(std::string(128, '%'));
    const auto separatorLimit = CompiledTextFormat::Parse(std::string(128, ';'));
    Check(percentLimit && percentLimit->Render({}) == std::string(128, '%'), "percent counts as three encoded bytes at the exact limit");
    Check(separatorLimit && separatorLimit->Render({}) == std::string(128, ';'), "separator counts as three encoded bytes at the exact limit");
    Check(!CompiledTextFormat::Parse(std::string(129, '%')) && !CompiledTextFormat::Parse(std::string(129, ';')),
        "percent and separator input over the encoded limit are rejected");
    const std::string escapedPrefix = std::string(63, '%') + std::string(63, ';');
    const auto expansion = CompiledTextFormat::Parse(escapedPrefix + "{0}");
    integerArguments[0].value = 123456;
    Check(expansion && expansion->Render(integerArguments) == escapedPrefix + "123456", "expanded numeric output can exactly reach the encoded limit");
    integerArguments[0].value = 1234567;
    Check(expansion && !expansion->Render(integerArguments), "output exceeding the encoded limit is rejected rather than shortened");
    integerArguments[0].value = 1;
    Check(expansion && expansion->Render(integerArguments) == escapedPrefix + "1", "failed rendering does not mutate the compiled format");

    const char* invalidFormats[] = {"{}", "{8}", "{00}", "{10}", "{0:}", "{0:7}", "{0:00}", "{0:-1}",
        "{0:1x}", "{0x}", "{ 0}", "{0 }", "{", "}", "text{", "text}", "{0", "{0:1", "{0}}", "{{}"};
    for (const auto* format : invalidFormats) {
        Check(!CompiledTextFormat::Parse(format), "malformed placeholder or dangling brace is rejected");
    }
    Check(!CompiledTextFormat::Parse(std::string("a\0b", 3)), "embedded NUL is rejected");
    Check(!CompiledTextFormat::Parse("a\nb") && !CompiledTextFormat::Parse("a\tb"), "control characters are rejected");
    Check(!CompiledTextFormat::Parse(std::string(1, '\x7f')), "DEL is rejected");
    Check(!CompiledTextFormat::Parse(std::string("\xc0\xaf", 2)), "overlong UTF-8 is rejected");
    Check(!CompiledTextFormat::Parse(std::string("\xed\xa0\x80", 3)), "UTF-8 surrogate encoding is rejected");
    Check(!CompiledTextFormat::Parse(std::string("\xe7\x94", 2)), "incomplete UTF-8 is rejected");
}
