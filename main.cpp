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

// Datenstruktur für Ergebnisse
struct ClusterResult {
    long long seed;
    int centerX, centerY, centerZ;
    long long distSq;

    // Vergleich für std::unique
    bool operator==(const ClusterResult& other) const {
        return seed == other.seed && centerX == other.centerX && centerY == other.centerY && centerZ == other.centerZ;
    }
};

// --- GLOBALE VARIABLEN ---
enum SearchMode { MODE_SINGLE_SCAN, MODE_SEED_FINDER };
SearchMode currentMode = MODE_SINGLE_SCAN;

std::atomic<bool> isSearching(false);
std::atomic<bool> isPaused(false);
std::mutex mergeMutex;
std::vector<ClusterResult> allResults;

// Fortschritt für Single Seed Scan
std::atomic<int> currentRxIndex(0);
int singleScanRadius = 15000;
char singleSeedBuf[64] = "0";

// Fortschritt für Seed Finder
std::atomic<long long> nextSeedToCheck(0);
std::atomic<long long> seedsCheckedThisSession(0);
int finderRadiusBlocks = 1000;

// Allgemeine Einstellungen
int threadsToUse = std::thread::hardware_concurrency();
bool lowPriorityMode = true;
auto startTime = std::chrono::steady_clock::now();

// --- PERSISTENZ (SPEICHERN/LADEN) ---

void saveData() {
    std::lock_guard<std::mutex> lock(mergeMutex);
    // Einstellungen speichern
    std::ofstream fs("config.cfg");
    if (fs.is_open()) {
        fs << nextSeedToCheck.load() << "\n";
        fs << singleScanRadius << "\n";
        fs << singleSeedBuf << "\n";
        fs << threadsToUse << "\n";
        fs << (lowPriorityMode ? 1 : 0) << "\n";
        fs << finderRadiusBlocks << "\n";
        fs.close();
    }
    // Ergebnisse speichern
    std::ofstream fr("results.dat");
    if (fr.is_open()) {
        for (const auto& r : allResults) {
            fr << r.seed << " " << r.centerX << " " << r.centerY << " " << r.centerZ << " " << r.distSq << "\n";
        }
        fr.close();
    }
}

void loadData() {
    std::ifstream fs("config.cfg");
    if (fs.is_open()) {
        long long nstc; fs >> nstc; nextSeedToCheck = nstc;
        fs >> singleScanRadius;
        fs >> singleSeedBuf;
        fs >> threadsToUse;
        int lpm; fs >> lpm; lowPriorityMode = (lpm == 1);
        fs >> finderRadiusBlocks;
        fs.close();
    }
    std::ifstream fr("results.dat");
    if (fr.is_open()) {
        ClusterResult r;
        while (fr >> r.seed >> r.centerX >> r.centerY >> r.centerZ >> r.distSq) {
            allResults.push_back(r);
        }
        fr.close();
    }
}

// --- KERNHILFSFUNKTION: QUAD-SUCHE IN REGION ---

bool checkRegionForQuad(Generator* g, long long seed, int rx, int rz, ClusterResult& out) {
    int mc = MC_1_21;
    Pos pos;
    if (!getStructurePos(Fortress, mc, (uint64_t)seed, rx, rz, &pos)) return false;
    if (!isViableStructurePos(Fortress, g, pos.x, pos.z, 0)) return false;

    Piece pieces[1000];
    int count = getFortressPieces(pieces, 1000, mc, (uint64_t)seed, pos.x >> 4, pos.z >> 4);
    if (count < 4) return false;

    for (int i = 0; i < count; i++) {
        if (pieces[i].type == BRIDGE_CROSSING) {
            int x = pieces[i].pos.x, y = pieces[i].pos.y, z = pieces[i].pos.z;
            bool hasR = false, hasB = false, hasD = false;
            for (int j = 0; j < count; j++) {
                if (pieces[j].type != BRIDGE_CROSSING || pieces[j].pos.y != y) continue;
                int dx = pieces[j].pos.x - x, dz = pieces[j].pos.z - z;
                if (dx == 19 && dz == 0) hasR = true;
                else if (dx == 0 && dz == 19) hasB = true;
                else if (dx == 19 && dz == 19) hasD = true;
            }
            if (hasR && hasB && hasD) {
                out.seed = seed;
                out.centerX = x + 9; out.centerY = y; out.centerZ = z + 9;
                out.distSq = (long long)x * x + (long long)z * z;
                return true;
            }
        }
    }
    return false;
}

// --- WORKER THREADS ---

#ifdef _WIN32
#include <windows.h>
#endif

