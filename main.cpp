#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
#include <map>

// Dear ImGui & GLFW
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

extern "C" {
#include "finders.h"
#include "generator.h"
}

// Muster-Typen
enum PatternType {
    PAT_QUAD = 0, PAT_T_SHAPE = 1, PAT_L_SHAPE = 2, PAT_TRIPLE = 3, PAT_COUNT = 4
};
const char* PATTERN_NAMES[] = { "Quads (2x2)", "T-Shapes", "L-Shapes", "Triple Lines" };

struct ClusterResult {
    long long seed;
    int centerX, centerY, centerZ;
    long long distSq;
    int patternType;
    bool isPermanent;

    bool operator==(const ClusterResult& other) const {
        return seed == other.seed && centerX == other.centerX && centerY == other.centerY &&
               centerZ == other.centerZ && patternType == other.patternType;
    }
};

// --- GLOBALE VARIABLEN ---
enum SearchMode { MODE_SINGLE_SCAN, MODE_SEED_FINDER };
SearchMode currentMode = MODE_SINGLE_SCAN;

std::atomic<bool> isSearching(false);
std::atomic<bool> isPaused(false);
std::mutex mergeMutex;
std::vector<ClusterResult> allResults;

std::atomic<int> currentRxIndex(0);
int singleScanRadius = 15000;
char singleSeedBuf[64] = "0";

std::atomic<long long> nextSeedToCheck(0);
std::atomic<long long> seedsCheckedThisSession(0);
int finderRadiusBlocks = 1000;

int threadsToUse = std::thread::hardware_concurrency();
bool lowPriorityMode = true;

// UI State für Lazy Loading
int displayLimits[PAT_COUNT];

// --- PERSISTENZ ---

void saveData() {
    std::lock_guard<std::mutex> lock(mergeMutex);
    std::ofstream fs("config.cfg");
    if (fs.is_open()) {
        fs << nextSeedToCheck.load() << "\n" << singleScanRadius << "\n" << singleSeedBuf << "\n"
           << threadsToUse << "\n" << (lowPriorityMode ? 1 : 0) << "\n" << finderRadiusBlocks << "\n";
        fs.close();
    }
    std::ofstream fr("results.dat");
    if (fr.is_open()) {
        for (const auto& r : allResults) {
            fr << r.seed << " " << r.centerX << " " << r.centerY << " " << r.centerZ << " " << r.distSq << " " << r.patternType << " " << (r.isPermanent ? 1 : 0) << "\n";
        }
        fr.close();
    }
}

void loadData() {
    std::ifstream fs("config.cfg");
    if (fs.is_open()) {
        long long nstc; fs >> nstc; nextSeedToCheck = nstc;
        fs >> singleScanRadius >> singleSeedBuf >> threadsToUse;
        int lpm; fs >> lpm; lowPriorityMode = (lpm == 1);
        fs >> finderRadiusBlocks;
        fs.close();
    }
    std::ifstream fr("results.dat");
    if (fr.is_open()) {
        ClusterResult r;
        int permInt;
        while (fr >> r.seed >> r.centerX >> r.centerY >> r.centerZ >> r.distSq >> r.patternType >> permInt) {
            r.isPermanent = (permInt == 1);
            allResults.push_back(r);
        }
        fr.close();
    }
}

// --- LOGIK ---

void checkPatternsInRegion(Generator* g, long long seed, int rx, int rz, std::vector<ClusterResult>& found, bool makePermanent) {
    int mc = MC_1_21;
    Pos pos;
    if (!getStructurePos(Fortress, mc, (uint64_t)seed, rx, rz, &pos)) return;
    if (!isViableStructurePos(Fortress, g, pos.x, pos.z, 0)) return;

    Piece pieces[1000];
    int count = getFortressPieces(pieces, 1000, mc, (uint64_t)seed, pos.x >> 4, pos.z >> 4);
    if (count < 3) return;

    struct CP { int x, y, z; };
    std::vector<CP> crosses;
    for (int i = 0; i < count; i++) {
        if (pieces[i].type == BRIDGE_CROSSING) crosses.push_back({pieces[i].pos.x, pieces[i].pos.y, pieces[i].pos.z});
    }

    for (size_t i = 0; i < crosses.size(); i++) {
        int x = crosses[i].x, y = crosses[i].y, z = crosses[i].z;
        bool hasR1 = false, hasR2 = false, hasD1 = false, hasD2 = false, hasDR = false;
        for (size_t j = 0; j < crosses.size(); j++) {
            if (crosses[j].y != y) continue;
            int dx = crosses[j].x - x; int dz = crosses[j].z - z;
            if (dx == 19 && dz == 0) hasR1 = true;
            if (dx == 38 && dz == 0) hasR2 = true;
            if (dx == 0 && dz == 19) hasD1 = true;
            if (dx == 0 && dz == 38) hasD2 = true;
            if (dx == 19 && dz == 19) hasDR = true;
        }
        ClusterResult res;
        res.seed = seed; res.centerX = x + 9; res.centerY = y; res.centerZ = z + 9;
        res.distSq = (long long)x*x + (long long)z*z;
        res.isPermanent = makePermanent;
        if (hasR1 && hasD1 && hasDR) res.patternType = PAT_QUAD;
        else if (hasR1 && hasR2 && hasD1) res.patternType = PAT_T_SHAPE;
        else if (hasR1 && hasD1) res.patternType = PAT_L_SHAPE;
        else if ((hasR1 && hasR2) || (hasD1 && hasD2)) res.patternType = PAT_TRIPLE;
        else continue;
        found.push_back(res);
    }
}

