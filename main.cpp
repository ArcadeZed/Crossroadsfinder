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
#include <string>

// Dear ImGui & GLFW
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

extern "C" {
#include "finders.h"
#include "generator.h"
}

struct ClusterResult {
    int centerX, centerY, centerZ;
    long long distSq;

    bool operator==(const ClusterResult& other) const {
        return centerX == other.centerX && centerY == other.centerY && centerZ == other.centerZ;
    }
};

std::atomic<long long> regionsProcessed(0);
std::atomic<bool> isSearching(false);
std::atomic<bool> isPaused(false);
std::atomic<int> currentRxIndex(0);
std::mutex mergeMutex;
std::vector<ClusterResult> allResults;
long long totalRegionsToProcess = 1;
auto startTime = std::chrono::steady_clock::now();
int threadsToUse = std::thread::hardware_concurrency();
bool lowPriorityMode = true;
std::atomic<long long> regionsAtSessionStart(0);

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

void saveSettings(const char* seed, int radius, int progressIndex) {
    std::ofstream f("settings.cfg");
    if (f.is_open()) {
        f << seed << "\n" << radius << "\n" << progressIndex << "\n";
        f << threadsToUse << "\n" << (lowPriorityMode ? 1 : 0) << "\n";
        f.close();
    }
}

void loadSettings(char* seedBuf, int& radius, int& progressIndex) {
    std::ifstream f("settings.cfg");
    if (f.is_open()) {
        std::string line;
        if (std::getline(f, line)) { strncpy(seedBuf, line.c_str(), 63); seedBuf[63] = '\0'; }
        if (std::getline(f, line)) radius = std::stoi(line);
        if (std::getline(f, line)) progressIndex = std::stoi(line);
        if (std::getline(f, line)) threadsToUse = std::stoi(line);
        if (std::getline(f, line)) lowPriorityMode = (std::stoi(line) == 1);
        f.close();
    }
}

void saveResults() {
    std::lock_guard<std::mutex> lock(mergeMutex);
    std::ofstream f("results.dat");
    if (f.is_open()) {
        for (const auto& r : allResults) {
            f << r.centerX << " " << r.centerY << " " << r.centerZ << " " << r.distSq << "\n";
        }
        f.close();
    }
}

void loadResults() {
    std::ifstream f("results.dat");
    if (f.is_open()) {
        std::lock_guard<std::mutex> lock(mergeMutex);
        allResults.clear();
        ClusterResult r;
        while (f >> r.centerX >> r.centerY >> r.centerZ >> r.distSq) {
            allResults.push_back(r);
        }
        f.close();
    }
}

#ifdef _WIN32
#include <windows.h>
#endif
void searchSector(int64_t seed, int mc_version, int radius) {
    if (lowPriorityMode) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
    Generator g;
    setupGenerator(&g, mc_version, 0);
    applySeed(&g, -1, (uint64_t)seed);

    Piece pieces[1000];
    struct SimplePos { int x, y, z; };
    SimplePos crossings[128];

    int startRx = -radius;
    int endRx = radius;
    int sideLen = (2 * radius) + 1;

    while (isSearching) {
        while (isPaused && isSearching) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        int jobIndex = currentRxIndex.fetch_add(1);
        if (lowPriorityMode) {
            std::this_thread::sleep_for(std::chrono::microseconds(100)); // 0.1 Millisekunden Pause
        }
        int rx = startRx + jobIndex;

        if (rx > endRx || !isSearching) break;

        std::vector<ClusterResult> columnResults;
        for (int rz = -radius; rz <= radius; rz++) {
            Pos pos;
            if (!getStructurePos(Fortress, mc_version, (uint64_t)seed, rx, rz, &pos)) continue;
            if (!isViableStructurePos(Fortress, &g, pos.x, pos.z, 0)) continue;

            int count = getFortressPieces(pieces, 1000, mc_version, (uint64_t)seed, pos.x >> 4, pos.z >> 4);
            if (count < 4) continue;

            int crossCount = 0;
            for (int i = 0; i < count && crossCount < 128; i++) {
                if (pieces[i].type == BRIDGE_CROSSING) {
                    crossings[crossCount++] = {pieces[i].pos.x, pieces[i].pos.y, pieces[i].pos.z};
                }
            }
            if (crossCount < 4) continue;

            for (int i = 0; i < crossCount; i++) {
                bool hasRight = false, hasBottom = false, hasDiagonal = false;
                int x = crossings[i].x, y = crossings[i].y, z = crossings[i].z;
                for (int j = 0; j < crossCount; j++) {
                    if (crossings[j].y != y) continue;
                    int dx = crossings[j].x - x, dz = crossings[j].z - z;
                    if (dx == 19 && dz == 0) hasRight = true;
                    else if (dx == 0 && dz == 19) hasBottom = true;
                    else if (dx == 19 && dz == 19) hasDiagonal = true;
                    if (hasRight && hasBottom && hasDiagonal) break;
                }
                if (hasRight && hasBottom && hasDiagonal) {
                    columnResults.push_back({x + 9, y, z + 9, (long long)x*x + (long long)z*z});
                    break;
                }
            }
        }

        regionsProcessed.fetch_add(sideLen, std::memory_order_relaxed);

        if (!columnResults.empty()) {
            std::lock_guard<std::mutex> lock(mergeMutex);
            allResults.insert(allResults.end(), columnResults.begin(), columnResults.end());
        }
    }
}

