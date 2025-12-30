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

void saveSettings(const char* seed, int radius) {
    std::ofstream f("settings.cfg");
    if (f.is_open()) {
        f << seed << "\n";
        f << radius << "\n";
        f.close();
    }
}

void loadSettings(char* seedBuf, int& radius) {
    std::ifstream f("settings.cfg");
    if (f.is_open()) {
        std::string line;
        if (std::getline(f, line)) {
            // Seed laden (max 63 Zeichen + Nullterminator)
            strncpy(seedBuf, line.c_str(), 63);
            seedBuf[63] = '\0';
        }
        if (std::getline(f, line)) {
            // Radius laden
            try {
                radius = std::stoi(line);
            } catch (...) {
                radius = 15000; // Fallback bei Fehler
            }
        }
        f.close();
    }
}

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

    loadSettings(seedBuf, radiusInput);

    // --- 3. Hauptschleife ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Fenster 1: Steuerung ---
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(380, 280), ImGuiCond_Always); // Etwas höher für die Zusatzinfos
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        // Seed Eingabe
        ImGui::InputText("Seed", seedBuf, 64);

        // --- Im Inneren der main-Schleife (während das Fenster gezeichnet wird) ---

        // 1. Modifier-Logik für den Radius berechnen
        int radiusStep = 1;
        ImGuiIO& io = ImGui::GetIO();

        if (io.KeyCtrl && io.KeyShift) {
            radiusStep = 1000;
        } else if (io.KeyCtrl) {
            radiusStep = 100;
        } else if (io.KeyShift) {
            radiusStep = 10;
        }

        // 2. Das Input-Feld mit dynamischem Step anzeigen
        // Wir setzen step_fast auf 0, damit nur unser berechneter Step zählt
        ImGui::InputInt("Radius (Regions)", &radiusInput, radiusStep, 0);

        // Schutz vor unsinnigen Werten
        if (radiusInput < 1) radiusInput = 1;

        // Kleiner Hilfstext für den Nutzer (optional, aber sehr hilfreich)
        ImGui::TextDisabled("(?) Shift=+10, Ctrl=+100, Ctrl+Shift=+1000");

        // --- BERECHNUNGEN FÜR INFOS ---
        // Eine Region in der 1.21 Nether-Struktur-Verteilung ist ca. 512x512 Blöcke groß (32x32 Chunks)
        long long sideLengthRegions = (2LL * radiusInput) + 1;
        long long totalRegions = sideLengthRegions * sideLengthRegions;
        double totalBlocksSide = sideLengthRegions * 512.0;
        double totalChunksSide = sideLengthRegions * 32.0;

        ImGui::Separator();
        ImGui::Text("Scan Area Info:");
        ImGui::BulletText("Total Regions: %lld", totalRegions);
        ImGui::BulletText("Area: %.0f x %.0f Blocks", totalBlocksSide, totalBlocksSide);
        ImGui::BulletText("Chunks: %.0f x %.0f", totalChunksSide, totalChunksSide);
        ImGui::Separator();

        // Start Button
        if (ImGui::Button("Start Search", ImVec2(-1, 40)) && !isSearching) {
            isSearching = true;
            saveSettings(seedBuf, radiusInput);
            try {
                int64_t seed = std::stoll(seedBuf);
                std::thread(runSearchManager, seed, radiusInput).detach();
            } catch (...) {
                isSearching = false; // Falls Seed kein Long ist
            }
        }

        // --- FORTSCHRITT & ETA ---
        if (isSearching) {
            long long current = regionsProcessed.load();
            long long total = totalRegionsToProcess; // Wird in runSearchManager gesetzt
            progress = (total > 0) ? (float)current / (float)total : 0.0f;

            ImGui::ProgressBar(progress, ImVec2(-1, 0));

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();

            if (elapsed > 0 && current > 1000) {
                long long speed = current / elapsed;
                long long remaining = total - current;
                long long secondsLeft = (speed > 0) ? (remaining / speed) : 0;

                ImGui::Text("Speed: %lld reg/s", speed);
                ImGui::SameLine(ImGui::GetWindowWidth() - 120);
                ImGui::Text("ETA: %s", formatTime(secondsLeft).c_str());
            } else {
                ImGui::Text("Calculating speed...");
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Status: Ready to scan");
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

    saveSettings(seedBuf, radiusInput);

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}