void workerThread() {
    if (lowPriorityMode) {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    }

    Generator g;
    setupGenerator(&g, MC_1_21, 0);

    while (isSearching) {
        if (isPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }

        if (currentMode == MODE_SINGLE_SCAN) {
            // Modus: Einen Seed großflächig absuchen
            long long s = std::stoll(singleSeedBuf);
            applySeed(&g, -1, (uint64_t)s);
            int rx = -singleScanRadius + currentRxIndex.fetch_add(1);
            if (rx > singleScanRadius) { isSearching = false; break; }

            for (int rz = -singleScanRadius; rz <= singleScanRadius; rz++) {
                ClusterResult res;
                if (checkRegionForQuad(&g, s, rx, rz, res)) {
                    std::lock_guard<std::mutex> lock(mergeMutex);
                    allResults.push_back(res);
                }
            }
        }
        else {
            // Modus: Viele Seeds nah am Spawn absuchen
            long long s = nextSeedToCheck.fetch_add(1);
            seedsCheckedThisSession++;
            applySeed(&g, -1, (uint64_t)s);

            int rRad = (finderRadiusBlocks / 350) + 1; // Grobe Schätzung der Regions-Reichweite
            bool foundInSeed = false;
            for (int rx = -rRad; rx <= rRad && !foundInSeed; rx++) {
                for (int rz = -rRad; rz <= rRad; rz++) {
                    ClusterResult res;
                    if (checkRegionForQuad(&g, s, rx, rz, res)) {
                        if (std::sqrt(res.distSq) <= finderRadiusBlocks) {
                            std::lock_guard<std::mutex> lock(mergeMutex);
                            allResults.push_back(res);
                            foundInSeed = true;
                        }
                    }
                }
            }
        }
    }
}

// --- MAIN UI LOOP ---

int main() {
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(1200, 800, "Ultimate Minecraft Quad Finder", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    loadData();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. EINSTELLUNGEN
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_Always);
        ImGui::Begin("Settings & Controls", nullptr, ImGuiWindowFlags_NoResize);

        if (ImGui::BeginTabBar("Modes")) {
            if (ImGui::BeginTabItem("Single Seed Scan")) {
                currentMode = MODE_SINGLE_SCAN;
                ImGui::InputText("Target Seed", singleSeedBuf, 64, ImGuiInputTextFlags_CharsDecimal);
                ImGui::InputInt("Scan Radius (Regions)", &singleScanRadius);
                ImGui::Text("Status: %d / %d Regions", currentRxIndex.load(), (singleScanRadius * 2));
                if (ImGui::Button("Reset Scan Progress")) currentRxIndex = 0;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Seed Finder")) {
                currentMode = MODE_SEED_FINDER;
                ImGui::Text("Current Seed: %lld", nextSeedToCheck.load());
                ImGui::InputInt("Max Distance from 0,0", &finderRadiusBlocks);
                ImGui::Text("Seeds checked this session: %lld", seedsCheckedThisSession.load());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::SliderInt("Threads", &threadsToUse, 1, std::thread::hardware_concurrency());
        ImGui::Checkbox("Low Priority Mode", &lowPriorityMode);

        if (!isSearching) {
            if (ImGui::Button("START", ImVec2(-1, 40))) {
                isSearching = true;
                isPaused = false;
                seedsCheckedThisSession = 0;
                startTime = std::chrono::steady_clock::now();
                for (int i = 0; i < threadsToUse; i++) std::thread(workerThread).detach();
            }
        } else {
            if (ImGui::Button(isPaused ? "RESUME" : "PAUSE", ImVec2(215, 40))) isPaused = !isPaused;
            ImGui::SameLine();
            if (ImGui::Button("STOP & SAVE", ImVec2(215, 40))) { isSearching = false; saveData(); }
        }

        ImGui::End();

        // 2. ERGEBNISSE
        ImGui::SetNextWindowPos(ImVec2(470, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(710, 750), ImGuiCond_Always);
        ImGui::Begin("Permanent Results List", nullptr, ImGuiWindowFlags_NoResize);

        ImGui::Text("Total Quads found: %zu", allResults.size());
        if (ImGui::BeginTable("ResTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Seed", 0, 150);
            ImGui::TableSetupColumn("Dist", 0, 60);
            ImGui::TableSetupColumn("Teleport Command", 0, 300);
            ImGui::TableSetupColumn("Copy", 0, 100);
            ImGui::TableHeadersRow();

            std::lock_guard<std::mutex> lock(mergeMutex);
            for (int i = (int)allResults.size() - 1; i >= 0; i--) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%lld", allResults[i].seed);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", (int)std::sqrt(allResults[i].distSq));
                ImGui::TableSetColumnIndex(2);
                char cmd[128];
                snprintf(cmd, 128, "/tp %d %d %d", allResults[i].centerX, allResults[i].centerY + 2, allResults[i].centerZ);
                ImGui::Text("%s", cmd);
                ImGui::TableSetColumnIndex(3);
                char btnid[32]; snprintf(btnid, 32, "Copy##%d", i);
                if (ImGui::SmallButton(btnid)) ImGui::SetClipboardText(std::to_string(allResults[i].seed).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::End();

        // Auto-Save alle 30 Sekunden
        static auto lastS = std::chrono::steady_clock::now();
        if (isSearching && std::chrono::steady_clock::now() - lastS > std::chrono::seconds(30)) {
            saveData();
            lastS = std::chrono::steady_clock::now();
        }

        ImGui::Render();
        int dw, dh; glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh); glClearColor(0.1f, 0.1f, 0.12f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    isSearching = false;
    saveData();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return 0;
}