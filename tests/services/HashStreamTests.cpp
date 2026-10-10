#include "War3MapCore/Hash.h"

#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::span<const std::byte> Bytes(std::string_view value) { return std::as_bytes(std::span(value.data(), value.size())); }
std::string Finish(Sha256Stream& stream) {
    auto result = stream.Finish(); Require(result.Succeeded(), "finish failed"); return std::move(*result.value);
}
void KnownVectorsAndIndependentStates() {
    Sha256Stream empty(0), abc(3), interleaved(3);
    Require(abc.Update(Bytes("a")).Succeeded() && interleaved.Update(Bytes("ab")).Succeeded(), "independent update");
    Require(abc.Update(Bytes("bc")).Succeeded() && interleaved.Update(Bytes("c")).Succeeded(), "independent tail");
    Require(Finish(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty vector");
    const auto expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    Require(Finish(abc) == expected && Finish(interleaved) == expected, "abc vector");
    Require(!abc.Update({}).Succeeded() && !abc.Finish().Succeeded(), "terminal success reuse");
}
void FragmentBoundaries() {
    std::string input(Sha256Stream::kBufferBytes * 3 + 17, '\0');
    for (std::size_t index = 0; index < input.size(); ++index) input[index] = static_cast<char>(index * 37);
    const auto reference = Sha256Text(input); Require(reference.Succeeded(), "legacy binary hash");
    for (const auto step : std::array<std::size_t, 8>{1, 7, 55, 64, 65535, 65536, 65537, input.size()}) {
        Sha256Stream stream(input.size());
        for (std::size_t offset = 0; offset < input.size();) {
            const auto count = (std::min)(step, input.size() - offset);
            Require(stream.Update(Bytes(std::string_view(input).substr(offset, count))).Succeeded(), "fragment update");
            offset += count;
        }
        Require(stream.BytesAccepted() == input.size(), "accepted includes pending tail");
        Require(stream.ScratchBytes() < Sha256Stream::kBufferBytes + 16384, "bounded logical scratch");
        Require(Finish(stream) == *reference.value, "fragment legacy equivalence");
    }
}
void BudgetIsTotalAndFailureIsSticky() {
    Sha256Stream limited(3);
    Require(limited.Update(Bytes("ab")).Succeeded() && limited.BytesAccepted() == 2, "pending bill");
    const auto failed = limited.Update(Bytes("cd"));
    Require(!failed.Succeeded() && failed.diagnostics.front().code == "HASH_STREAM_BUDGET", "total admission");
    Require(limited.BytesAccepted() == 2 && !limited.Update(Bytes("c")).Succeeded() && !limited.Finish().Succeeded(), "no prefix digest after failure");
    Sha256Stream zero(0);
    Require(!zero.Update(Bytes("x")).Succeeded() && !zero.Finish().Succeeded(), "zero limit rejects nonempty");
    Sha256Stream exact(3);
    Require(exact.Update(Bytes("abc")).Succeeded() && exact.Update({}).Succeeded(), "exact cap allowed");
    Require(Finish(exact) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "exact digest");
}
}
int main() {
    try {
        KnownVectorsAndIndependentStates(); FragmentBoundaries(); BudgetIsTotalAndFailureIsSticky();
        std::cout << "incremental SHA vectors, fragmentation, bounded scratch and terminal state passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
