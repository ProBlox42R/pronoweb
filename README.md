# Prono Web Server

**Prono** is a minimal, fast, and easy-to-build **C++17** HTTP web server focused on simplicity and low resource usage.

Perfect for small static sites, embedded systems, local development tools, or learning projects.

Current version: v0.1.x (pre-release / early development)  
License: [MIT](./LICENSE)

## Features

- Single-threaded / multi-threaded modes (configurable)
- Serves static files (HTML, CSS, JS, images, etc.)
- Basic HTTP/1.1 support (GET, HEAD, limited POST)
- Directory listing (optional)
- Very small binary size (~ few hundred KB when stripped)
- No external runtime dependencies
- Cross-platform (Windows, Linux, macOS possible with minor tweaks)

## Quick Start

### 1. Download prebuilt binary (recommended for most users)

Go to → [Releases page](https://github.com/problox42r/pronoweb/releases/latest)

- **Windows**: `prono-windows-x64.zip`
- **Linux**  : `prono-linux-x64.tar.gz`

**Windows example**

```bash
# Extract → double-click prono.exe  or run from cmd/powershell:
prono.exe
Linux example
Bashtar -xzf prono-linux-x64.tar.gz
cd prono
chmod +x prono
./prono
By default, server starts at http://localhost:8080 and serves files from the current directory.
2. Build from source (developers / custom builds)
Requirements

C++17 compatible compiler
GCC 7+ / Clang 5+ / MSVC 2019+

CMake ≥ 3.16
Git

Linux / macOS dependencies
Bash# Ubuntu / Debian
sudo apt update
sudo apt install build-essential cmake git

# Arch
sudo pacman -S base-devel cmake git

# Fedora
sudo dnf install gcc-c++ cmake git
Windows
Install:

Visual Studio 2022 (with Desktop development with C++ workload)
Latest CMake

Clone & Build
Bashgit clone https://github.com/problox42r/pronoweb.git
cd pronoweb

mkdir build && cd build

# Normal build
cmake ..
cmake --build .

# Optimized release build (recommended)
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release    # Windows
# or just: cmake --build .            # Linux/macOS
Binary location:

Linux/macOS → build/bin/prono
Windows    → build/bin/Release/prono.exe or build/bin/prono.exe

Running the Server
Basic usage:
Bash./prono                # Linux/macOS
prono.exe              # Windows
Common options (early versions — check ./prono --help if implemented):
Bash./prono --port 9000              # change port
./prono --dir ./public           # serve from specific folder
./prono --port 8080 --dir www    # combined
Open in browser: http://localhost:8080 (or chosen port)
Configuration (planned / basic support)
Future versions will support a simple prono.ini or command-line arguments.
Current minimal support (if any) is via flags only.
Example planned config style:
ini# prono.ini (not yet implemented in early versions)
port = 8080
root = ./public
index = index.html index.htm
directory_listing = false
Creating Your Own Releases
Linux
Bashcd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .

strip bin/prono
tar -czf prono-linux-x64.tar.gz bin/prono
Windows
Bashcd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release

# Compress (from build folder)
Compress-Archive -Path bin/Release/prono.exe -DestinationPath prono-windows-x64.zip
# or use 7-Zip / WinRAR
Project Structure
textprono/
├── src/
│   ├── main.cpp
│   ├── server.cpp
│   ├── server.hpp
│   └── ... (other .cpp/.h files)
├── CMakeLists.txt
├── README.md           ← you are here
├── LICENSE
└── build/              ← generated
Troubleshooting
CMake not found
→ Install from https://cmake.org/download/
Compiler does not support C++17
Check version:
Bashg++ --version           # should be ≥ 7
clang++ --version       # ≥ 5
"Permission denied" on Linux
Bashchmod +x prono
Port already in use
→ Change port: ./prono --port 9000
Binary crashes / no output

Make sure you run it from a folder with some .html files
Try with absolute path: ./prono --dir /full/path/to/site

Contributing
Contributions welcome!

Fork the repo
Create feature/bugfix branch
Submit pull request

Especially looking for:

HTTPS / SSL support
Better request routing
Logging
Configuration file support
Windows improvements

License
MIT License

Made with :heart: by problox42r
Happy hacking!
