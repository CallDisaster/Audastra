#include "ScanTypes.h"
#include "ScanDetail.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
namespace War3Map::Render::Scan {
namespace {
std::string Digest(std::span<const std::byte> bytes) {
    // 扫描器内部仅用作不跨进程复用的资产来源标签，不作安全校验或缓存命中证据。
    // 明示非密码摘要种类；没有假装是 SHA-256。模型字节只读取这一份供两个入口共用。
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto b : bytes) { hash ^= std::to_integer<unsigned char>(b); hash *= 1099511628211ull; }
    std::ostringstream out; out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash; return out.str();
}
int OneModel(const Options& options) {
    Record record; record.path = options.model;
    const auto begin = Clock::now(); auto lastFlush = begin;
    const auto checkpoint = [&] {
        const auto now = Clock::now();
        record.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now-begin).count());
        if (record.elapsedMs >= options.limits.modelMs) throw Interrupted{"SCAN_MODEL_TIMEOUT"};
        if (now-lastFlush >= std::chrono::milliseconds(100)) { WriteRecord(options.tsv,record); lastFlush = now; }
    };
    try {
        WriteRecord(options.tsv,record);
        const auto bytes = ReadModel(options,record.source); record.digest = Digest(bytes); checkpoint();
        // 不用收紧后的解析器冒充参考的兼容拒绝；按冻结默认解析，然后单独做扫描预算门。
        auto parsed = ParseMdx(bytes);
        record.parse = parsed.Succeeded() ? "OK" : parsed.diagnostics.front().code;
        if (!parsed.Succeeded()) {
            record.state = "parse_rejected";
            for (const auto& diagnostic : parsed.diagnostics) AppendDetail(record, diagnostic.code + ":" + diagnostic.message);
        }
        else {
            DescribeModel(*parsed.value,record,checkpoint);
            std::uint64_t vertices = 0; for (const auto& g : parsed.value->geosets) vertices += g.positions.size();
            if (MdxNodeCount(*parsed.value) > options.limits.maxNodes || vertices > options.limits.maxVertices)
                record.state = "SCAN_MODEL_COMPLEXITY_LIMIT";
            else { WriteRecord(options.tsv,record); Compare(*parsed.value,record,options.limits,checkpoint); }
        }
    } catch (const Interrupted& e) { record.state = e.code; }
    catch (const std::bad_alloc&) { record.state = "SCAN_MODEL_MEMORY_LIMIT"; }
    catch (const std::exception& e) {
        const std::string code = e.what(); record.state = code.starts_with("SCAN_") ? code : "SCAN_WORKER_ERROR";
        record.firstSample = code;
    }
    record.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count());
    WriteRecord(options.tsv,record); return 0;
}
struct Scratch {
    std::filesystem::path path;
    Scratch() {
        const auto base = std::filesystem::temp_directory_path(); std::random_device random;
        for (int i = 0; i < 10; ++i) {
            const auto candidate = base / ("forge-mdx-scan-"+std::to_string(random())+"-"+std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate,ec)) { path = candidate; return; }
        }
        throw std::runtime_error("SCAN_TEMP_CREATE_FAILED");
    }
    ~Scratch() { std::error_code ec; if (!path.empty()) std::filesystem::remove_all(path,ec); }
};
}
int Run(const Options& options) {
    if (options.mode == "model") return OneModel(options);
    if (options.mode == "enumerate") { Enumerate(options); return 0; }
    auto fallbackPath = options.tsv; fallbackPath += ".fallback.txt";
    auto metadataPath = options.tsv; metadataPath += ".scope.txt";
    if (std::filesystem::exists(options.tsv) || std::filesystem::exists(fallbackPath) || std::filesystem::exists(metadataPath))
        throw std::runtime_error("SCAN_OUTPUT_ALREADY_EXISTS");
    const auto begin = Clock::now(), deadline = begin+std::chrono::seconds(options.limits.totalSeconds);
    Scratch scratch;
    const auto remaining = [&] {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        return static_cast<std::uint32_t>(std::max<std::int64_t>(0,ms));
    };
    const auto makeArgs = [&](const char* mode, const std::filesystem::path& output) {
        auto args = LimitArguments(options.limits);
        args.insert(args.end(),{"--internal",mode,"--tsv",Utf8(output)});
        if (!options.gameRoot.empty()) args.insert(args.end(),{"--game-root",Utf8(options.gameRoot)});
        return args;
    };
    const auto candidates = scratch.path / "candidates.txt";
    auto enumerate = makeArgs("enumerate",candidates);
    if (!options.list.empty()) enumerate.insert(enumerate.end(),{"--list",Utf8(options.list)});
    const auto enumerationError = RunChild(options,enumerate,std::min(remaining(),30000u));
    if (!enumerationError.empty()) throw std::runtime_error("SCAN_ENUMERATION_"+enumerationError);
    const auto paths = ReadLines(candidates,16ull*1024*1024,options.limits.maxModels);
    auto infoPath = candidates; infoPath += ".info";
    const auto enumerationInfo = ReadLines(infoPath,16ull*1024*1024,100000);
    std::string scopeWarnings;
    bool scopePartial = false;
    for (const auto& line : enumerationInfo) if (!line.starts_with("discovered=") && !line.starts_with("selected=")) {
        if (!scopeWarnings.empty()) scopeWarnings += ',';
        scopeWarnings += Cell(line);
        if (line != "SCAN_MPQ_LISTFILE_COVERAGE_ONLY") scopePartial = true;
    }
    std::ofstream out(options.tsv,std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("SCAN_OUTPUT_OPEN_FAILED");
    out << Header() << '\n';
    std::uint64_t parsed = 0, compiled = 0, rejected = 0, fallback = 0, unknownFallback = 0, incomplete = 0, mismatches = 0, samples = 0, particles = 0;
    std::uint64_t numericSamples = 0, numericModels = 0, allRejectedModels = 0;
    std::map<std::string,std::uint64_t> codes, states, sampleRejectionCodes, allRejectedCodes;
    std::string firstMismatch;
    std::ofstream fallbackOut(fallbackPath,std::ios::binary | std::ios::trunc);
    if (!fallbackOut) throw std::runtime_error("SCAN_OUTPUT_OPEN_FAILED");
    for (const auto& path : paths) {
        Record record; record.path = path;
        const auto resultPath = scratch.path / "model.tsv";
        std::error_code ignored; std::filesystem::remove(resultPath,ignored);
        if (!remaining()) record.state = "SCAN_TOTAL_TIME_LIMIT";
        else {
            auto args = makeArgs("model",resultPath); args.insert(args.end(),{"--model",path});
            const auto error = RunChild(options,args,std::min(remaining(),options.limits.modelMs+250u));
            try { record = ReadRecord(resultPath); }
            catch (const std::exception&) { record.state = "SCAN_CHILD_REPORT_INCOMPLETE"; }
            record.path = path;
            if (!error.empty()) record.state = error;
        }
        numericSamples += record.numericSamples;
        numericModels += record.numericSamples != 0;
        if (record.AllSamplesRejected()) { ++allRejectedModels; ++allRejectedCodes[record.PrimaryRejection()]; }
        for (const auto& [code,count] : record.rejectionCodes) sampleRejectionCodes[code] += count;
        parsed += record.parse == "OK"; compiled += record.compile == "OK"; particles += record.particleOnly;
        if (record.compile != "NOT_RUN" && record.compile != "OK") {
            ++rejected; ++codes[record.compile];
            if (record.FallbackPositive()) { ++fallback; fallbackOut << path << '\n'; }
            else if (record.state != "compile_rejected") ++unknownFallback;
        }
        if (record.state.starts_with("SCAN_") || record.state == "pending") ++incomplete;
        ++states[record.state]; mismatches += record.mismatchSamples; samples += record.samples;
        if (record.mismatchSamples && firstMismatch.empty()) firstMismatch = Cell(path)+":"+Cell(record.firstSample);
        out << Row(record) << '\n'; out.flush(); fallbackOut.flush();
        if (!out || !fallbackOut) throw std::runtime_error("SCAN_REPORT_WRITE_FAILED");
    }
    std::ofstream metadata(metadataPath,std::ios::binary | std::ios::trunc);
    for (const auto& line : enumerationInfo) metadata << line << '\n';
    metadata << "fallback_negative_means_sampled_only=1\nquality=PreviewApproximate\n";
    metadata << "tsv_columns=29\ndetail_max_bytes=8192\nparticle_only=" << particles << "\nparticle_rendering_implemented=0\n";
    metadata << "enumeration_partial=" << scopePartial << "\n";
    metadata << "memory_limit=" << options.limits.memoryMiB << "MiB/per-child\n";
    for (const auto& [state,count] : states) metadata << "state=" << state << " count=" << count << '\n';
    metadata.flush(); if (!metadata) throw std::runtime_error("SCAN_REPORT_WRITE_FAILED");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count();
    std::cout << "SCAN_SUMMARY models=" << paths.size() << " parsed=" << parsed << " compiled=" << compiled
        << " particle_only=" << particles << " compile_rejected=" << rejected << " legacy_positive_whole_rejected=" << fallback
        << " fallback_unknown=" << unknownFallback << " incomplete=" << incomplete << " samples=" << samples
        << " numeric_samples=" << numericSamples << " models_with_numeric=" << numericModels
        << " all_samples_rejected_models=" << allRejectedModels
        << " mismatch_samples=" << mismatches << " enumeration_partial=" << scopePartial << " scope_warnings=" << scopeWarnings << " elapsed_ms=" << elapsed << " compile_codes={";
    bool comma = false; for (const auto& [code,count] : codes) { if (comma) std::cout << ','; std::cout << code << ':' << count; comma = true; }
    const auto histogram = [&](const char* name,const auto& counts) {
        std::cout << "} " << name << "={";
        bool separator = false;
        for (const auto& [code,count] : counts) { if (separator) std::cout << ','; std::cout << code << ':' << count; separator = true; }
    };
    histogram("sample_rejection_codes",sampleRejectionCodes);
    histogram("all_rejected_model_codes",allRejectedCodes);
    std::cout << "} first_mismatch=" << firstMismatch << " tsv=" << Cell(Utf8(options.tsv)) << '\n';
    return mismatches ? 3 : (incomplete || scopePartial) ? 2 : 0;
}
}