void runSearchManager(int64_t seed, int radius) {
    int sideLen = (2 * radius) + 1;
    totalRegionsToProcess = (long long)sideLen * sideLen;
    regionsProcessed = (long long)currentRxIndex.load() * sideLen;

    regionsAtSessionStart = regionsProcessed.load();

    startTime = std::chrono::steady_clock::now();

    unsigned int tc = (unsigned int)threadsToUse;
    if (tc < 1) tc = 1;

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < tc; i++) {
        threads.emplace_back(searchSector, seed, MC_1_21, radius);
    }

    for (auto& t : threads) t.join();

    std::lock_guard<std::mutex> lock(mergeMutex);
    std::sort(allResults.begin(), allResults.end(), [](const ClusterResult& a, const ClusterResult& b) {
        if (a.distSq != b.distSq) return a.distSq < b.distSq;
        return a.centerX < b.centerX;
    });
    allResults.erase(std::unique(allResults.begin(), allResults.end()), allResults.end());

    isSearching = false;
}

int main() {
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(1100, 750, "Ultimate Quad Finder 1.21", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    char seedBuf[64] = "0";
    int radiusInput = 15000;
    int progressIdx = 0;
    bool seedError = false;

    loadSettings(seedBuf, radiusInput, progressIdx);
    currentRxIndex = progressIdx;
    loadResults();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. SETTINGS WINDOW
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_Always);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        bool disableInputs = isSearching || (currentRxIndex > 0);

        if (disableInputs) {
            ImGui::BeginDisabled();
        }

        ImGui::InputText("Seed", seedBuf, 64, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsNoBlank);

        int radiusStep = 1;
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && io.KeyShift) radiusStep = 1000;
        else if (io.KeyCtrl) radiusStep = 100;
        else if (io.KeyShift) radiusStep = 10;

        ImGui::InputInt("Radius", &radiusInput, radiusStep, 0);
        if (radiusInput < 1) radiusInput = 1;
        long long sideLen = (2LL * radiusInput) + 1;
        ImGui::Text("Total Regions: %lld", sideLen * sideLen);
        ImGui::TextDisabled("(?) Shift=+10, Ctrl=+100, Ctrl+Shift=+1000");

        if (disableInputs) {
            ImGui::EndDisabled();
            if (currentRxIndex > 0 && !isSearching) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Reset progress to change Seed/Radius!");
            }
        }


        ImGui::Separator();
        ImGui::Text("Performance CPU:");
        ImGui::BeginDisabled(isSearching);
        int maxThreads = std::thread::hardware_concurrency();
        if (threadsToUse < 1) threadsToUse = 1;
        if (threadsToUse > maxThreads) threadsToUse = maxThreads;
        ImGui::SliderInt("Threads", &threadsToUse, 1, maxThreads);
        ImGui::Checkbox("Background Mode (Low Priority)", &lowPriorityMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sorgt dafür, dass der PC flüssig bleibt,\nindem die Suche anderen Programmen den Vorrang lässt.");
        ImGui::EndDisabled();

        if (isSearching && !isPaused) {
            ImGui::TextDisabled("(Stop search to change CPU settings)");
        }

        ImGui::Separator();


        ImGui::Separator();

        // Buttons
        if (!isSearching) {
            const char* btnLabel = (currentRxIndex > 0) ? "Resume Search" : "Start New Search";
            if (ImGui::Button(btnLabel, ImVec2(185, 40))) {
                try {
                    // Prüfen, ob das Feld leer ist
                    if (strlen(seedBuf) == 0) {
                        seedError = true;
                    } else {
                        int64_t s = std::stoll(seedBuf); // Versuch der Umwandlung
                        seedError = false;               // Alles okay
                        isSearching = true;
                        isPaused = false;
                        std::thread(runSearchManager, s, radiusInput).detach();
                    }
                } catch (const std::exception& e) {
                    // Falls stoll trotzdem scheitert (z.B. Zahl zu groß für long long)
                    seedError = true;
                }
            }
            if (seedError) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: Invalid Seed! Please enter a number.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Progress", ImVec2(185, 40))) {
                currentRxIndex = 0;
                regionsProcessed = 0;
                {
                    std::lock_guard<std::mutex> lock(mergeMutex);
                    allResults.clear();
                }
                std::remove("results.dat");
                std::remove("settings.cfg");
            }
        } else {
            if (ImGui::Button(isPaused ? "RESUME" : "PAUSE", ImVec2(185, 40))) isPaused = !isPaused;
            ImGui::SameLine();
            if (ImGui::Button("STOP & SAVE", ImVec2(185, 40))) isSearching = false;
        }

        // Progress & ETA
        if (isSearching || currentRxIndex > 0) {
            float prog = (float)currentRxIndex.load() / (float)sideLen;
            ImGui::ProgressBar(prog, ImVec2(-1, 0));

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (elapsed > 0 && isSearching && !isPaused) {
                long long currentTotal = regionsProcessed.load();

                long long processedInSession = currentTotal - regionsAtSessionStart.load();

                long long speed = processedInSession / elapsed;

                long long remainingTotal = (sideLen * sideLen) - currentTotal;

                if (speed > 0) {
                    if (elapsed > 5 && processedInSession > 5000) {
                        long long secondsLeft = remainingTotal / speed;
                        ImGui::Text("Speed: %lld reg/s | ETA: %s", speed, formatTime(secondsLeft).c_str());
                    } else {
                        ImGui::Text("Calculating ETA...");
                    }
                } else {
                    ImGui::Text("Speed: %lld reg/s | ETA: --:--:--", speed);
                }
            }
        }
        ImGui::End();

        // 2. RESULTS WINDOW
        ImGui::SetNextWindowPos(ImVec2(420, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(660, 730), ImGuiCond_Always);
        ImGui::Begin("Results Found", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        if (ImGui::BeginTable("ResTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Dist", 0, 80);
            ImGui::TableSetupColumn("Teleport Command", 0, 400);
            ImGui::TableSetupColumn("Copy", 0, 80);
            ImGui::TableHeadersRow();

            std::lock_guard<std::mutex> lock(mergeMutex);
            for (int i = 0; i < (int)allResults.size(); i++) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", (int)std::sqrt(allResults[i].distSq));

                ImGui::TableSetColumnIndex(1);
                char cmd[128];
                snprintf(cmd, 128, "/tp %d %d %d", allResults[i].centerX, allResults[i].centerY + 2, allResults[i].centerZ);
                ImGui::Text("%s", cmd);

                ImGui::TableSetColumnIndex(2);

                char btnLabel[32];
                snprintf(btnLabel, sizeof(btnLabel), "Copy##btn_%d", i);

                if (ImGui::SmallButton(btnLabel)) {
                    ImGui::SetClipboardText(cmd);
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();

        static auto lastS = std::chrono::steady_clock::now();
        if (isSearching && std::chrono::steady_clock::now() - lastS > std::chrono::seconds(5)) {
            saveSettings(seedBuf, radiusInput, currentRxIndex.load());
            saveResults();
            lastS = std::chrono::steady_clock::now();
        }

        ImGui::Render();
        int dw, dh; glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh); glClearColor(0.1f, 0.1f, 0.12f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    saveSettings(seedBuf, radiusInput, currentRxIndex.load());
    saveResults();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return 0;
}