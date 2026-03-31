// Quick diagnostic for D4Root enrichment.
#include <whiteout/storages/casc/storage.h>
#include <iostream>
#include <string>

using namespace whiteout::storages::casc;

int main() {
    auto s = Storage::open("C:\\Program Files (x86)\\Diablo IV");
    if (!s) { std::cerr << "open failed err=" << Storage::lastError() << "\n"; return 1; }

    auto prod = s->product();
    if (prod) std::cout << "Product: " << prod->name << " (" << prod->version << ")\n";

    size_t n = 0, enriched = 0, withId = 0, numeric = 0;
    std::vector<std::string> tocPaths;
    s->enumerate([&](const FindEntry& fe) {
        ++n;
        if (fe.fileDataId != 0 && fe.fileDataId != 0xFFFFFFFF) ++withId;

        // Find CoreTOC entries
        if (fe.path.find("CoreTOC") != std::string::npos ||
            fe.path.find("coretoc") != std::string::npos ||
            fe.path.find("CORETOC") != std::string::npos) {
            tocPaths.push_back(fe.path);
        }

        if (!fe.path.empty() && fe.path.find('/') != std::string::npos) {
            auto last = fe.path.rfind('/');
            auto fn = fe.path.substr(last + 1);
            // Check if the filename starts with a letter (enriched) vs digit (numeric).
            if (!fn.empty()) {
                if (std::isalpha(static_cast<unsigned char>(fn[0]))) {
                    ++enriched;
                    if (enriched <= 10)
                        std::cout << "  ENRICHED: " << fe.path
                                  << " (id=" << fe.fileDataId << ")\n";
                } else if (std::isdigit(static_cast<unsigned char>(fn[0]))) {
                    ++numeric;
                    if (numeric <= 5)
                        std::cout << "  NUMERIC:  " << fe.path
                                  << " (id=" << fe.fileDataId << ")\n";
                }
            }
        }
        return true;
    });

    std::cout << "\nTotal:    " << n << " entries\n";
    std::cout << "Enriched: " << enriched << " (human-readable paths)\n";
    std::cout << "Numeric:  " << numeric << " (still numeric)\n";
    std::cout << "With ID:  " << withId << " (have fileDataId)\n";

    // CoreTOC discovery
    std::cout << "\nCoreTOC entries found: " << tocPaths.size() << "\n";
    for (auto& p : tocPaths)
        std::cout << "  " << p << "\n";

    // Try all known CoreTOC path variants
    std::cout << "\n--- CoreTOC read attempts ---\n";
    for (auto& path : {"Base/CoreTOC.dat", "base/coretoc.dat",
                        "Base\\CoreTOC.dat", "base\\coretoc.dat",
                        "Base:CoreTOC.dat", "base:coretoc.dat"}) {
        auto data = s->readFile(path);
        std::cout << "  " << path << ": "
                  << (data ? std::to_string(data->size()) + " bytes" : "FAILED")
                  << "\n";
    }
    // Also try reading CoreTOC if it appeared in enumerate
    for (auto& p : tocPaths) {
        auto data = s->readFile(p);
        std::cout << "  (enum) " << p << ": "
                  << (data ? std::to_string(data->size()) + " bytes" : "FAILED")
                  << "\n";
    }

    // Show first 20 entries to understand path format
    std::cout << "\n--- First 30 entries ---\n";
    size_t show = 0;
    s->enumerate([&](const FindEntry& fe) {
        if (show < 30) {
            std::cout << "  [" << show << "] " << fe.path
                      << " (size=" << fe.fileSize << ")\n";
        }
        ++show;
        return show < 30;
    });

    // Test readFile by SNO ID.
    std::cout << "\n--- readFile by SNO ID ---\n";
    // Try a few known SNO IDs.
    for (int id : {197860, 100000, 200000, 300000}) {
        auto data = s->readFile(id);
        if (data)
            std::cout << "  readFile(" << id << "): " << data->size() << " bytes\n";
        else
            std::cout << "  readFile(" << id << "): failed (err=" << Storage::lastError() << ")\n";
    }

    // Test findByPath with enriched path.
    std::cout << "\n--- findByPath with enriched path ---\n";
    auto files = s->listFiles();
    size_t found = 0;
    for (size_t i = 0; i < files.size() && found < 3; ++i) {
        if (files[i].find('/') == std::string::npos) continue;
        auto last = files[i].rfind('/');
        auto fn = files[i].substr(last + 1);
        if (!fn.empty() && std::isalpha(static_cast<unsigned char>(fn[0]))) {
            auto data = s->readFile(files[i]);
            std::cout << "  readFile(\"" << files[i] << "\"): "
                      << (data ? std::to_string(data->size()) + " bytes" : "failed")
                      << "\n";
            ++found;
        }
    }

    return 0;
}
