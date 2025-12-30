# Ultimate Quad Crossroad Finder (Java Edition)

This tool is a high-performance scanner designed to find "Quad Crossroads" in Nether Fortresses. These locations are the most efficient spots for Wither Skeleton farms. It uses the `cubiomes` library for fast biome and structure calculations.

## Example of a Quad Crossroad
Here is what a typical Quad Crossroad found by this tool looks like:

**MC_Version:** 1.21.10  
**Seed:** -7251143472382003430  
**Coordinates:** /tp -4770756 75 -874694

![Quad Crossroad Example 1](quadCrossroad.png)

## Performance & Testing
- **Search Speed:** Scanning a radius of 15,000 regions takes approximately **26 minutes** on an Intel(R) Core(TM) i7-10700K CPU @ 3.80GHz (8 Cores, 16 Threads).
- **Compatibility:** This tool is designed for Minecraft **Java Edition** only.
- **Tested Versions:** Specifically tested on versions **1.21.10** and **1.21.11**.
- **Status:** This is an experimental tool and has not been extensively tested across all environments or older versions.

## Prerequisites
- C++17 compatible compiler (GCC, Clang, or MSVC)
- CMake (Version 3.28 or higher)
- Git

## Installation & Build

1. Clone the repository with dependencies (submodules):
git clone --recursive <your-repo-url>
cd <project-folder>

2. Create a build environment:
mkdir build
cd build

3. Compile:
cmake -DCMAKE_BUILD_TYPE=Release ..
make

## Usage
1. Open `main.cpp` and configure your desired settings:
   - `radius`: The search range (e.g., 15000).
   - `mc_version`: Ensure this is set to `MC_1_21`.
2. Recompile the project after making changes.
3. Execute the finder:
./Crossroadsfinder
4. Enter the world seed when prompted.
5. Results will be saved to `quads_found.txt`, including the coordinates, teleport commands, and distance from the origin.

## Credits
This project utilizes the [cubiomes](https://github.com/Cubitect/cubiomes) library by Cubitect for core Minecraft generation logic.

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

*Note: The underlying cubiomes library is also licensed under the MIT License by its respective authors.*