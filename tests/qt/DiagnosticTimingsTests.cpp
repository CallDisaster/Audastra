#include "../../src/QtCommon/DiagnosticTimings.h"
#include "../../src/QtCommon/OwnedWork.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <sstream>
#include <stdexcept>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace War3Map::QtCommon;
    std::ostringstream captured;
    auto* original = std::cerr.rdbuf(captured.rdbuf());
    bool passed = false;
    try {
        ConfigureDiagnosticTimings(true);
        StartDiagnosticHeartbeat(&app);
        {
            DiagnosticTimingScope open("open-total");
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        int ticks = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] { ++ticks; });
        timer.start(20);
        const auto ui = std::this_thread::get_id();
        if (RunOwnedWork([ui] { return std::this_thread::get_id() == ui; }) != true)
            throw std::runtime_error("default dispatch changed");
        {
            OwnedWorkScope owned(true);
            std::string live = "frozen";
            QTimer::singleShot(0, &app, [&live] { live = "late edit"; });
            const auto copied = RunOwnedWork([snapshot = live, ui] {
                if (std::this_thread::get_id() == ui) throw std::runtime_error("not a worker");
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                return snapshot;
            });
            if (copied != "frozen" || live != "late edit") throw std::runtime_error("owned snapshot changed");
            bool propagated = false;
            try { (void)RunOwnedWork([]() -> int { throw std::runtime_error("fixture"); }); }
            catch (const std::runtime_error&) { propagated = true; }
            if (!propagated || !RunOwnedWork([ui] { return std::this_thread::get_id() == ui; }, false))
                throw std::runtime_error("failure or ineligible dispatch differs");
        }
        if (!RunOwnedWork([ui] { return std::this_thread::get_id() == ui; }))
            throw std::runtime_error("dispatch scope leaked");
        {
            DiagnosticTimingScope save("save-total");
            QEventLoop loop;
            std::thread worker([] {
                DiagnosticTimingScope scope("worker-fixture");
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            });
            QTimer::singleShot(180, &loop, &QEventLoop::quit); loop.exec();
            worker.join();
        }
        const auto text = captured.str();
        const auto summary = text.find("|operation=open-total|max_gap_us=");
        if (summary == std::string::npos) throw std::runtime_error("no UI gap summary");
        const auto number = summary + std::string("|operation=open-total|max_gap_us=").size();
        if (std::stoll(text.substr(number)) < 100000 || ticks < 2
            || text.find("|worker|begin/worker-fixture") == std::string::npos
            || text.find("|ui|begin/open-total") == std::string::npos)
            throw std::runtime_error("thread or heartbeat observation differs");
        ConfigureDiagnosticTimings(false);
        DiagnosticTimingEvent("disabled-fixture");
        if (captured.str() != text) throw std::runtime_error("disabled diagnostics emitted output");
        passed = true;
    } catch (...) { ConfigureDiagnosticTimings(false); }
    std::cerr.rdbuf(original);
    if (!passed) { std::cerr << "diagnostic heartbeat contract failed\n"; return 1; }
    return 0;
}
