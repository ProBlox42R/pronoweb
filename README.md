![Prono Server](images/logo/logo.png)

# Prono Web Server — Documentation

> A blazing-fast HTTP/1.1 web server written in C++17 with a live admin dashboard, built by **probloxworld**.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Building from Source](#building-from-source)
   - [Linux](#linux)
   - [Windows](#windows)
4. [Running Prono](#running-prono)
5. [Configuration Reference](#configuration-reference)
6. [Admin Dashboard](#admin-dashboard)
7. [All Dashboard Endpoints](#all-dashboard-endpoints)
8. [Installing as a Service](#installing-as-a-service)
   - [Linux — systemd](#linux--systemd)
   - [Windows — NSSM](#windows--nssm)
   - [Windows — Task Scheduler](#windows--task-scheduler)
9. [Security Hardening](#security-hardening)
10. [Default Cache Rules](#default-cache-rules)
11. [Known Limitations](#known-limitations)

---

## Overview

Prono is a single-file C++17 HTTP/1.1 web server built on top of **Boost.Asio**. It is designed to be compiled once into a standalone binary and run with zero configuration files. All settings are managed live through a built-in admin dashboard served on a dedicated port.

**Key facts:**

- ~1,900 lines of C++17 in a single `.cpp` file
- No runtime configuration files — everything lives in memory
- Web serving and admin dashboard run on two completely independent ports and thread loops
- Every admin dashboard button and form hits a real C++ backend route — no stubs
- Binds to `0.0.0.0` by default, making it accessible from any device on the network

---

## Architecture

Prono runs two independent TCP acceptors inside a single process:

```
Internet / LAN
      │
      ├─── port 8080 ──▶  handleClient()   Static file serving, rewrites, rate limiting
      │
      └─── port 8081 ──▶  handleAdmin()    Live dashboard, config mutations, file browser
```

Both acceptors share the same `io_context` but run on separate threads. Each incoming connection gets its own detached `std::thread`. All shared state is protected by `std::mutex` locks. Runtime counters (request count, bytes sent/received, error counts) use `std::atomic<long long>` so they are never locked on the hot path.

### Core structs

| Struct | Purpose |
|--------|---------|
| `Config` | All live server configuration. One global instance `CFG` behind `cfgMutex` |
| `RewriteRule` | Holds a regex pattern, replacement string, and HTTP code (0 = internal rewrite) |
| `ParsedReq` | Parsed HTTP request — method, path, headers map, body, query params. Shared by both handlers |

### Request flow (web port)

```
TCP accept → parseRequest() → IP block check → method check
    → rate limit check → log + stats → vhost resolution
    → rewrite engine → static file serve → Cache-Control inject
    → security headers inject → response
```

### Request flow (admin port)

```
TCP accept → parseRequest() → route match on cleanPath
    → handler block (mutates CFG under cfgMutex) → dashboard() or plain text response
```

---

## Building from Source

### Linux

#### Prerequisites

You need GCC 9+ (or Clang 10+) with C++17 support, and Boost development headers.

**Debian / Ubuntu:**
```bash
sudo apt update
sudo apt install build-essential libboost-dev
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc-c++ boost-devel
```

**Arch Linux:**
```bash
sudo pacman -S base-devel boost
```

#### Compile

```bash
g++ -std=c++17 -O2 -o prono prono_server.cpp -lboost_system -lpthread
```

**Flag breakdown:**

| Flag | Meaning |
|------|---------|
| `-std=c++17` | Required for `std::filesystem`, structured bindings, `if constexpr` |
| `-O2` | Optimisation level 2 — fast binary, reasonable compile time |
| `-lboost_system` | Links Boost.System (required by Boost.Asio error codes) |
| `-lpthread` | Links POSIX threads (required by `std::thread`) |

**Optional flags:**

```bash
# Debug build with AddressSanitizer
g++ -std=c++17 -g -fsanitize=address -o prono_debug prono_server.cpp -lboost_system -lpthread

# Fully static binary (larger, but no .so dependencies at runtime)
g++ -std=c++17 -O2 -static -o prono_static prono_server.cpp -lboost_system -lpthread -lboost_thread -lrt

# Strip symbols (smaller binary)
g++ -std=c++17 -O2 -s -o prono prono_server.cpp -lboost_system -lpthread
```

#### Verify

```bash
./prono --help   # will fail gracefully and print usage
ldd prono        # check runtime dependencies
```

---

### Windows

#### Prerequisites

Install **MSYS2** from https://msys2.org. It provides a GCC toolchain and package manager (`pacman`) on Windows.

After installing MSYS2, open the **UCRT64 terminal** and run:

```bash
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-boost
```

#### Required code change

Add this at the very top of `prono_server.cpp`, **before** any `#include` lines:

```cpp
#ifdef _WIN32
  #define _WIN32_WINNT 0x0601  // Target Windows 7 and above
#endif
```

Without this, Boost.Asio on Windows emits WinSock deprecation errors and may fail to compile.

Also replace the Linux-only include:

```cpp
// Remove this line:
#include <unistd.h>   // gethostname

// Replace with:
#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <unistd.h>
#endif
```

#### Compile (in MSYS2 UCRT64 terminal)

```bash
g++ -std=c++17 -O2 -o prono.exe prono_server.cpp \
    -lboost_system -lpthread -lws2_32 -lwsock32
```

**Extra Windows flags:**

| Flag | Meaning |
|------|---------|
| `-lws2_32` | Windows Sockets 2 library — required for all TCP networking |
| `-lwsock32` | Legacy WinSock compatibility — required by some Boost.Asio internals |

#### Distributing the binary

The compiled `prono.exe` will depend on MSYS2 runtime DLLs. To make it run on machines without MSYS2 installed, copy these DLLs next to `prono.exe`:

```bash
# Find and copy required DLLs
ldd prono.exe | grep ucrt | awk '{print $3}' | xargs -I{} cp {} .
ldd prono.exe | grep mingw | awk '{print $3}' | xargs -I{} cp {} .
```

Common required DLLs: `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `libboost_system.dll`.

Alternatively, compile statically to avoid DLL dependencies:

```bash
g++ -std=c++17 -O2 -static -o prono.exe prono_server.cpp \
    -lboost_system -lpthread -lws2_32 -lwsock32 -lmswsock
```

---

## Running Prono

```
./prono [webPort] [webroot] [adminPort]
```

All arguments are optional. Defaults:

| Argument | Default | Description |
|----------|---------|-------------|
| `webPort` | `8080` | Port for static file serving |
| `webroot` | `./www` | Directory to serve files from |
| `adminPort` | `8081` | Port for the admin dashboard |

**Examples:**

```bash
# Default — web on 8080, serve ./www, admin on 8081
./prono

# Custom web port and webroot
./prono 80 /var/www/html

# All three explicit
./prono 80 /var/www/html 9090

# Windows
prono.exe 8080 C:\Users\you\www 8081
```

**Startup output:**

```
╔══════════════════════════════════════════╗
║  🔥  PRONO WEB SERVER  v1.0              ║
║      by probloxworld                     ║
╠══════════════════════════════════════════╣
║  WebRoot  : ./www                        ║
║  Web      : http://192.168.1.42:8080/    ║
║  Admin    : http://192.168.1.42:8081/    ║
╚══════════════════════════════════════════╝
[prono] Web   listening on 0.0.0.0:8080
[prono] Admin listening on 0.0.0.0:8081
```

Both ports bind to `0.0.0.0`, meaning they are reachable from any device on your network using the displayed LAN IP. The server auto-detects the local IP using `gethostname()` + DNS resolution.

---

## Configuration Reference

All configuration is held in the global `Config CFG` struct. Every field can be modified live via the admin dashboard — no restarts needed. Changes are written under `cfgMutex` and take effect on the next request.

### Core settings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `webroot` | `string` | `./www` | Document root for static files |
| `port` | `int` | `8080` | Web server port |
| `adminPort` | `int` | `8081` | Admin dashboard port |
| `serverName` | `string` | `Prono/1.0` | Value of the `Server:` response header |
| `logFile` | `string` | `prono.log` | Path for the access log file |
| `maxLogEntries` | `int` | `500` | Max in-memory log lines before oldest are dropped |

### Toggles

| Field | Default | Description |
|-------|---------|-------------|
| `dirListing` | `true` | Show directory index when no `index.html` exists |
| `accessLog` | `true` | Write requests to `logFile` |
| `colorConsole` | `true` | ANSI colour in terminal output |

### Security headers

| Field | Default | Description |
|-------|---------|-------------|
| `xcto` | `true` | `X-Content-Type-Options: nosniff` |
| `xframe` | `true` | `X-Frame-Options: SAMEORIGIN` |
| `xss` | `true` | `X-XSS-Protection: 1; mode=block` |
| `hsts` | `false` | `Strict-Transport-Security: max-age=31536000` |
| `csp` | `false` | `Content-Security-Policy: <cspValue>` |
| `cspValue` | `default-src 'self'` | The CSP directive string |
| `referrerPolicy` | `strict-origin-when-cross-origin` | `Referrer-Policy` header value |

### CORS

| Field | Default | Description |
|-------|---------|-------------|
| `corsEnabled` | `false` | Add CORS headers to every response |
| `corsCredentials` | `false` | `Access-Control-Allow-Credentials: true` |
| `corsOrigin` | `*` | `Access-Control-Allow-Origin` value |
| `corsMethods` | `GET, POST, PUT, DELETE, OPTIONS` | Allowed methods |
| `corsHeaders` | `Content-Type, Authorization` | Allowed headers |
| `corsMaxAge` | `86400` | Preflight cache duration in seconds |

### Rate limiting

| Field | Default | Description |
|-------|---------|-------------|
| `rateLimitEnabled` | `false` | Enable per-IP rate limiting |
| `rateLimitPerMin` | `200` | Max requests per minute per IP |
| `rateLimitBurst` | `40` | Extra burst allowance above the per-minute limit |

Rate limiting uses a sliding 60-second window stored in `rlState` (a `map<string, pair<int,long long>>`). IPs that exceed the limit receive `429 Too Many Requests` before any file logic runs.

### Allowed HTTP methods

Default: `GET`, `HEAD`, `POST`, `OPTIONS`.

Requests using a method not in this set receive `405 Method Not Allowed`.

### Collections (managed via dashboard)

| Collection | Type | Description |
|------------|------|-------------|
| `customHeaders` | `map<string,string>` | Extra headers added to every response |
| `vhosts` | `map<string,string>` | Hostname → docroot mappings |
| `blockedIPs` | `set<string>` | IPs that receive 403 immediately |
| `errorPages` | `map<int,string>` | HTTP code → custom error page path (relative to webroot) |
| `rewrites` | `vector<RewriteRule>` | URL rewrite/redirect rules with regex |
| `cacheRules` | `map<string,string>` | File extension → Cache-Control value |
| `authUsers` | `map<string,string>` | Username → password for Basic Auth |
| `authPaths` | `vector<string>` | Path prefixes protected by Basic Auth |

---

## Admin Dashboard

Open the admin dashboard at:

```
http://<your-ip>:8081/pronoadmin
```

The dashboard is served exclusively on the admin port (`8081` by default). It is never accessible from the web port (`8080`) — attempting to visit `/pronoadmin` on port 8080 returns a `404`.

### Tabs

| Tab | What it does |
|-----|-------------|
| **Overview** | Live counters: total requests, uptime, bytes sent, 4xx/5xx errors. Top paths, top IPs, status code breakdown, method breakdown |
| **Logs** | In-memory access log with timestamps and IPs. Download as `.log` file or clear in one click |
| **Virtual Hosts** | Add/remove hostname → docroot mappings. Creates the docroot directory if it doesn't exist |
| **Config** | Edit server name, webroot, log file path, max log entries. Toggle allowed HTTP methods |
| **Rewrites** | Add/remove URL rewrite rules. Regex pattern + replacement + code (0 = internal rewrite, 301/302 = redirect) |
| **Headers** | Toggle all security headers (HSTS, CSP, XFrame, XCTO, XSS, Referrer-Policy). Edit CSP value and Referrer-Policy. Add/remove custom response headers |
| **Cache** | Add/remove per-extension Cache-Control rules |
| **Rate Limit** | Enable/disable rate limiting, set max req/min and burst. View and clear current per-IP state |
| **Firewall** | Block/unblock IPs. Blocked IPs are checked first — before any request processing |
| **Stats** | Per-path hit counts, per-IP hit counts, status code distribution, method distribution. Reset all stats with one click |
| **Files** | Browse the webroot directory tree. View file sizes and modification dates. Download or delete individual files |
| **Error Pages** | Assign custom HTML pages to specific HTTP status codes. Preview any error page. Remove overrides to revert to Prono's built-in error pages |

### AJAX toggles

Security header toggles use `fetch()` to hit `/pronoadmin/toggle?key=<name>&val=0/1` and show a toast notification without a full page reload. The server responds with plain text (`enabled` / `disabled`) and the toggle state is reflected immediately.

---

## All Dashboard Endpoints

Every form and button in the dashboard hits one of these routes on the admin port:

| Endpoint | Method | Action |
|----------|--------|--------|
| `GET /pronoadmin` | GET | Render dashboard (overview tab) |
| `GET /pronoadmin/files?dir=X` | GET | Browse directory X in the file browser tab |
| `GET /pronoadmin/savecfg` | GET | Save serverName, webroot, logFile, maxLogEntries |
| `GET /pronoadmin/savemethods` | GET | Update allowed HTTP methods from checkboxes |
| `GET /pronoadmin/toggle?key=X&val=1` | GET | AJAX toggle — mutates any boolean config key, returns plain text |
| `GET /pronoadmin/savecsp` | GET | Save CSP value + Referrer-Policy |
| `GET /pronoadmin/savecors` | GET | Save all CORS settings |
| `GET /pronoadmin/addheader` | GET | Add a custom response header |
| `GET /pronoadmin/removeheader` | GET | Remove a custom response header |
| `GET /pronoadmin/addvhost` | GET | Add a virtual host and create its docroot |
| `GET /pronoadmin/removevhost` | GET | Remove a virtual host |
| `GET /pronoadmin/addrewrite` | GET | Add a URL rewrite rule |
| `GET /pronoadmin/removerewrite` | GET | Remove a rewrite rule by index |
| `GET /pronoadmin/addcache` | GET | Add a Cache-Control rule for a file extension |
| `GET /pronoadmin/removecache` | GET | Remove a Cache-Control rule |
| `GET /pronoadmin/saverl` | GET | Save rate limit settings |
| `GET /pronoadmin/clearrl` | GET | Clear all per-IP rate limit state |
| `GET /pronoadmin/block` | GET | Block an IP address |
| `GET /pronoadmin/unblock` | GET | Unblock an IP address |
| `GET /pronoadmin/resetstats` | GET | Reset all atomic counters and stat maps |
| `GET /pronoadmin/clearlog` | GET | Delete all in-memory log entries |
| `GET /pronoadmin/downloadlog` | GET | Download the access log as a file attachment |
| `GET /pronoadmin/seterrorpage` | GET | Assign a custom error page path to a status code |
| `GET /pronoadmin/removeerrorpage` | GET | Remove a custom error page override |
| `GET /pronoadmin/previewerror?code=NNN` | GET | Preview the error page for any status code |
| `GET /pronoadmin/testconfig` | GET | Validate current config (checks webroot exists, ports valid, etc.) |
| `GET /pronoadmin/delete?file=X&dir=Y` | GET | Delete a file from the webroot |
| `GET /pronoadmin/download?file=X` | GET | Serve a file as a download attachment |

---

## Installing as a Service

### Linux — systemd

Copy the binary to a system path and create the service unit file:

```bash
sudo cp prono /usr/local/bin/prono
sudo chmod +x /usr/local/bin/prono
```

Create `/etc/systemd/system/prono.service`:

```ini
[Unit]
Description=Prono Web Server
After=network.target

[Service]
Type=simple
User=www-data
WorkingDirectory=/var/www
ExecStart=/usr/local/bin/prono 8080 /var/www/html 8081
Restart=on-failure
RestartSec=3
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/var/www/html /var/log/prono

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now prono
```

Useful commands:

```bash
sudo systemctl stop prono        # Stop the server
sudo systemctl restart prono     # Restart after binary update
sudo systemctl disable prono     # Remove from autostart
sudo systemctl status prono      # Check current status
sudo journalctl -u prono -f      # Follow live logs
sudo journalctl -u prono --since "1 hour ago"   # Recent logs
```

---

### Windows — NSSM

**NSSM** (Non-Sucking Service Manager) wraps any `.exe` as a proper Windows service with auto-restart, logging, and integration with `services.msc`.

1. Download from https://nssm.cc/download
2. Extract and copy `nssm.exe` to `C:\Windows\System32`
3. Create a folder for Prono, e.g. `C:\prono\`, and place `prono.exe` and your webroot there

Open **Command Prompt as Administrator**:

```bat
nssm install prono "C:\prono\prono.exe"
nssm set prono AppParameters "8080 C:\prono\www 8081"
nssm set prono AppDirectory "C:\prono"
nssm set prono Description "Prono Web Server"
nssm set prono Start SERVICE_AUTO_START
nssm set prono AppStdout "C:\prono\prono.log"
nssm set prono AppStderr "C:\prono\prono-error.log"
nssm start prono
```

Useful commands:

```bat
nssm stop prono              :: Stop the service
nssm restart prono           :: Restart the service
nssm status prono            :: Check status
nssm edit prono              :: Open GUI editor for the service
nssm remove prono confirm    :: Uninstall the service completely
```

You can also manage the service from `services.msc` — it will appear in the list as **Prono Web Server**.

---

### Windows — Task Scheduler

A lighter-weight option that requires no extra tools. Does not auto-restart on crash.

```bat
schtasks /create ^
  /tn "Prono" ^
  /tr "C:\prono\prono.exe 8080 C:\prono\www 8081" ^
  /sc onstart ^
  /ru SYSTEM ^
  /f
```

To stop and remove:

```bat
schtasks /end /tn "Prono"
schtasks /delete /tn "Prono" /f
```

> For production use, prefer NSSM — it handles crash recovery automatically.

---

## Security Hardening

Before using Prono on a public-facing server, address these points:

### 1. Put HTTPS in front of it

Prono speaks plain HTTP only. Use a reverse proxy for TLS termination:

**Nginx** (`/etc/nginx/sites-available/prono`):

```nginx
server {
    listen 443 ssl http2;
    server_name yourdomain.com;

    ssl_certificate     /etc/letsencrypt/live/yourdomain.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/yourdomain.com/privkey.pem;

    location / {
        proxy_pass         http://127.0.0.1:8080;
        proxy_set_header   Host $host;
        proxy_set_header   X-Real-IP $remote_addr;
        proxy_set_header   X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

Or use **Caddy** for automatic Let's Encrypt:

```
yourdomain.com {
    reverse_proxy localhost:8080
}
```

### 2. Lock down the admin port

The admin port should never be reachable from the internet. On Linux:

```bash
# Block port 8081 from external access
sudo ufw deny 8081
sudo ufw allow from 127.0.0.1 to any port 8081
```

Access the admin remotely via SSH tunnel:

```bash
ssh -L 8081:localhost:8081 user@yourserver
# Then open http://localhost:8081/pronoadmin in your browser
```

### 3. Add a password to the admin

The admin dashboard currently has no authentication. Until this is added, protect it with nginx basic auth:

```nginx
location /pronoadmin {
    auth_basic "Prono Admin";
    auth_basic_user_file /etc/nginx/.htpasswd;
    proxy_pass http://127.0.0.1:8081;
}
```

### 4. Run as a non-root user

The systemd unit file already sets `User=www-data`. On Windows, run NSSM as a restricted service account rather than `SYSTEM`.

### 5. Enable security headers

In the dashboard → **Headers** tab, enable:

- `X-Content-Type-Options` (enabled by default)
- `X-Frame-Options` (enabled by default)
- `X-XSS-Protection` (enabled by default)
- `HSTS` — enable once you have HTTPS set up
- `CSP` — set an appropriate policy for your site

---

## Default Cache Rules

Prono ships with these Cache-Control rules pre-configured:

| Extension | Cache-Control |
|-----------|---------------|
| `.html` | `no-store` |
| `.css` | `public, max-age=604800` (7 days) |
| `.js` | `public, max-age=604800` (7 days) |
| `.png` | `public, max-age=2592000` (30 days) |
| `.jpg` | `public, max-age=2592000` (30 days) |
| `.webp` | `public, max-age=2592000` (30 days) |
| `.woff2` | `public, max-age=31536000, immutable` (1 year) |
| `.json` | `public, max-age=600` (10 minutes) |

These can be modified or removed from the **Cache** tab in the dashboard.

---

## Known Limitations

| Area | Status | Notes |
|------|--------|-------|
| **HTTPS / TLS** | Not implemented | Use Nginx or Caddy as a TLS-terminating reverse proxy |
| **Gzip / Brotli compression** | Config only | Fields exist but compression is not applied. Requires linking zlib and adding deflate logic |
| **Reverse proxy** | Config only | Proxy rules can be added but are not forwarded. Requires implementing a TCP upstream connection |
| **Basic Auth** | Partial | Auth paths and users are stored and checked, but Base64 decoding of the `Authorization` header is a stub — accepts any `Basic` token |
| **Keep-alive** | Not implemented | Each connection closes after one response. Fine for most uses but increases latency for pages with many assets |
| **Config persistence** | Not implemented | All config is in-memory. Restarting Prono resets everything to defaults. Implement by serialising `CFG` to JSON on each change |
| **HTTP/2** | Not implemented | Prono speaks HTTP/1.1 only |
| **IPv6** | Not implemented | Binds on `0.0.0.0` (IPv4 only). Add a second acceptor on `tcp::v6()` to support IPv6 |
| **sendfile / zero-copy** | Not implemented | Large files are read into RAM before being written to the socket. Use `sendfile()` syscall for files over a few KB |
| **Thread pool** | Not implemented | One thread per connection. Under very high concurrency this exhausts OS thread limits. Replace with a fixed-size thread pool |
| **Admin authentication** | Not implemented | Anyone who can reach port 8081 controls the server. Protect with a firewall rule or nginx basic auth |

---

*Built by probloxworld · MIT License · C++17*
