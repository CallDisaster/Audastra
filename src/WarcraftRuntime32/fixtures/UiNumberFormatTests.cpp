#include "War3MapCore/Text/UiNumberFormat.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>

namespace {
using War3Map::Text::FormatUiInteger;
using War3Map::Text::FormatUiReal;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Real(float input, int precision, const std::string& expected) {
    const auto actual = FormatUiReal(input, precision);
    if (!actual || *actual != expected) {
        throw std::runtime_error("real precision=" + std::to_string(precision) + " expected=" + expected
            + " actual=" + actual.value_or("<rejected>"));
    }
}

void Golden() {
    Real(0.0f, -1, "0"); Real(1.0f, -1, "1"); Real(-1.0f, -1, "-1");
    Real(1.5f, -1, "1.5"); Real(1.2f, -1, "1.2"); Real(1000.0f, -1, "1000");
    Real(123.125f, -1, "123.125");
    Real(1.0f, 2, "1.00"); Real(1.2f, 2, "1.20"); Real(0.0f, 6, "0.000000");
    Real(2.675f, 2, "2.67"); Real(-2.675f, 2, "-2.67");
    Real(16777216.0f, 0, "16777216");
    Real((std::numeric_limits<float>::max)(), 0, "340282346638528859811704183484516925440");
    Real((std::numeric_limits<float>::max)(), 6, "340282346638528859811704183484516925440.000000");
    Real(-(std::numeric_limits<float>::max)(), 6, "-340282346638528859811704183484516925440.000000");
    Real((std::numeric_limits<float>::min)(), 6, "0.000000");
    Real(std::numeric_limits<float>::denorm_min(), -1, "0");
    Real(-std::numeric_limits<float>::denorm_min(), 6, "0.000000");
    Real(-0.0001f, -1, "0"); Real(-0.0001f, 2, "0.00");
    for (int precision = -1; precision <= 6; ++precision) {
        const std::string zero = precision > 0 ? "0." + std::string(static_cast<std::size_t>(precision), '0') : "0";
        Real(-0.0f, precision, zero); Real(0.0f, precision, zero);
    }
}

void HalfValues() {
    struct Case { float value; int precision; const char* below; const char* half; };
    constexpr std::array<Case, 13> cases{{
        {0.5f, 0, "0", "1"}, {1.5f, 0, "1", "2"}, {2.5f, 0, "2", "3"},
        {9.5f, 0, "9", "10"}, {1.25f, 1, "1.2", "1.3"}, {1.75f, 1, "1.7", "1.8"},
        {123.125f, 2, "123.12", "123.13"}, {1.375f, 2, "1.37", "1.38"},
        {0.0625f, 3, "0.062", "0.063"}, {0.1875f, 3, "0.187", "0.188"},
        {0.03125f, 4, "0.0312", "0.0313"}, {0.015625f, 5, "0.01562", "0.01563"},
        {0.0078125f, 6, "0.007812", "0.007813"},
    }};
    for (const auto& item : cases) {
        const auto below = std::nextafter(item.value, 0.0f);
        const auto above = std::nextafter(item.value, std::numeric_limits<float>::infinity());
        Real(below, item.precision, item.below);
        Real(item.value, item.precision, item.half);
        Real(above, item.precision, item.half);
        const std::string negativeBelow = std::string(item.below) == "0" ? "0" : "-" + std::string(item.below);
        Real(-below, item.precision, negativeBelow);
        Real(-item.value, item.precision, "-" + std::string(item.half));
        Real(-above, item.precision, "-" + std::string(item.half));
    }
    Real(0.0625f, -1, "0.063"); Real(-0.0625f, -1, "-0.063");
}

void IntegerAndRejections() {
    Check(FormatUiInteger(2147483647) == "2147483647", "int32 maximum stays exact");
    Check(FormatUiInteger((std::numeric_limits<std::int32_t>::min)()) == "-2147483648", "int32 minimum stays exact");
    Check(FormatUiInteger(16777217) == "16777217", "integer does not round through float");
    Check(FormatUiInteger(2147483647, 6) == "2147483647.000000", "large integer fixed precision");
    Check(FormatUiInteger((std::numeric_limits<std::int32_t>::min)(), 2) == "-2147483648.00", "negative integer fixed precision");
    Check(FormatUiInteger(0, 0) == "0" && FormatUiInteger(-3, 0) == "-3", "integer zero precision has no decimal point");
    for (int precision = 1; precision <= 6; ++precision) {
        Check(FormatUiInteger(0, precision) == "0." + std::string(static_cast<std::size_t>(precision), '0'), "integer zero padding");
    }
    for (const auto invalid : {-2, 7, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()}) {
        Check(!FormatUiReal(1.0f, invalid) && !FormatUiInteger(1, invalid), "unsupported precision rejected");
    }
    for (int precision = -1; precision <= 6; ++precision) {
        Check(!FormatUiReal(std::numeric_limits<float>::infinity(), precision), "positive infinity rejected");
        Check(!FormatUiReal(-std::numeric_limits<float>::infinity(), precision), "negative infinity rejected");
        Check(!FormatUiReal(std::numeric_limits<float>::quiet_NaN(), precision), "NaN rejected");
    }
}

class CommaPunctuation final : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

void LocaleIndependence() {
    const std::locale previous = std::locale();
    struct Restore { std::locale value; ~Restore() { std::locale::global(value); } } restore{previous};
    std::locale::global(std::locale(previous, new CommaPunctuation));
    Real(12345.125f, 2, "12345.13");
    Real(-12345.125f, -1, "-12345.125");
    Check(FormatUiInteger(1234567, 2) == "1234567.00", "global locale does not change integer formatting");
}
} // namespace

int main() {
    try {
        Golden(); HalfValues(); IntegerAndRejections(); LocaleIndependence();
        std::cout << "UI number formatting fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UI number formatting fixture failed: " << error.what() << '\n';
        return 1;
    }
}
