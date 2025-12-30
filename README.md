# Ultimate Quad Crossroad Finder (Java Edition)

This tool is a high-performance GUI-based scanner designed to find "Quad Crossroads" in Nether Fortresses. These locations are the most efficient spots for Wither Skeleton farms. It utilizes the `cubiomes` library for fast calculations and `Dear ImGui` for a modern, interactive interface.

## Key Features
- **Interactive GUI**: No more editing source code or using the command line.
- **Persistence**: Automatically saves your settings and search results.
- **Resume/Pause**: Stop the search at any time and resume exactly where you left off, even after a restart.
- **Performance Control**: Adjustable thread count and "Low Priority Mode" to keep your PC smooth while scanning.
- **Direct Integration**: Click "Copy" to put teleport commands directly into your clipboard.

## Example of a Quad Crossroad
![Quad Crossroad Example 1](quadCrossroad.png)

**MC_Version:** 1.21.x  
**Seed:** -7251143472382003430  
**Coordinates:** `/tp -4770756 75 -874694`

## Usage
1. **Launch the tool**  
   Run the compiled `Crossroadsfinder` executable.

2. **Enter Seed**  
   Paste your world seed into the input field.

3. **Set Radius**
   - Type in the radius or use the `+` / `-` buttons.
   - **Pro Tip**: Hold `Shift` for +10, `Ctrl` for +100, or `Ctrl+Shift` for +1000 increments.

4. **Performance Settings**  
   Choose how many CPU threads to allocate and toggle **Background Mode** if you want to use your PC for other tasks during the scan.

5. **Start Scan**  
   Click **Start New Search**.

6. **Results**  
   Found crossroads appear in real-time in the results table. Use the **Copy** button to get the `/tp` command.

## Performance & Testing
- **Search Speed:** Scanning a radius of 15,000 regions takes approximately **26 minutes** on an Intel(R) Core(TM) i7-10700K CPU @ 3.80GHz (8 Cores, 16 Threads).
- **Compatibility:** Designed for Minecraft **Java Edition (1.21+)**.
- **Efficiency:** The tool uses structure-specific generation logic to skip unnecessary biome checks, making it one of the fastest finders available.

## Prerequisites
- C++17 compatible compiler (MSVC, GCC, or Clang)
- CMake (Version 3.28 or higher)
- OpenGL drivers and GLFW dependencies

## Installation & Build

1. **Clone the repository** (including all submodules: cubiomes, imgui, glfw)
```bash
   git clone --recursive <your-repo-url>  
   cd <project-folder>
```

2. **Build with CMake**
```bash
   mkdir build  
   cd build  
   cmake -DCMAKE_BUILD_TYPE=Release ..  
   cmake --build . --config Release
```
## Credits
This project utilizes:
- **cubiomes** by Cubitect for core Minecraft generation logic
- **Dear ImGui** for the graphical interface
- **GLFW** for window and input management

## License
This project is licensed under the **MIT License** – see the `LICENSE` file for details.