void workerThread() {
#ifdef _WIN32
    if (lowPriorityMode) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    Generator g; setupGenerator(&g, MC_1_21, 0);
    while (isSearching) {
        if (isPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        std::vector<ClusterResult> localFound;
        if (currentMode == MODE_SINGLE_SCAN) {
            try {
                long long s = std::stoll(singleSeedBuf); applySeed(&g, -1, (uint64_t)s);
                int rx = -singleScanRadius + currentRxIndex.fetch_add(1);
                if (rx > singleScanRadius) { isSearching = false; break; }
                for (int rz = -singleScanRadius; rz <= singleScanRadius; rz++) checkPatternsInRegion(&g, s, rx, rz, localFound, false);
            } catch (...) { isSearching = false; break; }
        } else {
            long long s = nextSeedToCheck.fetch_add(1); seedsCheckedThisSession++;
            applySeed(&g, -1, (uint64_t)s);
            int rRad = (finderRadiusBlocks / 432) + 1;
            for (int rx = -rRad; rx <= rRad; rx++)
                for (int rz = -rRad; rz <= rRad; rz++) checkPatternsInRegion(&g, s, rx, rz, localFound, true);
        }
        if (!localFound.empty()) {
            std::lock_guard<std::mutex> lock(mergeMutex);
            for (const auto& lf : localFound)
                if (std::find(allResults.begin(), allResults.end(), lf) == allResults.end()) allResults.push_back(lf);
        }
    }
}

int main() {
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Minecraft Fortress Finder 1.21", NULL, NULL);
    glfwMakeContextCurrent(window); glfwSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 130");

    for (int i = 0; i < PAT_COUNT; i++) displayLimits[i] = 20;
    loadData();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        int stepSize = (io.KeyShift && io.KeyCtrl) ? 1000 : (io.KeyCtrl ? 100 : (io.KeyShift ? 10 : 1));

        // --- Settings ---
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 420), ImGuiCond_Always);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoResize);
        if (ImGui::BeginTabBar("ModeTabs")) {
            if (ImGui::BeginTabItem("Single Scan")) {
                currentMode = MODE_SINGLE_SCAN;
                ImGui::InputText("Seed", singleSeedBuf, 64, ImGuiInputTextFlags_CharsDecimal);
                ImGui::InputInt("Radius", &singleScanRadius, stepSize);
                long long side = (2LL * singleScanRadius) + 1;
                ImGui::Text("Area: %lldx%lld Regions (%lld total)", side, side, side*side);
                ImGui::ProgressBar((float)currentRxIndex.load() / side);
                if (ImGui::Button("Reset Progress")) currentRxIndex = 0;
                ImGui::SameLine();
                if (ImGui::Button("Clear Scan Results")) {
                    std::lock_guard<std::mutex> lock(mergeMutex);
                    allResults.erase(std::remove_if(allResults.begin(), allResults.end(), [](const ClusterResult& r){ return !r.isPermanent; }), allResults.end());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Seed Finder")) {
                currentMode = MODE_SEED_FINDER;
                ImGui::Text("Next Seed: %lld", nextSeedToCheck.load());
                ImGui::InputInt("Origin Max Dist", &finderRadiusBlocks, stepSize);
                ImGui::Text("Checked this session: %lld", seedsCheckedThisSession.load());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::SliderInt("Threads", &threadsToUse, 1, std::thread::hardware_concurrency());
        ImGui::Checkbox("Low Priority Mode", &lowPriorityMode);
        if (!isSearching) {
            if (ImGui::Button("START", ImVec2(-1, 40))) {
                isSearching = true; isPaused = false; seedsCheckedThisSession = 0;
                for (int i = 0; i < threadsToUse; i++) std::thread(workerThread).detach();
            }
        } else {
            if (ImGui::Button(isPaused ? "RESUME" : "PAUSE", ImVec2(190, 40))) isPaused = !isPaused;
            ImGui::SameLine();
            if (ImGui::Button("STOP", ImVec2(190, 40))) { isSearching = false; saveData(); }
        }
        ImGui::End();

        // --- Results ---
        ImGui::SetNextWindowPos(ImVec2(420, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(840, 750), ImGuiCond_Always);
        ImGui::Begin("Results List", nullptr, ImGuiWindowFlags_NoResize);
        if (ImGui::BeginTabBar("ResultTabs")) {
            for (int p = 0; p < PAT_COUNT; p++) {
                if (ImGui::BeginTabItem(PATTERN_NAMES[p])) {
                    ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
                    if (ImGui::BeginTable("Table", 4, tableFlags, ImVec2(0, 630))) {
                        ImGui::TableSetupColumn("Seed", ImGuiTableColumnFlags_None, 160);
                        ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_DefaultSort, 60);
                        ImGui::TableSetupColumn("TP Command", ImGuiTableColumnFlags_None, 240);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_None, 180);
                        ImGui::TableHeadersRow();

                        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                            if (sortSpecs->SpecsDirty) {
                                std::lock_guard<std::mutex> lock(mergeMutex);
                                bool asc = (sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending);
                                std::sort(allResults.begin(), allResults.end(), [asc](const ClusterResult& a, const ClusterResult& b) {
                                    return asc ? (a.distSq < b.distSq) : (a.distSq > b.distSq);
                                });
                                sortSpecs->SpecsDirty = false;
                            }
                        }

                        std::lock_guard<std::mutex> lock(mergeMutex);
                        int shownCount = 0;
                        int totalInType = 0;
                        for (int i = 0; i < (int)allResults.size(); i++) {
                            if (allResults[i].patternType != p) continue;
                            totalInType++;
                            if (shownCount >= displayLimits[p]) continue;

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            if(allResults[i].isPermanent) ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%lld", allResults[i].seed);
                            else ImGui::Text("%lld", allResults[i].seed);
                            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", (int)std::sqrt(allResults[i].distSq));
                            ImGui::TableSetColumnIndex(2);
                            char cmd[128]; snprintf(cmd, 128, "/tp %d %d %d", allResults[i].centerX, allResults[i].centerY+2, allResults[i].centerZ);
                            ImGui::Text("%s", cmd);
                            ImGui::TableSetColumnIndex(3);
                            char sBtn[32], tBtn[32], dBtn[32];
                            snprintf(sBtn, 32, "Seed##%d_%d", p, i); snprintf(tBtn, 32, "TP##%d_%d", p, i); snprintf(dBtn, 32, "Del##%d_%d", p, i);
                            if (ImGui::SmallButton(sBtn)) ImGui::SetClipboardText(std::to_string(allResults[i].seed).c_str());
                            ImGui::SameLine();
                            if (ImGui::SmallButton(tBtn)) ImGui::SetClipboardText(cmd);
                            if (!allResults[i].isPermanent) {
                                ImGui::SameLine();
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
                                if (ImGui::SmallButton(dBtn)) allResults.erase(allResults.begin() + i);
                                ImGui::PopStyleColor();
                            }
                            shownCount++;
                        }
                        ImGui::EndTable();

                        // Load More Logic
                        ImGui::Separator();
                        ImGui::Text("Showing %d of %d found %s", shownCount, totalInType, PATTERN_NAMES[p]);
                        if (shownCount < totalInType) {
                            ImGui::SameLine();
                            if (ImGui::Button("Load +50")) displayLimits[p] += 50;
                            ImGui::SameLine();
                            if (ImGui::Button("Load All")) displayLimits[p] = 1000000;
                        } else if (displayLimits[p] > 20) {
                            ImGui::SameLine();
                            if (ImGui::Button("Reset View")) displayLimits[p] = 20;
                        }
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        int dw, dh; glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh); glClearColor(0.1f, 0.1f, 0.12f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    saveData();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return 0;
}