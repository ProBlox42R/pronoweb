# prono — Installation & Build Guide

`prono` is a lightweight C++ web server designed to be simple, fast, and easy to build.

This guide explains:

- Installing prebuilt binaries
- Building from source
- Running the server
- Creating your own builds
- Downloading releases

---

# Download (Prebuilt Binaries)

You can download the latest compiled binaries from the GitHub Releases page.

## Windows

Download the latest Windows build:

https://github.com/problox42r/pronoweb/releases/latest

Look for:


prono-windows-x64.zip


### Install Steps

1. Download the zip file
2. Extract it
3. Open the folder
4. Run:


prono.exe


---

## Linux

Download the latest Linux binary:

https://github.com/problox42r/pronoweb/releases/latest

Look for:


prono-linux-x64.tar.gz


### Install Steps

Extract and run:


tar -xzf prono-linux-x64.tar.gz
cd prono
chmod +x prono
./prono


---

# Build From Source

## Requirements

You need:

- C++17 compatible compiler
- CMake 3.16+
- Git

### Linux Dependencies

Ubuntu / Debian:


sudo apt install build-essential cmake git


Arch:


sudo pacman -S base-devel cmake git


Fedora:


sudo dnf install gcc-c++ cmake git


---

### Windows Dependencies

Install:

- Visual Studio 2022
- CMake

Make sure the **Desktop Development with C++** workload is installed.

---

# Clone the Repository


git clone https://github.com/YOUR_USERNAME/prono.git

cd prono


---

# Build Using CMake

Create a build directory:


mkdir build
cd build


Generate the project:


cmake ..


Build:


cmake --build .


The binary will be located in:


build/bin/prono


On Windows:


build/bin/prono.exe


---

# Run the Server

Linux:


./prono


Windows:


prono.exe


---

# Build Release Version

For optimized builds:


cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .


---

# Creating Your Own Binary Releases

## Linux


cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
strip bin/prono


Package:


tar -czf prono-linux-x64.tar.gz bin/prono


---

## Windows

Build using Release mode in Visual Studio or:


cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release


Package:


zip prono-windows-x64.zip bin/prono.exe


---

# Repository Structure

Example layout:


prono/
│
├── src/
│ ├── main.cpp
│ ├── server.cpp
│ └── server.hpp
│
├── CMakeLists.txt
├── README.md
└── build/


---

# Troubleshooting

## CMake not found

Install it:

Linux:


sudo apt install cmake


Windows:

Download from:

https://cmake.org/download/

---

## Compiler errors

Make sure your compiler supports **C++17**.

Check with:


g++ --version


---

# License

MIT License
"""

base = Path("/mnt/data")
cmake_path = base / "CMakeLists.txt"
md_path = base / "INSTALL.md"

cmake_path.write_text(cmake_content)
md_path.write_text(md_content)

str(cmake_path), str(md_path)
Result
