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
};

// --- Globale Daten für die Kommunikation zwischen Threads ---
std::atomic<long long> regionsProcessed(0);
std::atomic<bool> isSearching(false);
std::mutex mergeMutex;
std::vector<ClusterResult> allResults;
long long totalRegionsToProcess = 1;
auto startTime = std::chrono::steady_clock::now();

// --- Logik-Funktionen (Unverändert, nur leicht angepasst) ---

void searchSector(int64_t seed, int mc_version, int rxStart, int rxEnd, int radius) {
    Generator g;
    setupGenerator(&g, mc_version, 0);
    applySeed(&g, -1, (uint64_t)seed);

    std::vector<ClusterResult> localResults;
    Piece pieces[1000];
    struct SimplePos { int x, y, z; };
    SimplePos crossings[128];
    long long localCounter = 0;

    for (int rx = rxStart; rx < rxEnd; rx++) {
        for (int rz = -radius; rz <= radius; rz++) {
            if (++localCounter >= 512) {
                regionsProcessed.fetch_add(localCounter, std::memory_order_relaxed);
                localCounter = 0;
            }

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

// Startet die Suche in einem Hintergrund-Thread-Pool
void runSearchManager(int64_t seed, int radius) {
    int mc_version = MC_1_21;
    long long rangeX = (2LL * radius) + 1;
    totalRegionsToProcess = rangeX * rangeX;
    regionsProcessed = 0;
    allResults.clear();
    startTime = std::chrono::steady_clock::now();

    unsigned int tc = std::thread::hardware_concurrency();
    if (tc == 0) tc = 4;

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < tc; i++) {
        int startRx = -radius + (int)(i * rangeX / tc);
        int endRx = -radius + (int)((i + 1) * rangeX / tc);
        threads.emplace_back(searchSector, seed, mc_version, startRx, endRx, radius);
    }

    for (auto& t : threads) t.join();

    std::sort(allResults.begin(), allResults.end(), [](const ClusterResult& a, const ClusterResult& b) {
        return a.distSq < b.distSq;
    });

    isSearching = false; // Signal an die GUI: Fertig!
}

int main() {
    // --- 1. GLFW Initialisierung ---
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(1000, 700, "Ultimate Quad Finder 1.21", NULL, NULL);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // V-Sync an

    // --- 2. ImGui Initialisierung ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // UI State
    char seedBuf[64] = "0";
    int radiusInput = 15000;
    float progress = 0.0f;

    // --- 3. Hauptschleife ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Fenster 1: Steuerung ---
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(380, 220), ImGuiCond_Always);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::InputText("Seed", seedBuf, 64);
        ImGui::SliderInt("Radius", &radiusInput, 1000, 30000);

        if (ImGui::Button("Start Search", ImVec2(-1, 40)) && !isSearching) {
            isSearching = true;
            int64_t seed = std::stoll(seedBuf);
            std::thread(runSearchManager, seed, radiusInput).detach();
        }

        if (isSearching) {
            long long current = regionsProcessed.load();
            progress = (float)current / (float)totalRegionsToProcess;
            ImGui::ProgressBar(progress, ImVec2(-1, 0));

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (elapsed > 0) {
                long long speed = current / elapsed;
                ImGui::Text("Speed: %lld reg/s", speed);
            }
        } else {
            ImGui::Text("Status: Waiting...");
        }
        ImGui::End();

        // --- Fenster 2: Ergebnisse ---
        ImGui::SetNextWindowPos(ImVec2(400, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(580, 680), ImGuiCond_Always);
        ImGui::Begin("Results Found", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        if (ImGui::BeginTable("ResultTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Coordinates (X, Y, Z)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            std::lock_guard<std::mutex> lock(mergeMutex);
            for (int i = 0; i < allResults.size(); i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", (int)std::sqrt(allResults[i].distSq));

                ImGui::TableSetColumnIndex(1);
                char coordBuf[128];
                snprintf(coordBuf, sizeof(coordBuf), "/tp %d %d %d", allResults[i].centerX, allResults[i].centerY + 2, allResults[i].centerZ);
                ImGui::Text("%s", coordBuf);

                ImGui::TableSetColumnIndex(2);
                ImGui::PushID(i);
                if (ImGui::SmallButton("Copy TP")) {
                    ImGui::SetClipboardText(coordBuf);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::End();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}