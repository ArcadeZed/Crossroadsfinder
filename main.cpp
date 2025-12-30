#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cmath>

extern "C" {
#include "finders.h"
#include "generator.h"
}

struct ClusterResult {
    int centerX, centerY, centerZ;
    long long distSq; // Distance squared for faster sorting
};

// Global progress tracking and results storage
std::atomic<long long> regionsProcessed(0);
std::mutex mergeMutex;
std::vector<ClusterResult> allResults;

/**
 * Worker function to search a specific sector of the world for Quad Fortresses.
 */
void searchSector(int64_t seed, int mc_version, int rxStart, int rxEnd, int radius) {
    Generator g;
    setupGenerator(&g, mc_version, 0);
    applySeed(&g, -1, (uint64_t)seed);

    std::vector<ClusterResult> localResults;
    Piece pieces[1000];

    // Small buffer for filtered crossings to avoid heavy heap allocation
    struct SimplePos { int x, y, z; };
    SimplePos crossings[128];

    long long localCounter = 0;

    for (int rx = rxStart; rx < rxEnd; rx++) {
        for (int rz = -radius; rz <= radius; rz++) {
            // Update global atomic counter periodically
            if (++localCounter >= 1024) {
                regionsProcessed.fetch_add(localCounter, std::memory_order_relaxed);
                localCounter = 0;
            }

            Pos pos;
            // Get the theoretical structure position based on the grid
            if (!getStructurePos(Fortress, mc_version, (uint64_t)seed, rx, rz, &pos))
                continue;

            // In 1.16+, biomes decide if a Fortress or a Bastion spawns.
            // isViableStructurePos checks if the biome allows a Fortress here.
            if (!isViableStructurePos(Fortress, &g, pos.x, pos.z, 0))
                continue;

            // Generate the structure pieces
            int count = getFortressPieces(pieces, 1000, mc_version, (uint64_t)seed, pos.x >> 4, pos.z >> 4);
            if (count < 4) continue;

            // --- OPTIMIZED QUAD SEARCH ---
            // First, filter for bridge crossings to reduce the search space
            int crossCount = 0;
            for (int i = 0; i < count && crossCount < 128; i++) {
                if (pieces[i].type == BRIDGE_CROSSING) {
                    crossings[crossCount++] = {pieces[i].pos.x, pieces[i].pos.y, pieces[i].pos.z};
                }
            }

            if (crossCount < 4) continue;

            // Search for a 19x19 square alignment of four crossings (a "Quad")
            for (int i = 0; i < crossCount; i++) {
                bool hasRight = false, hasBottom = false, hasDiagonal = false;
                int x = crossings[i].x;
                int y = crossings[i].y;
                int z = crossings[i].z;

                for (int j = 0; j < crossCount; j++) {
                    if (crossings[j].y != y) continue;

                    int dx = crossings[j].x - x;
                    int dz = crossings[j].z - z;

                    // Standard Fortress crossing offset is 19 blocks
                    if (dx == 19 && dz == 0) hasRight = true;
                    else if (dx == 0 && dz == 19) hasBottom = true;
                    else if (dx == 19 && dz == 19) hasDiagonal = true;

                    if (hasRight && hasBottom && hasDiagonal) break;
                }

                if (hasRight && hasBottom && hasDiagonal) {
                    // Center of the quad is roughly +9 blocks from the top-left crossing
                    localResults.push_back({x + 9, y, z + 9, (long long)x*x + (long long)z*z});
                    break;
                }
            }
        }
    }
    regionsProcessed.fetch_add(localCounter, std::memory_order_relaxed);

    if (!localResults.empty()) {
        std::lock_guard<std::mutex> lock(mergeMutex);
        allResults.insert(allResults.end(), localResults.begin(), localResults.end());
    }
}

/**
 * Formats seconds into a HH:MM:SS string
 */
std::string formatTime(long long totalSeconds) {
    long long h = totalSeconds / 3600;
    long long m = (totalSeconds % 3600) / 60;
    long long s = totalSeconds % 60;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", h, m, s);
    return std::string(buffer);
}

int main() {
    int64_t seed;
    int radius = 15000;
    int mc_version = MC_1_21;

    std::cout << "=== ULTIMATE QUAD FINDER 1.21 ===\n";
    std::cout << "Enter world seed: ";
    if (!(std::cin >> seed)) {
        std::cerr << "Invalid seed input.\n";
        return 1;
    }

    auto startTime = std::chrono::steady_clock::now();
    unsigned int tc = std::thread::hardware_concurrency();
    if (tc == 0) tc = 4; // Fallback

    long long rangeX = (2LL * radius) + 1;
    long long total = rangeX * rangeX;

    std::cout << "Searching in radius: " << radius << " (approx. " << total << " regions)\n";
    std::cout << "Using " << tc << " threads...\n\n";

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < tc; i++) {
        int startRx = -radius + (int)(i * rangeX / tc);
        int endRx = -radius + (int)((i + 1) * rangeX / tc);
        threads.emplace_back(searchSector, seed, mc_version, startRx, endRx, radius);
    }

    // Progress monitoring loop
    while (regionsProcessed.load() < total) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        long long current = regionsProcessed.load();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();

        if (elapsed <= 0) continue;

        double pct = (current * 100.0) / total;
        long long regPerSec = current / elapsed;
        long long secondsLeft = (regPerSec > 0) ? (total - current) / regPerSec : 0;

        std::cout << "\rProgress: " << std::fixed << std::setprecision(2) << pct << "% | "
                  << "Speed: " << regPerSec << " reg/s | "
                  << "ETA: " << formatTime(secondsLeft) << " | "
                  << "Quads found: " << allResults.size() << "    " << std::flush;

        if (current >= total) break;
    }

    for (auto& t : threads) t.join();

    // Sort results by distance to (0,0)
    std::sort(allResults.begin(), allResults.end(), [](const ClusterResult& a, const ClusterResult& b) {
        return a.distSq < b.distSq;
    });

    // Write results to file
    std::ofstream f("quads_found.txt");
    f << "# Quad Crossroads for Seed: " << seed << " (MC 1.21)\n";
    for (const auto& r : allResults) {
        f << "QUAD | Dist: " << (int)std::sqrt(r.distSq)
          << " | /tp " << r.centerX << " " << r.centerY + 2 << " " << r.centerZ << "\n";
    }
    f.close();

    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    std::cout << "\n\nSearch finished! Total time: " << formatTime(totalElapsed) << "\n";
    std::cout << "Results saved to quads_found.txt\n";

    return 0;
}