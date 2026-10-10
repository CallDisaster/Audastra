// 人工运行的扫描器原创样例/子进程预算探针，不登记 CTest，不读取游戏资产。
#include "MdxTestModel.h"
#include "GeosetChannelFixture.h"
#include "RenderCoverageParticles.h"
#include "../src/Render/Tools/MdxScan/ScanTypes.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
namespace {
using namespace War3Map::Render::Scan;
std::size_t Find(const std::vector<std::byte>& bytes, const char* tag, int occurrence = 1) {
    for (std::size_t i = 0; i+4 <= bytes.size(); ++i)
        if (std::memcmp(bytes.data()+i,tag,4) == 0 && --occurrence == 0) return i;
    throw std::runtime_error("测试标签缺失");
}
void Put(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) { std::memcpy(bytes.data()+offset,&value,4); }
// 原零法线样本已成为支持项；保留真正的局部回退阳性：无关 HELP 带未知位。
// 不修改旧入口可用的骨骼链，也不放宽编译器/差分断言。
std::vector<std::byte> UnusedInvalidNode(const std::vector<std::byte>& source) {
    MdxTest::Writer out; out.Tag("MDLX");
    for (std::size_t at = 4; at < source.size();) {
        std::uint32_t size = 0; std::memcpy(&size,source.data()+at+4,4);
        if (std::memcmp(source.data()+at,"PIVT",4) == 0) {
            out.Tag("PIVT"); out.U32(size+12); out.Raw(source.data()+at+8,size);
            out.F32(0); out.F32(0); out.F32(0);
        } else out.Raw(source.data()+at,size+8);
        at += size+8;
    }
    out.Tag("HELP"); const auto chunk = out.Begin(); const auto node = out.Begin();
    out.Fixed("unused unknown flags",80); out.U32(2); out.U32(UINT32_MAX); out.U32(0x20000000);
    out.EndInclusive(node); out.EndChunk(chunk);
    return std::move(out.bytes);
}
int Entry(const std::vector<std::string>& args, const std::filesystem::path& executable) {
    if (args.size() == 1 && args[0] == "--sleep-child") { std::this_thread::sleep_for(std::chrono::seconds(20)); return 0; }
    if (args.size() == 1 && args[0] == "--memory-child") {
        try {
            std::vector<std::unique_ptr<char[]>> blocks;
            for (int i = 0; i < 128; ++i) { auto block = std::make_unique<char[]>(1024*1024); block[0] = 'x'; blocks.push_back(std::move(block)); }
            return 0;
        } catch (const std::bad_alloc&) { return 7; }
    }
    if (args.size() != 1) throw std::runtime_error("需要一个全新的合成样例目录");
    const auto root = FromUtf8(args[0]);
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("不覆盖已有样例目录");
    MdxTest::Options model; model.animatedHelper = true;
    const auto good = MdxTest::BuildTestMdx(model);
    const auto write = [&](const char* name, const auto& bytes) {
        std::ofstream out(root / name,std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("写入合成样例失败");
    };
    write("good.mdx",good);
    auto fallback = UnusedInvalidNode(good);
    write("fallback.mdx",fallback);
    auto boundary = good; Put(boundary,Find(good,"KGTR")+16,100); write("boundary.mdx",boundary);
    auto global = good; Put(global,Find(good,"KGTR")+12,0);
    MdxTest::Writer chunk; chunk.Tag("GLBS"); chunk.U32(4); chunk.U32(1000);
    global.insert(global.end(),chunk.bytes.begin(),chunk.bytes.end()); write("global.mdx",global);
    auto bad = good; bad.resize(bad.size()-3); write("bad.mdx",bad);
    std::ofstream list(root / "models.txt",std::ios::binary);
    list << "good.mdx\nfallback.mdx\nboundary.mdx\nglobal.mdx\nbad.mdx\n"; list.close();
    auto zero = good; std::fill_n(zero.begin()+static_cast<std::ptrdiff_t>(Find(good,"NRMS",2)+8),12,std::byte{0});
    write("zero-normal.mdx",zero);
    write("parent-attachment.mdx",Coverage::OtherParent("ATCH"));
    write("particles.mdx",Coverage::ParticleOnlyBytes("PRE2"));
    write("particles-no-sequence.mdx",Coverage::ParticleOnlyBytes("PRE2",false));
    // 首个动作现为 Stand [0,1000]：把起点改到终点之后，构成不可用区间。
    auto sequence = good; Put(sequence,Find(good,"SEQS")+8+80,2000); write("bad-sequence.mdx",sequence);
    for (const auto flags : {2u,4u}) {
        auto inherit = good; Put(inherit,Find(good,"BONE")+8+92,flags);
        write(flags == 2 ? "no-scale.mdx" : "no-rotation.mdx",inherit);
    }
    std::ofstream coverage(root / "coverage-models.txt",std::ios::binary);
    coverage << "good.mdx\nzero-normal.mdx\nparent-attachment.mdx\nparticles.mdx\nparticles-no-sequence.mdx\n"
        << "bad-sequence.mdx\nno-scale.mdx\nno-rotation.mdx\nfallback.mdx\n";
    coverage.close(); if (!coverage) throw std::runtime_error("写入覆盖清单失败");
    auto colored = GeosetChannelFixture::Bytes(1,false); write("animated-color.mdx",colored);
    auto flags = colored; Put(flags,Find(flags,"GEOA")+16,1); write("rejected-geoa-flag.mdx",flags);
    auto unsupportedGlobal = colored; Put(unsupportedGlobal,Find(unsupportedGlobal,"KGAC")+12,9); write("rejected-geoa-global.mdx",unsupportedGlobal);
    std::ofstream reasons(root / "reason-models.txt",std::ios::binary);
    reasons << "animated-color.mdx\nrejected-geoa-flag.mdx\nrejected-geoa-global.mdx\n";
    reasons.close(); if (!reasons) throw std::runtime_error("写入原因清单失败");
    Options options; options.executable = std::filesystem::absolute(executable); options.limits.memoryMiB = 32;
    const auto timeout = RunChild(options,{"--sleep-child"},30);
    const auto memory = RunChild(options,{"--memory-child"},5000);
    if (timeout != "SCAN_MODEL_TIMEOUT" || memory != "SCAN_CHILD_EXIT_7") throw std::runtime_error("子进程预算未被实际执行");
    std::cout << "scanner_fixtures=5 coverage_fixtures=9 timeout=" << timeout << " memory=" << memory << " limits_enforced=1\n";
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try { std::vector<std::string> args; for (int i = 1; i < argc; ++i) args.push_back(Utf8(std::filesystem::path(argv[i])));
        return Entry(args,std::filesystem::path(argv[0])); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
#else
int main(int argc, char** argv) {
    try { return Entry(std::vector<std::string>(argv+1,argv+argc),std::filesystem::path(argv[0])); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
#endif
