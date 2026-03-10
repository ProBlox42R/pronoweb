/*
 * Prono Web Server — by probloxworld
 * Build:  g++ -std=c++17 -O2 -o prono prono_server.cpp -lboost_system -lpthread
 * Run:    ./prono [port] [webroot]
 * Access: http://<your-lan-ip>:8080/pronoadmin  (from any device on network)w lol ddddddddddddddddddddd
 */
#include <boost/asio.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <vector>
#include <map>
#include <set>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <algorithm>
#include <ctime>
#include <regex>
#include <unistd.h>   // gethostname
#include <limits.h>   // PATH_MAX
#include <libgen.h>   // dirname

using boost::asio::ip::tcp;
namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
// LIVE CONFIG — every field writable via dashboard
// ═══════════════════════════════════════════════════════════════════════════
struct RewriteRule { std::string pattern, replacement; int code = 302; };

struct Config {
    // Core
    std::string webroot       = "./www";
    int         port          = 8080;
    int         adminPort     = 8081;   // dashboard lives here
    std::string mainUrl       = "";     // e.g. http://192.168.1.10:8080 — used for file View links
    std::string serverName    = "Prono/1.0";
    std::string logFile       = "prono.log";
    std::string configFile    = "prono.cfg"; // absolute path set at startup
    int         maxLogEntries = 500;
    // Toggles
    bool dirListing    = true;
    bool accessLog     = true;
    bool colorConsole  = true;
    // Security headers
    bool xcto   = true;
    bool xframe = true;
    bool xss    = true;
    bool hsts   = false;
    bool csp    = false;
    std::string cspValue       = "default-src 'self'";
    std::string referrerPolicy = "strict-origin-when-cross-origin";
    // CORS
    bool corsEnabled     = false;
    bool corsCredentials = false;
    std::string corsOrigin  = "*";
    std::string corsMethods = "GET, POST, PUT, DELETE, OPTIONS";
    std::string corsHeaders = "Content-Type, Authorization";
    int  corsMaxAge         = 86400;
    // Custom headers name→value
    std::map<std::string,std::string> customHeaders;
    // Virtual hosts  hostname→docroot
    std::map<std::string,std::string> vhosts;
    // IP block/allow
    std::set<std::string> blockedIPs;
    // Allowed HTTP methods
    std::set<std::string> allowedMethods = {"GET","HEAD","POST","OPTIONS"};
    // Error page overrides  code→path-rel-to-webroot
    std::map<int,std::string> errorPages;
    // Rewrites
    std::vector<RewriteRule> rewrites;
    // Proxy rules  prefix→upstream
    std::map<std::string,std::string> proxyRules;
    // Rate limiting
    bool rateLimitEnabled = false;
    int  rateLimitPerMin  = 200;
    int  rateLimitBurst   = 40;
    // Compression (config only — real impl needs zlib)
    bool gzipEnabled   = false;
    int  gzipLevel     = 6;
    bool brotliEnabled = false;
    // Cache-Control per extension
    std::map<std::string,std::string> cacheRules = {
        {".html","no-store"},
        {".css","public, max-age=604800"},
        {".js","public, max-age=604800"},
        {".png","public, max-age=2592000"},
        {".jpg","public, max-age=2592000"},
        {".webp","public, max-age=2592000"},
        {".woff2","public, max-age=31536000, immutable"},
        {".json","public, max-age=600"},
    };
    // Auth
    bool basicAuthEnabled = false;
    std::string authRealm = "Restricted";
    std::map<std::string,std::string> authUsers;
    std::vector<std::string> authPaths;
};

Config     CFG;
std::mutex cfgMutex;

// ═══════════════════════════════════════════════════════════════════════════
// RUNTIME COUNTERS
// ═══════════════════════════════════════════════════════════════════════════
std::mutex               logMutex;
std::vector<std::string> accessLogs;

std::atomic<long long> requestCount{0};
std::atomic<long long> bytesSent{0};
std::atomic<long long> bytesRecv{0};
std::atomic<long long> err4xx{0};
std::atomic<long long> err5xx{0};
std::atomic<long long> req2xx{0};

auto startTime = std::chrono::steady_clock::now();

std::mutex                statsMutex;
std::map<std::string,int> pathHits;
std::map<std::string,int> ipHits;
std::map<int,int>         statusCounts;
std::map<std::string,int> methodCounts;

// Rate-limit state: ip → {count, window_start_uptime_secs}
std::mutex                                              rlMutex;
std::map<std::string,std::pair<int,long long>>          rlState;

// ═══════════════════════════════════════════════════════════════════════════
// UTILITY
// ═══════════════════════════════════════════════════════════════════════════
std::string readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
}
bool writeFileDisk(const std::string& p, const std::string& c)
{
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream f(p);
    if (!f) return false;
    f << c;
    return f.good();
}
// ═══════════════════════════════════════════════════════════════════════════
// CONFIG PERSISTENCE  — simple key=value flat file, always absolute path
// ═══════════════════════════════════════════════════════════════════════════
void saveConfig()
{
    // Called with cfgMutex already held — do NOT lock again
    std::ofstream f(CFG.configFile);
    if(!f)return;
    f<<"webroot="     <<CFG.webroot      <<"\n";
    f<<"port="        <<CFG.port         <<"\n";
    f<<"adminPort="   <<CFG.adminPort    <<"\n";
    f<<"mainUrl="     <<CFG.mainUrl      <<"\n";
    f<<"serverName="  <<CFG.serverName   <<"\n";
    f<<"logFile="     <<CFG.logFile      <<"\n";
    f<<"maxLogEntries="<<CFG.maxLogEntries<<"\n";
    f<<"dirListing="  <<(CFG.dirListing?1:0)<<"\n";
    f<<"accessLog="   <<(CFG.accessLog?1:0)<<"\n";
    f<<"colorConsole="<<(CFG.colorConsole?1:0)<<"\n";
    f<<"xcto="        <<(CFG.xcto?1:0)   <<"\n";
    f<<"xframe="      <<(CFG.xframe?1:0) <<"\n";
    f<<"xss="         <<(CFG.xss?1:0)    <<"\n";
    f<<"hsts="        <<(CFG.hsts?1:0)   <<"\n";
    f<<"csp="         <<(CFG.csp?1:0)    <<"\n";
    f<<"cspValue="    <<CFG.cspValue     <<"\n";
    f<<"referrerPolicy="<<CFG.referrerPolicy<<"\n";
    f<<"corsEnabled=" <<(CFG.corsEnabled?1:0)<<"\n";
    f<<"corsCredentials="<<(CFG.corsCredentials?1:0)<<"\n";
    f<<"corsOrigin="  <<CFG.corsOrigin   <<"\n";
    f<<"corsMethods=" <<CFG.corsMethods  <<"\n";
    f<<"corsHeaders=" <<CFG.corsHeaders  <<"\n";
    f<<"corsMaxAge="  <<CFG.corsMaxAge   <<"\n";
    f<<"rateLimitEnabled="<<(CFG.rateLimitEnabled?1:0)<<"\n";
    f<<"rateLimitPerMin=" <<CFG.rateLimitPerMin<<"\n";
    f<<"rateLimitBurst="  <<CFG.rateLimitBurst<<"\n";
    f<<"gzipEnabled=" <<(CFG.gzipEnabled?1:0)<<"\n";
    f<<"gzipLevel="   <<CFG.gzipLevel    <<"\n";
    f<<"brotliEnabled="<<(CFG.brotliEnabled?1:0)<<"\n";
    f<<"basicAuthEnabled="<<(CFG.basicAuthEnabled?1:0)<<"\n";
    f<<"authRealm="   <<CFG.authRealm    <<"\n";
    // Allowed methods
    for(auto& m:CFG.allowedMethods)f<<"allowedMethod="<<m<<"\n";
    // blocked IPs
    for(auto& ip:CFG.blockedIPs)f<<"blockedIP="<<ip<<"\n";
    // vhosts
    for(auto& kv:CFG.vhosts)f<<"vhost="<<kv.first<<"="<<kv.second<<"\n";
    // custom headers
    for(auto& kv:CFG.customHeaders)f<<"customHeader="<<kv.first<<"="<<kv.second<<"\n";
    // cache rules
    for(auto& kv:CFG.cacheRules)f<<"cacheRule="<<kv.first<<"="<<kv.second<<"\n";
    // error pages
    for(auto& kv:CFG.errorPages)f<<"errorPage="<<kv.first<<"="<<kv.second<<"\n";
    // auth users
    for(auto& kv:CFG.authUsers)f<<"authUser="<<kv.first<<"="<<kv.second<<"\n";
    // auth paths
    for(auto& p:CFG.authPaths)f<<"authPath="<<p<<"\n";
    // rewrite rules
    for(auto& r:CFG.rewrites)f<<"rewrite="<<r.pattern<<"|"<<r.replacement<<"|"<<r.code<<"\n";
    // proxy rules
    for(auto& kv:CFG.proxyRules)f<<"proxyRule="<<kv.first<<"="<<kv.second<<"\n";
}

void loadConfig(const std::string& path)
{
    std::ifstream f(path);
    if(!f)return;
    // Reset collections so we don't double-load
    CFG.allowedMethods.clear();
    CFG.blockedIPs.clear();
    CFG.vhosts.clear();
    CFG.customHeaders.clear();
    CFG.cacheRules.clear();
    CFG.errorPages.clear();
    CFG.authUsers.clear();
    CFG.authPaths.clear();
    CFG.rewrites.clear();
    CFG.proxyRules.clear();

    std::string line;
    while(std::getline(f,line)){
        if(line.empty()||line[0]=='#')continue;
        auto eq=line.find('=');
        if(eq==std::string::npos)continue;
        std::string k=line.substr(0,eq);
        std::string v=line.substr(eq+1);
        if(k=="webroot")          CFG.webroot=v;
        else if(k=="port")        try{CFG.port=std::stoi(v);}catch(...){}
        else if(k=="adminPort")   try{CFG.adminPort=std::stoi(v);}catch(...){}
        else if(k=="mainUrl")     CFG.mainUrl=v;
        else if(k=="serverName")  CFG.serverName=v;
        else if(k=="logFile")     CFG.logFile=v;
        else if(k=="maxLogEntries")try{CFG.maxLogEntries=std::stoi(v);}catch(...){}
        else if(k=="dirListing")  CFG.dirListing=v=="1";
        else if(k=="accessLog")   CFG.accessLog=v=="1";
        else if(k=="colorConsole")CFG.colorConsole=v=="1";
        else if(k=="xcto")        CFG.xcto=v=="1";
        else if(k=="xframe")      CFG.xframe=v=="1";
        else if(k=="xss")         CFG.xss=v=="1";
        else if(k=="hsts")        CFG.hsts=v=="1";
        else if(k=="csp")         CFG.csp=v=="1";
        else if(k=="cspValue")    CFG.cspValue=v;
        else if(k=="referrerPolicy")CFG.referrerPolicy=v;
        else if(k=="corsEnabled") CFG.corsEnabled=v=="1";
        else if(k=="corsCredentials")CFG.corsCredentials=v=="1";
        else if(k=="corsOrigin")  CFG.corsOrigin=v;
        else if(k=="corsMethods") CFG.corsMethods=v;
        else if(k=="corsHeaders") CFG.corsHeaders=v;
        else if(k=="corsMaxAge")  try{CFG.corsMaxAge=std::stoi(v);}catch(...){}
        else if(k=="rateLimitEnabled")CFG.rateLimitEnabled=v=="1";
        else if(k=="rateLimitPerMin") try{CFG.rateLimitPerMin=std::stoi(v);}catch(...){}
        else if(k=="rateLimitBurst")  try{CFG.rateLimitBurst=std::stoi(v);}catch(...){}
        else if(k=="gzipEnabled") CFG.gzipEnabled=v=="1";
        else if(k=="gzipLevel")   try{CFG.gzipLevel=std::stoi(v);}catch(...){}
        else if(k=="brotliEnabled")CFG.brotliEnabled=v=="1";
        else if(k=="basicAuthEnabled")CFG.basicAuthEnabled=v=="1";
        else if(k=="authRealm")   CFG.authRealm=v;
        else if(k=="allowedMethod")CFG.allowedMethods.insert(v);
        else if(k=="blockedIP")   CFG.blockedIPs.insert(v);
        else if(k=="vhost"){
            auto p2=v.find('=');
            if(p2!=std::string::npos)CFG.vhosts[v.substr(0,p2)]=v.substr(p2+1);
        }
        else if(k=="customHeader"){
            auto p2=v.find('=');
            if(p2!=std::string::npos)CFG.customHeaders[v.substr(0,p2)]=v.substr(p2+1);
        }
        else if(k=="cacheRule"){
            auto p2=v.find('=');
            if(p2!=std::string::npos)CFG.cacheRules[v.substr(0,p2)]=v.substr(p2+1);
        }
        else if(k=="errorPage"){
            auto p2=v.find('=');
            if(p2!=std::string::npos)try{CFG.errorPages[std::stoi(v.substr(0,p2))]=v.substr(p2+1);}catch(...){}
        }
        else if(k=="authUser"){
            auto p2=v.find('=');
            if(p2!=std::string::npos)CFG.authUsers[v.substr(0,p2)]=v.substr(p2+1);
        }
        else if(k=="authPath")    CFG.authPaths.push_back(v);
        else if(k=="rewrite"){
            auto p1=v.find('|');
            if(p1!=std::string::npos){
                auto p2=v.find('|',p1+1);
                if(p2!=std::string::npos){
                    RewriteRule r;
                    r.pattern=v.substr(0,p1);
                    r.replacement=v.substr(p1+1,p2-p1-1);
                    try{r.code=std::stoi(v.substr(p2+1));}catch(...){r.code=302;}
                    CFG.rewrites.push_back(r);
                }
            }
        }
        else if(k=="proxyRule"){
            auto p2=v.find('=');
            if(p2!=std::string::npos)CFG.proxyRules[v.substr(0,p2)]=v.substr(p2+1);
        }
    }
    // Ensure allowedMethods has sane defaults if file was empty/partial
    if(CFG.allowedMethods.empty())
        CFG.allowedMethods={"GET","HEAD","POST","OPTIONS"};
    // Ensure cache rules populated if empty
    if(CFG.cacheRules.empty())
        CFG.cacheRules={
            {".html","no-store"},{".css","public, max-age=604800"},
            {".js","public, max-age=604800"},{".png","public, max-age=2592000"},
            {".jpg","public, max-age=2592000"},{".webp","public, max-age=2592000"},
            {".woff2","public, max-age=31536000, immutable"},{".json","public, max-age=600"},
        };
}

long uptimeSecs()
{
    return (long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime).count();
}
std::string fmtUptime(long s)
{
    long d=s/86400, h=(s%86400)/3600, m=(s%3600)/60, sc=s%60;
    std::ostringstream o;
    if(d)o<<d<<"d ";
    if(h)o<<h<<"h ";
    if(m)o<<m<<"m ";
    o<<sc<<"s";
    return o.str();
}
std::string fmtBytes(long long b)
{
    if(b<1024)return std::to_string(b)+" B";
    if(b<1048576)return std::to_string(b/1024)+" KB";
    if(b<1073741824)return std::to_string(b/1048576)+" MB";
    return std::to_string(b/1073741824)+" GB";
}
std::string nowTs()
{
    auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32]; std::strftime(buf,sizeof(buf),"%d/%b/%Y:%H:%M:%S",std::localtime(&t));
    return buf;
}
std::string escH(const std::string& s)
{
    std::string o; o.reserve(s.size());
    for(char c:s){
        if(c=='<')o+="&lt;";
        else if(c=='>')o+="&gt;";
        else if(c=='&')o+="&amp;";
        else if(c=='"')o+="&quot;";
        else if(c=='\'')o+="&#39;";
        else o+=c;
    }
    return o;
}
std::string urlDec(const std::string& s)
{
    std::string o; o.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='%'&&i+2<s.size()){
            o+=(char)std::stoi(s.substr(i+1,2),nullptr,16);
            i+=2;
        } else if(s[i]=='+') o+=' ';
        else o+=s[i];
    }
    return o;
}
std::map<std::string,std::string> parseQS(const std::string& raw)
{
    std::map<std::string,std::string> p;
    std::string q=raw;
    if(!q.empty()&&q[0]=='?')q=q.substr(1);
    std::istringstream ss(q);
    std::string tok;
    while(std::getline(ss,tok,'&')){
        auto eq=tok.find('=');
        if(eq!=std::string::npos)
            p[urlDec(tok.substr(0,eq))]=urlDec(tok.substr(eq+1));
        else if(!tok.empty())
            p[urlDec(tok)]="";
    }
    return p;
}
bool safePath(const std::string& p)
{
    if(p.find("..")!=std::string::npos)return false;
    if(p.find("//")!=std::string::npos)return false;
    for(auto& seg:fs::path(p)){
        auto s=seg.string();
        if(!s.empty()&&s[0]=='.'&&s!="."&&s!="..")return false;
    }
    return true;
}
bool rlAllow(const std::string& ip)
{
    if(!CFG.rateLimitEnabled)return true;
    auto now=uptimeSecs();
    std::lock_guard<std::mutex> lk(rlMutex);
    auto& st=rlState[ip];
    if(now-st.second>=60){st.first=0;st.second=now;}
    if(st.first>=CFG.rateLimitPerMin+CFG.rateLimitBurst)return false;
    st.first++;
    return true;
}
void addLog(const std::string& e)
{
    std::lock_guard<std::mutex> lk(logMutex);
    accessLogs.push_back(e);
    if((int)accessLogs.size()>CFG.maxLogEntries)accessLogs.erase(accessLogs.begin());
    if(CFG.accessLog){std::ofstream f(CFG.logFile,std::ios::app);f<<e<<"\n";}
}
std::string mimeType(const std::string& path)
{
    auto ext=fs::path(path).extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
    if(ext==".html"||ext==".htm") return "text/html; charset=utf-8";
    if(ext==".css")               return "text/css; charset=utf-8";
    if(ext==".js"||ext==".mjs")  return "application/javascript; charset=utf-8";
    if(ext==".json")              return "application/json";
    if(ext==".xml")               return "application/xml";
    if(ext==".txt"||ext==".md")  return "text/plain; charset=utf-8";
    if(ext==".csv")               return "text/csv";
    if(ext==".png")               return "image/png";
    if(ext==".jpg"||ext==".jpeg")return "image/jpeg";
    if(ext==".gif")               return "image/gif";
    if(ext==".svg")               return "image/svg+xml";
    if(ext==".ico")               return "image/x-icon";
    if(ext==".webp")              return "image/webp";
    if(ext==".avif")              return "image/avif";
    if(ext==".mp4")               return "video/mp4";
    if(ext==".webm")              return "video/webm";
    if(ext==".mp3")               return "audio/mpeg";
    if(ext==".ogg")               return "audio/ogg";
    if(ext==".wav")               return "audio/wav";
    if(ext==".woff")              return "font/woff";
    if(ext==".woff2")             return "font/woff2";
    if(ext==".ttf")               return "font/ttf";
    if(ext==".otf")               return "font/otf";
    if(ext==".pdf")               return "application/pdf";
    if(ext==".zip")               return "application/zip";
    if(ext==".tar"||ext==".gz")  return "application/x-tar";
    if(ext==".wasm")              return "application/wasm";
    return "application/octet-stream";
}
std::string fileIcon(const std::string& ext)
{
    if(ext==".html"||ext==".htm")return "📄";
    if(ext==".css")              return "🎨";
    if(ext==".js"||ext==".mjs") return "⚡";
    if(ext==".json"||ext==".xml"||ext==".csv")return "📋";
    if(ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".gif"
      ||ext==".webp"||ext==".svg"||ext==".ico"||ext==".avif")return "🖼";
    if(ext==".mp4"||ext==".webm"||ext==".mov")return "🎬";
    if(ext==".mp3"||ext==".ogg"||ext==".wav")return "🎵";
    if(ext==".pdf")              return "📕";
    if(ext==".zip"||ext==".gz"||ext==".tar"||ext==".7z")return "📦";
    if(ext==".woff"||ext==".woff2"||ext==".ttf"||ext==".otf")return "🔤";
    if(ext==".md"||ext==".txt"||ext==".log")return "📝";
    if(ext==".sh"||ext==".bash")return "🖥";
    if(ext==".cpp"||ext==".c"||ext==".h"||ext==".py"||ext==".rs")return "🔧";
    return "📄";
}
std::string tagCls(const std::string& ext)
{
    if(ext==".html"||ext==".htm")return "t-blue";
    if(ext==".css")              return "t-purple";
    if(ext==".js"||ext==".mjs") return "t-orange";
    if(ext==".json"||ext==".xml")return "t-cyan";
    if(ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".gif"||ext==".webp"||ext==".svg")return "t-green";
    if(ext==".mp4"||ext==".mp3")return "t-pink";
    if(ext==".pdf")              return "t-red";
    return "t-gray";
}

// ═══════════════════════════════════════════════════════════════════════════
// SHARED CSS
// ═══════════════════════════════════════════════════════════════════════════
static const std::string CSS = R"RAWCSS(
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=Space+Grotesk:wght@500;600;700;800&family=JetBrains+Mono:wght@400;500&display=swap');
:root{
  --bg:#07090f;--bg2:#0c0f19;--bg3:#111520;--bg4:#181d2c;
  --border:#1c2236;--border2:#242d44;--border3:#2e3a55;
  --text:#e2e6f3;--text2:#8892b0;--text3:#4a5578;--text4:#2d3654;
  --accent:#5b9cf6;--accent2:#3d7fee;
  --green:#10d48e;--green2:#0ab87a;
  --orange:#f4823a;--yellow:#f5c842;
  --red:#f0485a;--red2:#d93448;
  --cyan:#18cfe8;--purple:#9b7ef8;--pink:#f472b6;
}
*{margin:0;padding:0;box-sizing:border-box;}
html{scroll-behavior:smooth;}
body{font-family:'Inter',system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;font-size:14px;line-height:1.6;}
a{color:var(--accent);text-decoration:none;}
a:hover{color:var(--accent2);}
::-webkit-scrollbar{width:6px;height:6px;}
::-webkit-scrollbar-track{background:var(--bg);}
::-webkit-scrollbar-thumb{background:var(--border3);border-radius:3px;}
.mono{font-family:'JetBrains Mono',monospace;}

.tag{display:inline-flex;align-items:center;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:600;}
.t-green{background:rgba(16,212,142,.1);color:var(--green);border:1px solid rgba(16,212,142,.2);}
.t-red{background:rgba(240,72,90,.1);color:var(--red);border:1px solid rgba(240,72,90,.2);}
.t-orange{background:rgba(244,130,58,.1);color:var(--orange);border:1px solid rgba(244,130,58,.2);}
.t-blue{background:rgba(91,156,246,.1);color:var(--accent);border:1px solid rgba(91,156,246,.2);}
.t-cyan{background:rgba(24,207,232,.1);color:var(--cyan);border:1px solid rgba(24,207,232,.2);}
.t-purple{background:rgba(155,126,248,.1);color:var(--purple);border:1px solid rgba(155,126,248,.2);}
.t-pink{background:rgba(244,114,182,.1);color:var(--pink);border:1px solid rgba(244,114,182,.2);}
.t-gray{background:var(--bg4);color:var(--text3);border:1px solid var(--border2);}

.btn{padding:8px 16px;border-radius:8px;font-family:'Inter',sans-serif;font-size:13px;font-weight:500;
  cursor:pointer;border:1px solid var(--border2);background:var(--bg3);color:var(--text);
  transition:all .15s;display:inline-flex;align-items:center;gap:6px;text-decoration:none;white-space:nowrap;}
.btn:hover{border-color:var(--accent);color:var(--accent);}
.btn-primary{background:var(--accent2);border-color:var(--accent2);color:#fff;font-weight:600;}
.btn-primary:hover{background:var(--accent);border-color:var(--accent);color:#fff;}
.btn-danger{border-color:var(--red);color:var(--red);background:rgba(240,72,90,.05);}
.btn-danger:hover{background:var(--red);color:#fff;}
.btn-success{border-color:var(--green);color:var(--green);background:rgba(16,212,142,.05);}
.btn-success:hover{background:var(--green);color:#000;}
.btn-sm{padding:4px 11px;font-size:12px;}

.card{background:var(--bg2);border:1px solid var(--border);border-radius:12px;padding:20px;}
.card-title{font-family:'Space Grotesk',sans-serif;font-weight:700;font-size:14px;
  margin-bottom:16px;display:flex;align-items:center;justify-content:space-between;gap:8px;}

.tbl-wrap{overflow-x:auto;}
table{width:100%;border-collapse:collapse;font-size:13px;}
th{text-align:left;padding:10px 14px;background:var(--bg3);color:var(--text3);
   font-size:11px;text-transform:uppercase;letter-spacing:.8px;font-weight:500;border-bottom:1px solid var(--border);}
td{padding:10px 14px;border-bottom:1px solid var(--border);color:var(--text2);}
tr:hover td{background:rgba(255,255,255,.012);color:var(--text);}
tr:last-child td{border-bottom:none;}

.fg{margin-bottom:14px;}
.fl{font-size:12px;color:var(--text2);margin-bottom:5px;display:block;font-weight:500;}
.fi,.fs,.ft{width:100%;background:var(--bg3);border:1px solid var(--border2);color:var(--text);
  padding:9px 13px;border-radius:8px;font-family:'Inter',sans-serif;font-size:13px;outline:none;transition:border .15s;}
.fi:focus,.fs:focus,.ft:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(91,156,246,.08);}
.fs option{background:var(--bg3);}
.ft{resize:vertical;min-height:80px;line-height:1.6;}
.fr{display:grid;grid-template-columns:1fr 1fr;gap:12px;}
.fr3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;}

.tog-row{display:flex;align-items:center;justify-content:space-between;
  padding:11px 14px;background:var(--bg3);border:1px solid var(--border);border-radius:9px;margin-bottom:8px;}
.tog-info .tt{font-size:13px;color:var(--text);font-weight:500;}
.tog-info .td{font-size:11px;color:var(--text3);margin-top:2px;}
.tog{width:40px;height:22px;background:var(--bg4);border-radius:11px;cursor:pointer;
  position:relative;transition:background .2s;border:1px solid var(--border2);flex-shrink:0;}
.tog.on{background:var(--accent2);border-color:var(--accent2);}
.tog::after{content:'';position:absolute;top:2px;left:2px;width:16px;height:16px;
  background:#fff;border-radius:50%;transition:left .2s;box-shadow:0 1px 3px rgba(0,0,0,.4);}
.tog.on::after{left:20px;}

.notice{padding:11px 15px;border-radius:9px;font-size:13px;margin-bottom:14px;display:flex;align-items:flex-start;gap:9px;}
.n-warn{background:rgba(245,200,66,.07);border:1px solid rgba(245,200,66,.2);color:var(--yellow);}
.n-info{background:rgba(91,156,246,.07);border:1px solid rgba(91,156,246,.2);color:var(--accent);}
.n-err{background:rgba(240,72,90,.07);border:1px solid rgba(240,72,90,.2);color:var(--red);}
.n-ok{background:rgba(16,212,142,.07);border:1px solid rgba(16,212,142,.2);color:var(--green);}

.ce{background:var(--bg);border:1px solid var(--border2);border-radius:10px;overflow:hidden;}
.ce-h{background:var(--bg3);padding:10px 15px;display:flex;align-items:center;
  justify-content:space-between;border-bottom:1px solid var(--border);}
.ce-fn{font-size:12px;color:var(--accent);font-family:'JetBrains Mono',monospace;}
textarea.ca{width:100%;background:transparent;border:none;color:var(--text);
  font-family:'JetBrains Mono',monospace;font-size:12.5px;line-height:1.75;
  outline:none;resize:vertical;padding:14px 16px;min-height:180px;}

.grid{display:grid;gap:16px;}
.g2{grid-template-columns:1fr 1fr;}
.g3{grid-template-columns:1fr 1fr 1fr;}
.g4{grid-template-columns:repeat(4,1fr);}
@media(max-width:1100px){.g4{grid-template-columns:1fr 1fr;}}
@media(max-width:800px){.g3,.g4{grid-template-columns:1fr 1fr;}}
@media(max-width:540px){.g2,.g3,.g4{grid-template-columns:1fr;}.fr{grid-template-columns:1fr;}}

.bw{margin-bottom:12px;}
.bl{display:flex;justify-content:space-between;font-size:12px;color:var(--text2);margin-bottom:4px;}
.bar{height:5px;background:var(--bg4);border-radius:3px;overflow:hidden;}
.bf{height:100%;border-radius:3px;transition:width .7s cubic-bezier(.4,0,.2,1);}
.c-blue{background:linear-gradient(90deg,var(--accent2),var(--cyan));}
.c-green{background:linear-gradient(90deg,var(--green2),#6ee7b7);}
.c-orange{background:linear-gradient(90deg,var(--orange),var(--yellow));}
.c-red{background:linear-gradient(90deg,var(--red2),#fb7185);}

.spark{display:flex;align-items:flex-end;gap:3px;height:40px;margin-top:12px;}
.sb{flex:1;background:rgba(91,156,246,.15);border-radius:2px 2px 0 0;}
.sb.hi{background:var(--accent2);}

.mc{background:var(--bg2);border:1px solid var(--border);border-radius:12px;padding:20px;position:relative;overflow:hidden;}
.mc::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;}
.mc.blue::before{background:linear-gradient(90deg,var(--accent2),var(--cyan));}
.mc.green::before{background:linear-gradient(90deg,var(--green2),#6ee7b7);}
.mc.orange::before{background:linear-gradient(90deg,var(--orange),var(--yellow));}
.mc.red::before{background:linear-gradient(90deg,var(--red2),#fb7185);}
.mc-val{font-family:'Space Grotesk',sans-serif;font-weight:800;font-size:32px;line-height:1;margin-bottom:4px;}
.mc-key{font-size:11px;color:var(--text3);text-transform:uppercase;letter-spacing:1px;font-weight:500;}
.mc-sub{font-size:12px;color:var(--text2);margin-top:8px;}

.lw{background:var(--bg);border:1px solid var(--border2);border-radius:10px;overflow:hidden;}
.lh{background:var(--bg3);padding:10px 15px;display:flex;align-items:center;
  justify-content:space-between;border-bottom:1px solid var(--border);font-size:12px;color:var(--text2);}
.lb{padding:10px 14px;overflow-y:auto;font-family:'JetBrains Mono',monospace;font-size:12px;line-height:1.9;}
.ll{padding:1px 2px;}
.l2{color:var(--green);}
.l3{color:var(--accent);}
.l4{color:var(--orange);}
.l5{color:var(--red);}

hr.dv{border:none;border-top:1px solid var(--border);margin:16px 0;}
.sec-title{font-family:'Space Grotesk',sans-serif;font-weight:800;font-size:20px;margin-bottom:3px;}
.sec-desc{font-size:13px;color:var(--text3);margin-bottom:20px;}

/* toast */
#toast{position:fixed;bottom:24px;right:24px;z-index:9999;display:flex;flex-direction:column;gap:8px;pointer-events:none;}
.tmsg{padding:12px 18px;border-radius:9px;font-size:13px;font-weight:500;
  animation:tsl .25s ease;box-shadow:0 4px 20px rgba(0,0,0,.4);pointer-events:auto;}
@keyframes tsl{from{opacity:0;transform:translateX(40px)}to{opacity:1;transform:translateX(0)}}
.tok{background:rgba(16,212,142,.15);border:1px solid rgba(16,212,142,.3);color:var(--green);}
.terr{background:rgba(240,72,90,.15);border:1px solid rgba(240,72,90,.3);color:var(--red);}
.tinfo{background:rgba(91,156,246,.15);border:1px solid rgba(91,156,246,.3);color:var(--accent);}
)RAWCSS";

// ═══════════════════════════════════════════════════════════════════════════
// LOG HTML
// ═══════════════════════════════════════════════════════════════════════════
std::string logsHtml()
{
    std::lock_guard<std::mutex> lk(logMutex);
    std::string o;
    for(int i=(int)accessLogs.size()-1;i>=0;--i){
        const auto& l=accessLogs[i];
        std::string c="ll ";
        if(l.find("404")!=std::string::npos||l.find("403")!=std::string::npos)c+="l4";
        else if(l.find("50")!=std::string::npos)c+="l5";
        else if(l.find("30")!=std::string::npos)c+="l3";
        else c+="l2";
        o+="<div class='"+c+"'>"+escH(l)+"</div>\n";
    }
    return o;
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE BROWSER ROWS
// ═══════════════════════════════════════════════════════════════════════════
std::string fileRows(const std::string& relDir)
{
    std::string absDir=CFG.webroot+(relDir.empty()?"":"/"+relDir);
    if(!fs::exists(absDir)||!fs::is_directory(absDir))
        return "<tr><td colspan='6' style='text-align:center;color:var(--text3);padding:20px'>Directory not found</td></tr>";

    std::vector<fs::directory_entry> dirs,files;
    try{
        for(auto& e:fs::directory_iterator(absDir)){
            if(e.is_directory())dirs.push_back(e);
            else files.push_back(e);
        }
    }catch(...){
        return "<tr><td colspan='6' style='color:var(--red)'>Permission denied</td></tr>";
    }
    auto cmp=[](auto& a,auto& b){return a.path().filename()<b.path().filename();};
    std::sort(dirs.begin(),dirs.end(),cmp);
    std::sort(files.begin(),files.end(),cmp);

    std::string rows;

    // Parent dir
    if(!relDir.empty()){
        std::string par=relDir.find('/')!=std::string::npos?relDir.substr(0,relDir.rfind('/')):"";
        rows+="<tr><td>📂</td><td><a href='/pronoadmin/files?dir="+escH(par)
             +"' style='color:var(--accent)'>..</a></td>"
             "<td>—</td><td><span class='tag t-gray'>DIR</span></td><td>—</td><td></td></tr>";
    }
    for(auto& d:dirs){
        std::string name=d.path().filename().string();
        std::string sub=relDir.empty()?name:relDir+"/"+name;
        rows+="<tr><td>📂</td><td><a href='/pronoadmin/files?dir="+escH(sub)
             +"' style='color:var(--accent)'>"+escH(name)+"</a></td>"
             "<td>—</td><td><span class='tag t-gray'>DIR</span></td><td>—</td><td></td></tr>";
    }
    for(auto& f:files){
        std::string name=f.path().filename().string();
        std::string ext=f.path().extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        std::string ico=fileIcon(ext);
        std::string tc=tagCls(ext);
        std::string eu=ext.empty()?"FILE":ext.substr(1);
        std::transform(eu.begin(),eu.end(),eu.begin(),::toupper);
        long long sz=0;
        try{sz=(long long)fs::file_size(f.path());}catch(...){}
        std::string mod="—";
        try{
            auto lwt=fs::last_write_time(f.path());
            auto sc=std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                lwt-fs::file_time_type::clock::now()+std::chrono::system_clock::now());
            std::time_t tt=std::chrono::system_clock::to_time_t(sc);
            char tb[32];std::strftime(tb,sizeof(tb),"%Y-%m-%d %H:%M",std::localtime(&tt));
            mod=tb;
        }catch(...){}
        std::string wp="/"+(relDir.empty()?"":relDir+"/")+name;
        // Build absolute URL for View — always points to the web server, not the admin port
        std::string viewUrl;
        {
            std::lock_guard<std::mutex> lk(cfgMutex);
            if(!CFG.mainUrl.empty()){
                // strip trailing slash from mainUrl
                std::string base=CFG.mainUrl;
                while(!base.empty()&&base.back()=='/')base.pop_back();
                viewUrl=base+wp;
            }else{
                // Fall back: same host, web port
                viewUrl="http://127.0.0.1:"+std::to_string(CFG.port)+wp;
            }
        }
        rows+="<tr>"
            "<td>"+ico+"</td>"
            "<td style='color:var(--text)'>"+escH(name)+"</td>"
            "<td>"+fmtBytes(sz)+"</td>"
            "<td><span class='tag "+tc+"'>"+escH(eu)+"</span></td>"
            "<td>"+mod+"</td>"
            "<td style='display:flex;gap:5px;padding:6px 12px'>"
            "<a href='"+escH(viewUrl)+"' target='_blank' class='btn btn-sm'>View</a>"
            "<a href='/pronoadmin/download?file="+escH(wp)+"' class='btn btn-sm'>⬇</a>"
            "<a href='/pronoadmin/delete?file="+escH(wp)+"&dir="+escH(relDir)
            +"' class='btn btn-sm btn-danger' onclick=\"return confirm('Delete "+escH(name)+"?')\">✕</a>"
            "</td></tr>";
    }
    if(dirs.empty()&&files.empty())
        rows="<tr><td colspan='6' style='text-align:center;color:var(--text3);padding:20px'>Empty directory</td></tr>";
    return rows;
}

// ═══════════════════════════════════════════════════════════════════════════
// ERROR PAGES
// ═══════════════════════════════════════════════════════════════════════════
struct EI{int code;std::string title,desc,emoji,color;};
static const std::vector<EI> ERRS={
  {400,"Bad Request",            "The server could not understand the request syntax.",         "⚠", "#f4823a"},
  {401,"Unauthorized",           "Authentication is required to access this resource.",         "🔒","#f4823a"},
  {403,"Forbidden",              "You do not have permission to access this resource.",         "🚫","#f4823a"},
  {404,"Not Found",              "The page you are looking for does not exist.",                "🔍","#f4823a"},
  {405,"Method Not Allowed",     "The HTTP method is not supported for this endpoint.",         "❌","#f4823a"},
  {408,"Request Timeout",        "The server timed out waiting for the request.",               "⏱","#f4823a"},
  {409,"Conflict",               "The request conflicts with the current state.",               "⚡","#f4823a"},
  {410,"Gone",                   "This resource has been permanently removed.",                 "🗑","#f4823a"},
  {413,"Payload Too Large",      "The request body exceeds the server size limit.",             "📦","#f4823a"},
  {429,"Too Many Requests",      "You have sent too many requests. Please slow down.",          "🚦","#f4823a"},
  {451,"Unavailable For Legal",  "This content was removed for legal reasons.",                 "⚖","#f4823a"},
  {500,"Internal Server Error",  "Something went wrong on our end.",                            "💥","#f0485a"},
  {501,"Not Implemented",        "The server does not support this functionality.",             "🔧","#f0485a"},
  {502,"Bad Gateway",            "An invalid response was received from an upstream server.",   "🔀","#f0485a"},
  {503,"Service Unavailable",    "The server is temporarily unable to handle requests.",        "🔴","#f0485a"},
  {504,"Gateway Timeout",        "The upstream server did not respond in time.",                "⏰","#f0485a"},
  {505,"HTTP Version Not Supported","The HTTP version in the request is not supported.",        "📡","#f0485a"},
  {507,"Insufficient Storage",   "The server cannot store the needed representation.",          "💾","#f0485a"},
  {508,"Loop Detected",          "The server detected an infinite loop while processing.",      "∞","#f0485a"},
  {520,"Unknown Error",          "The upstream server returned an unknown error.",              "❓","#f0485a"},
  {521,"Web Server Down",        "The origin web server is currently offline.",                 "🔌","#f0485a"},
  {522,"Connection Timed Out",   "Connection to the origin server timed out.",                  "⌛","#f0485a"},
  {523,"Origin Unreachable",     "The origin server is unreachable.",                           "🌐","#f0485a"},
  {524,"A Timeout Occurred",     "A timeout occurred connecting to the origin.",                "⏳","#f0485a"},
  {525,"SSL Handshake Failed",   "The SSL handshake between server and origin failed.",         "🔐","#f0485a"},
  {526,"Invalid SSL Certificate","The origin SSL certificate is invalid.",                      "🔑","#f0485a"},
  {530,"Site Frozen",            "The site has been frozen due to inactivity.",                 "❄","#f0485a"},
  {540,"Temporarily Disabled",   "This resource has been disabled by the administrator.",       "🛑","#f0485a"},
};

std::string errorPage(int code,const std::string& detail="")
{
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        auto it=CFG.errorPages.find(code);
        if(it!=CFG.errorPages.end()){
            std::string c=readFile(CFG.webroot+it->second);
            if(!c.empty())return c;
        }
    }
    std::string title="Error",desc="An error occurred.",emoji="⚠",color="#f4823a";
    for(auto& e:ERRS)if(e.code==code){title=e.title;desc=e.desc;emoji=e.emoji;color=e.color;break;}
    if(!detail.empty())desc=detail;
    return "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>"+std::to_string(code)+" "+title+"</title>"
        "<style>"+CSS+R"CSS(
body{display:flex;flex-direction:column;min-height:100vh;}
.em{flex:1;display:flex;align-items:center;justify-content:center;padding:40px 20px;}
.eb{text-align:center;max-width:520px;}
.ee{font-size:72px;margin-bottom:20px;animation:drop .5s cubic-bezier(.175,.885,.32,1.275);}
@keyframes drop{from{opacity:0;transform:translateY(-30px)}to{opacity:1;transform:translateY(0)}}
)CSS"
        +".ec{font-family:'Space Grotesk',sans-serif;font-weight:800;font-size:96px;line-height:1;margin-bottom:12px;"
        "background:linear-gradient(135deg,"+color+",#f5c842);"
        "-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}"
        +R"CSS(
.et{font-family:'Space Grotesk',sans-serif;font-weight:700;font-size:24px;color:var(--text);margin-bottom:10px;}
.ed{font-size:14px;color:var(--text2);line-height:1.7;margin-bottom:28px;}
.edtl{font-family:'JetBrains Mono',monospace;font-size:12px;color:var(--text3);
  background:var(--bg2);border:1px solid var(--border);padding:10px 16px;border-radius:8px;margin-bottom:24px;word-break:break-all;}
.ea{display:flex;gap:10px;justify-content:center;flex-wrap:wrap;}
.ef{text-align:center;padding:16px;font-size:12px;color:var(--text4);border-top:1px solid var(--border);
  display:flex;align-items:center;justify-content:center;gap:8px;}
.ef img{width:18px;height:18px;border-radius:3px;opacity:.5;}
.ef a{color:var(--text3);}
)CSS"
        +"</style></head><body>"
        "<div class='em'><div class='eb'>"
        "<div class='ee'>"+emoji+"</div>"
        "<div class='ec'>"+std::to_string(code)+"</div>"
        "<div class='et'>"+title+"</div>"
        "<div class='ed'>"+desc+"</div>"
        +(detail.empty()?"":"<div class='edtl'>"+escH(detail)+"</div>")
        +"<div class='ea'>"
        "<a href='/' class='btn'>← Home</a>"
        "<a href='javascript:history.back()' class='btn'>↩ Go Back</a>"
        "<a href='/pronoadmin' class='btn'>⚙ Admin</a>"
        "</div></div></div>"
        "<div class='ef'>"
        "<img src='https://cdn.brandfetch.io/idD7af6BB5/w/500/h/500/theme/dark/logo.png?c=1dxbfHSJFAPEGdCLU4o5B' alt=''>"
        "<span>Prono Server &mdash; <a href='https://probloxworld.com' target='_blank'>probloxworld</a></span>"
        "</div></body></html>\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// HTTP RESPONSE BUILDER
// ═══════════════════════════════════════════════════════════════════════════
static const std::map<int,std::string> STATUS_LINES={
    {200,"200 OK"},{201,"201 Created"},{204,"204 No Content"},
    {301,"301 Moved Permanently"},{302,"302 Found"},{304,"304 Not Modified"},
    {400,"400 Bad Request"},{401,"401 Unauthorized"},{403,"403 Forbidden"},
    {404,"404 Not Found"},{405,"405 Method Not Allowed"},{408,"408 Request Timeout"},
    {409,"409 Conflict"},{410,"410 Gone"},{413,"413 Payload Too Large"},
    {429,"429 Too Many Requests"},{431,"431 Request Header Fields Too Large"},
    {500,"500 Internal Server Error"},{501,"501 Not Implemented"},
    {502,"502 Bad Gateway"},{503,"503 Service Unavailable"},
    {504,"504 Gateway Timeout"},{505,"505 HTTP Version Not Supported"},
    {520,"520 Unknown Error"},{540,"540 Temporarily Disabled"},
};

std::string buildResp(const std::string& body,
                      const std::string& ct="text/html; charset=utf-8",
                      int code=200,
                      const std::string& extra="")
{
    auto it=STATUS_LINES.find(code);
    std::string sl=(it!=STATUS_LINES.end())?it->second:std::to_string(code)+" Error";
    std::string r=
        "HTTP/1.1 "+sl+"\r\n"
        "Server: "+CFG.serverName+"\r\n"
        "Content-Type: "+ct+"\r\n"
        "Content-Length: "+std::to_string(body.size())+"\r\n";
    if(CFG.xcto)   r+="X-Content-Type-Options: nosniff\r\n";
    if(CFG.xframe) r+="X-Frame-Options: SAMEORIGIN\r\n";
    if(CFG.xss)    r+="X-XSS-Protection: 1; mode=block\r\n";
    if(CFG.hsts)   r+="Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n";
    if(CFG.csp)    r+="Content-Security-Policy: "+CFG.cspValue+"\r\n";
    r+="Referrer-Policy: "+CFG.referrerPolicy+"\r\n";
    if(CFG.corsEnabled){
        r+="Access-Control-Allow-Origin: "+CFG.corsOrigin+"\r\n";
        r+="Access-Control-Allow-Methods: "+CFG.corsMethods+"\r\n";
        r+="Access-Control-Allow-Headers: "+CFG.corsHeaders+"\r\n";
        r+="Access-Control-Max-Age: "+std::to_string(CFG.corsMaxAge)+"\r\n";
        if(CFG.corsCredentials)r+="Access-Control-Allow-Credentials: true\r\n";
    }
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        for(auto& kv:CFG.customHeaders)r+=kv.first+": "+kv.second+"\r\n";
    }
    if(!extra.empty())r+=extra;
    r+="Connection: close\r\n\r\n"+body;
    return r;
}
std::string redir(const std::string& loc,int c=302)
{
    return "HTTP/1.1 "+std::to_string(c)+(c==301?" Moved Permanently":" Found")+"\r\n"
           "Location: "+loc+"\r\n"
           "Server: "+CFG.serverName+"\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// DASHBOARD — every button/form posts to a real endpoint, zero dummy alerts
// ═══════════════════════════════════════════════════════════════════════════
std::string dashboard(const std::string& activeTab="overview",
                      const std::string& fileDir="",
                      const std::string& flash="",
                      const std::string& flashType="ok")
{
    long up=uptimeSecs();
    long long total=requestCount.load(),e4=err4xx.load(),e5=err5xx.load(),ok=req2xx.load();
    int pct4=total>0?(int)(e4*100/total):0;
    int pct5=total>0?(int)(e5*100/total):0;
    int pctok=total>0?(int)(ok*100/total):0;

    // ── Build table rows ────────────────────────────────────────────────
    std::string pathRows,ipRows,statusRows;
    {
        std::lock_guard<std::mutex> lk(statsMutex);
        std::vector<std::pair<int,std::string>> sp,si;
        for(auto& kv:pathHits)sp.push_back({kv.second,kv.first});
        for(auto& kv:ipHits)  si.push_back({kv.second,kv.first});
        std::sort(sp.rbegin(),sp.rend());
        std::sort(si.rbegin(),si.rend());
        int n=0;for(auto& kv:sp){if(n++>=12)break;
            pathRows+="<tr><td>"+escH(kv.second)+"</td><td style='color:var(--text)'>"+std::to_string(kv.first)+"</td></tr>";}
        n=0;for(auto& kv:si){if(n++>=10)break;
            ipRows+="<tr><td>"+escH(kv.second)+"</td><td style='color:var(--text)'>"+std::to_string(kv.first)+"</td></tr>";}
        for(auto& kv:statusCounts){
            std::string c=kv.first>=500?"t-red":kv.first>=400?"t-orange":kv.first>=300?"t-blue":"t-green";
            statusRows+="<tr><td><span class='tag "+c+"'>"+std::to_string(kv.first)+"</span></td>"
                        "<td style='color:var(--text)'>"+std::to_string(kv.second)+"</td></tr>";}
    }
    // Vhost rows
    std::string vhRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.vhosts.empty())
            vhRows="<tr><td colspan='4' style='text-align:center;color:var(--text3);padding:16px'>No virtual hosts configured</td></tr>";
        else for(auto& kv:CFG.vhosts)
            vhRows+="<tr><td style='color:var(--text)'>"+escH(kv.first)+"</td>"
                    "<td style='color:var(--text3)'>"+escH(kv.second)+"</td>"
                    "<td><span class='tag t-green'>Active</span></td>"
                    "<td><a href='/pronoadmin/removevhost?host="+escH(kv.first)
                    +"' class='btn btn-sm btn-danger' onclick=\"return confirm('Remove "+escH(kv.first)+"?')\">Remove</a></td></tr>";
    }
    // Blocked IPs
    std::string blkRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.blockedIPs.empty())
            blkRows="<tr><td colspan='2' style='text-align:center;color:var(--text3);padding:16px'>No blocked IPs</td></tr>";
        else for(auto& ip:CFG.blockedIPs)
            blkRows+="<tr><td style='color:var(--text)'>"+escH(ip)+"</td>"
                     "<td><a href='/pronoadmin/unblock?ip="+escH(ip)+"' class='btn btn-sm'>Unblock</a></td></tr>";
    }
    // Rewrite rows
    std::string rwRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.rewrites.empty())
            rwRows="<tr><td colspan='4' style='text-align:center;color:var(--text3);padding:16px'>No rewrite rules</td></tr>";
        else for(int i=0;i<(int)CFG.rewrites.size();++i){
            auto& r=CFG.rewrites[i];
            rwRows+="<tr><td class='mono'>"+escH(r.pattern)+"</td>"
                    "<td class='mono'>"+escH(r.replacement)+"</td>"
                    "<td><span class='tag t-blue'>"+std::to_string(r.code)+"</span></td>"
                    "<td><a href='/pronoadmin/removerewrite?i="+std::to_string(i)
                    +"' class='btn btn-sm btn-danger' onclick=\"return confirm('Remove rule?')\">Remove</a></td></tr>";
        }
    }
    // Proxy rows
    std::string prRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.proxyRules.empty())
            prRows="<tr><td colspan='3' style='text-align:center;color:var(--text3);padding:16px'>No proxy rules</td></tr>";
        else for(auto& kv:CFG.proxyRules)
            prRows+="<tr><td class='mono' style='color:var(--text)'>"+escH(kv.first)+"</td>"
                    "<td class='mono' style='color:var(--text2)'>"+escH(kv.second)+"</td>"
                    "<td><a href='/pronoadmin/removeproxy?path="+escH(kv.first)
                    +"' class='btn btn-sm btn-danger' onclick=\"return confirm('Remove proxy rule?')\">Remove</a></td></tr>";
    }
    // Custom header rows
    std::string chRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.customHeaders.empty())
            chRows="<tr><td colspan='3' style='text-align:center;color:var(--text3);padding:16px'>No custom headers</td></tr>";
        else for(auto& kv:CFG.customHeaders)
            chRows+="<tr><td class='mono' style='color:var(--text)'>"+escH(kv.first)+"</td>"
                    "<td class='mono' style='color:var(--text2)'>"+escH(kv.second)+"</td>"
                    "<td><a href='/pronoadmin/removeheader?name="+escH(kv.first)
                    +"' class='btn btn-sm btn-danger'>Remove</a></td></tr>";
    }
    // Auth user rows
    std::string auRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.authUsers.empty())
            auRows="<tr><td colspan='3' style='text-align:center;color:var(--text3);padding:16px'>No users</td></tr>";
        else for(auto& kv:CFG.authUsers)
            auRows+="<tr><td style='color:var(--text)'>"+escH(kv.first)+"</td>"
                    "<td><span class='tag t-green'>active</span></td>"
                    "<td><a href='/pronoadmin/removeuser?user="+escH(kv.first)
                    +"' class='btn btn-sm btn-danger' onclick=\"return confirm('Remove user?')\">Remove</a></td></tr>";
    }
    // Cache rule rows
    std::string crRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        for(auto& kv:CFG.cacheRules)
            crRows+="<tr><td class='mono' style='color:var(--text)'>"+escH(kv.first)+"</td>"
                    "<td class='mono' style='color:var(--text2)'>"+escH(kv.second)+"</td>"
                    "<td><a href='/pronoadmin/removecache?ext="+escH(kv.first)
                    +"' class='btn btn-sm btn-danger'>Remove</a></td></tr>";
    }
    // Error page override rows
    std::string epRows;
    {
        std::lock_guard<std::mutex> lk(cfgMutex);
        if(CFG.errorPages.empty())
            epRows="<tr><td colspan='3' style='text-align:center;color:var(--text3);padding:16px'>No overrides set</td></tr>";
        else for(auto& kv:CFG.errorPages)
            epRows+="<tr><td><span class='tag t-orange'>"+std::to_string(kv.first)+"</span></td>"
                    "<td class='mono'>"+escH(kv.second)+"</td>"
                    "<td><a href='/pronoadmin/removeerrorpage?code="+std::to_string(kv.first)
                    +"' class='btn btn-sm btn-danger'>Remove</a></td></tr>";
    }
    // Rate-limit state
    std::string rlRows;
    {
        std::lock_guard<std::mutex> lk(rlMutex);
        int n=0;
        for(auto& kv:rlState){
            if(n++>20)break;
            rlRows+="<tr><td style='color:var(--text)'>"+escH(kv.first)+"</td>"
                    "<td>"+std::to_string(kv.second.first)+"</td></tr>";
        }
        if(rlState.empty())
            rlRows="<tr><td colspan='2' style='text-align:center;color:var(--text3);padding:16px'>No data yet</td></tr>";
    }

    // Helper lambdas
    auto nt=[&](const std::string& id,const std::string& lbl)->std::string{
        std::string c=(id==activeTab)?"nt active":"nt";
        return "<div class='"+c+"' onclick=\"showTab('"+id+"')\">"+lbl+"</div>";
    };
    // Toggle widget — uses AJAX /pronoadmin/toggle?key=X&val=0/1
    auto togRow=[&](const std::string& lbl,const std::string& desc,bool val,const std::string& key)->std::string{
        std::string on=val?"on":"";
        return "<div class='tog-row'>"
            "<div class='tog-info'><div class='tt'>"+lbl+"</div>"
            +(desc.empty()?"":"<div class='td'>"+desc+"</div>")+"</div>"
            "<div class='tog "+on+"' onclick=\"doToggle('"+key+"',this)\"></div>"
            "</div>";
    };

    // ── BEGIN HTML ──────────────────────────────────────────────────────
    std::string h=
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Prono Admin — probloxworld</title>"
        "<style>"+CSS+R"CSS(
.topbar{background:var(--bg2);border-bottom:1px solid var(--border);
  padding:0 28px;height:62px;display:flex;align-items:center;gap:14px;position:sticky;top:0;z-index:100;}
.logo{display:flex;align-items:center;gap:12px;}
.logo img{width:36px;height:36px;border-radius:9px;object-fit:contain;}
.ln{font-family:'Space Grotesk',sans-serif;font-weight:800;font-size:16px;line-height:1.2;}
.ls{font-size:10px;color:var(--text3);letter-spacing:.8px;text-transform:uppercase;}
.tbc{margin-left:auto;display:flex;align-items:center;gap:10px;}
.pulse{width:8px;height:8px;border-radius:50%;background:var(--green);position:relative;}
.pulse::after{content:'';position:absolute;top:-3px;left:-3px;right:-3px;bottom:-3px;
  border-radius:50%;border:1.5px solid var(--green);animation:pa 2s infinite;}
@keyframes pa{0%,100%{opacity:.8;transform:scale(1)}50%{opacity:0;transform:scale(1.8)}}
.vtag{font-size:11px;color:var(--text3);background:var(--bg3);border:1px solid var(--border2);padding:3px 11px;border-radius:20px;}
#clock{font-size:12px;color:var(--text3);font-family:'JetBrains Mono',monospace;}
.sbar{background:var(--bg3);border-bottom:1px solid var(--border);
  padding:7px 28px;display:flex;align-items:center;gap:20px;font-size:11px;color:var(--text3);flex-wrap:wrap;}
.sbar b{color:var(--text);}
.sdot{width:6px;height:6px;border-radius:50%;background:var(--green);display:inline-block;margin-right:4px;}
.nav{background:var(--bg2);border-bottom:1px solid var(--border);padding:0 28px;display:flex;gap:2px;overflow-x:auto;}
.nav::-webkit-scrollbar{height:0;}
.nt{padding:13px 15px;font-size:12px;font-weight:500;color:var(--text3);cursor:pointer;
  border-bottom:2px solid transparent;transition:all .15s;white-space:nowrap;user-select:none;}
.nt:hover{color:var(--text2);}
.nt.active{color:var(--accent);border-bottom-color:var(--accent);}
.content{padding:26px 28px;max-width:1500px;margin:0 auto;}
.section{display:none;}
.section.active{display:block;animation:fi .18s ease;}
@keyframes fi{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
.footer{background:var(--bg2);border-top:1px solid var(--border);padding:16px 28px;
  text-align:center;font-size:12px;color:var(--text3);display:flex;align-items:center;justify-content:center;gap:10px;}
.footer img{width:20px;height:20px;border-radius:4px;opacity:.6;}
)CSS"+"</style></head><body>\n"

    // TOPBAR
    "<div class='topbar'>"
    "<div class='logo'>"
    "<img src='https://cdn.brandfetch.io/idD7af6BB5/w/500/h/500/theme/dark/logo.png?c=1dxbfHSJFAPEGdCLU4o5B' alt='' onerror=\"this.style.display='none'\">"
    "<div><div class='ln'>Prono Server</div><div class='ls'>by probloxworld</div></div>"
    "</div>"
    "<div class='tbc'>"
    "<div class='pulse'></div>"
    "<span style='color:var(--green);font-size:12px;font-weight:500'>Running</span>"
    "<div class='vtag'>Prono/1.0</div>"
    "<div id='clock'></div>"
    "</div>"
    "<div style='display:flex;gap:8px;margin-left:12px'>"
    "<a href='/pronoadmin/testconfig' class='btn btn-success btn-sm'>✓ Test Config</a>"
    "<a href='/pronoadmin' class='btn btn-sm'>↺ Refresh</a>"
    "</div>"
    "</div>\n"

    // STATUS BAR
    "<div class='sbar'>"
    "<span><span class='sdot'></span>Online</span>"
    "<span>Port: <b>"+std::to_string(CFG.port)+"</b></span>"
    "<span>Webroot: <b>"+escH(CFG.webroot)+"</b></span>"
    "<span>Uptime: <b>"+escH(fmtUptime(up))+"</b></span>"
    "<span>Requests: <b>"+std::to_string(total)+"</b></span>"
    "<span>Sent: <b>"+fmtBytes(bytesSent.load())+"</b></span>"
    "<span>Recv: <b>"+fmtBytes(bytesRecv.load())+"</b></span>"
    "<span style='margin-left:auto'>Vhosts: <b>"+std::to_string(CFG.vhosts.size())+"</b></span>"
    "</div>\n"

    // NAV
    "<div class='nav'>"
    +nt("overview","◈ Overview")
    +nt("logs","▤ Logs")
    +nt("vhosts","⬡ Virtual Hosts")
    +nt("config","⚙ Config")
    +nt("rewrites","⟲ Rewrites")
    +nt("headers","≡ Headers")
    +nt("cache","⚡ Cache")
    +nt("ratelimit","⊘ Rate Limit")
    +nt("firewall","⬖ Firewall")
    +nt("stats","◳ Stats")
    +nt("files","📂 Files")
    +nt("errorpages","✕ Error Pages")
    +"</div>\n"

    "<div id='toast'></div>\n"
    "<div class='content'>\n";

    // ── OVERVIEW ────────────────────────────────────────────────────────
    h+="<div class='section' id='s-overview'>"
       "<div class='sec-title'>Dashboard Overview</div>"
       "<div class='sec-desc'>Live server metrics and activity</div>"
       "<div class='grid g4' style='margin-bottom:16px'>"
       "<div class='mc blue'><div class='mc-val' style='color:var(--accent)'>"+std::to_string(total)+"</div>"
       "<div class='mc-key'>Total Requests</div>"
       "<div class='spark'><div class='sb' style='height:30%'></div><div class='sb' style='height:50%'></div>"
       "<div class='sb' style='height:40%'></div><div class='sb' style='height:70%'></div>"
       "<div class='sb' style='height:60%'></div><div class='sb' style='height:80%'></div>"
       "<div class='sb' style='height:65%'></div><div class='sb hi' style='height:100%'></div></div></div>"
       "<div class='mc green'><div class='mc-val' style='color:var(--green)'>"+escH(fmtUptime(up))+"</div>"
       "<div class='mc-key'>Uptime</div><div class='mc-sub'>Since start</div></div>"
       "<div class='mc orange'><div class='mc-val' style='color:var(--orange)'>"+fmtBytes(bytesSent.load())+"</div>"
       "<div class='mc-key'>Data Sent</div><div class='mc-sub'>Recv: "+fmtBytes(bytesRecv.load())+"</div></div>"
       "<div class='mc red'><div class='mc-val' style='color:var(--red)'>"+std::to_string(e4+e5)+"</div>"
       "<div class='mc-key'>Errors (4xx+5xx)</div><div class='mc-sub'>"+std::to_string(pct4+pct5)+"% of requests</div></div>"
       "</div>"
       "<div class='grid g2' style='margin-bottom:16px'>"
       "<div class='card'><div class='card-title'>Status Breakdown</div>"
       "<div class='bw'><div class='bl'><span>2xx Success</span><span style='color:var(--green)'>"+std::to_string(pctok)+"%</span></div>"
       "<div class='bar'><div class='bf c-green' style='width:"+std::to_string(pctok)+"%'></div></div></div>"
       "<div class='bw'><div class='bl'><span>4xx Client Errors</span><span style='color:var(--orange)'>"+std::to_string(pct4)+"%</span></div>"
       "<div class='bar'><div class='bf c-orange' style='width:"+std::to_string(pct4)+"%'></div></div></div>"
       "<div class='bw'><div class='bl'><span>5xx Server Errors</span><span style='color:var(--red)'>"+std::to_string(pct5)+"%</span></div>"
       "<div class='bar'><div class='bf c-red' style='width:"+std::to_string(pct5)+"%'></div></div></div>"
       "</div>"
       "<div class='card'><div class='card-title'>Top Paths</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Path</th><th>Hits</th></tr></thead>"
       "<tbody>"+pathRows+"</tbody></table></div></div>"
       "</div>"
       "<div class='card'><div class='card-title'>Recent Requests</div>"
       "<div class='lw'><div class='lb' style='height:200px'>"+logsHtml()+"</div></div></div>"
       "</div>\n"; // overview

    // ── LOGS ────────────────────────────────────────────────────────────
    h+="<div class='section' id='s-logs'>"
       "<div class='sec-title'>Access Logs</div>"
       "<div class='sec-desc'>All requests, newest first</div>"
       "<div style='display:flex;gap:8px;margin-bottom:14px;flex-wrap:wrap'>"
       "<select class='fs' style='max-width:160px' id='lf' onchange='filterLog()'>"
       "<option value=''>All</option><option value='l2'>2xx</option>"
       "<option value='l3'>3xx</option><option value='l4'>4xx</option><option value='l5'>5xx</option>"
       "</select>"
       "<input class='fi' style='max-width:220px' id='ls' placeholder='Search...' oninput='filterLog()'>"
       "<a href='/pronoadmin' class='btn btn-sm'>↺ Refresh</a>"
       "<a href='/pronoadmin/clearlog' class='btn btn-sm btn-danger' onclick=\"return confirm('Clear all logs?')\">🗑 Clear</a>"
       "<a href='/pronoadmin/downloadlog' class='btn btn-sm'>⬇ Download</a>"
       "</div>"
       "<div class='lw'><div class='lh'><span>"+escH(CFG.logFile)+" &middot; "+std::to_string(accessLogs.size())+" entries</span></div>"
       "<div class='lb' id='logBody' style='height:500px'>"+logsHtml()+"</div></div>"
       "<script>function filterLog(){"
       "var f=document.getElementById('lf').value;"
       "var s=document.getElementById('ls').value.toLowerCase();"
       "document.querySelectorAll('#logBody .ll').forEach(function(el){"
       "var ok=true;"
       "if(f&&!el.classList.contains(f))ok=false;"
       "if(s&&el.textContent.toLowerCase().indexOf(s)<0)ok=false;"
       "el.style.display=ok?'':'none';});}"
       "</script>"
       "</div>\n"; // logs

    // ── VIRTUAL HOSTS ───────────────────────────────────────────────────
    h+="<div class='section' id='s-vhosts'>"
       "<div class='sec-title'>Virtual Hosts</div>"
       "<div class='sec-desc'>Route hostnames to separate document roots. The Host header selects the docroot.</div>"
       "<div class='grid g2'>"
       "<div class='card'><div class='card-title'>Active Virtual Hosts</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Hostname</th><th>Document Root</th><th>Status</th><th></th></tr></thead>"
       "<tbody>"+vhRows+"</tbody></table></div></div>"
       "<div class='card'><div class='card-title'>Add Virtual Host</div>"
       "<form action='/pronoadmin/addvhost' method='GET'>"
       "<div class='fg'><label class='fl'>Hostname</label><input class='fi' name='host' placeholder='example.com' required></div>"
       "<div class='fg'><label class='fl'>Document Root (absolute path)</label><input class='fi' name='root' placeholder='/var/www/example' required></div>"
       "<button type='submit' class='btn btn-primary'>+ Add Vhost</button>"
       "</form></div>"
       "</div></div>\n"; // vhosts

    // ── CONFIG ──────────────────────────────────────────────────────────
    h+="<div class='section' id='s-config'>"
       "<div class='sec-title'>Server Configuration</div>"
       "<div class='sec-desc'>Settings apply immediately. Port requires restart.</div>"
       "<div class='notice n-warn'>⚠ Changing the port requires restarting the server process.</div>"
       "<div class='grid g2'>"
       "<div class='card'><div class='card-title'>Core Settings</div>"
       "<form action='/pronoadmin/savecfg' method='GET'>"
       "<div class='fg'><label class='fl'>Server Name Header</label>"
       "<input class='fi' name='serverName' value='"+escH(CFG.serverName)+"'></div>"
       "<div class='fg'><label class='fl'>Web Root</label>"
       "<input class='fi' name='webroot' value='"+escH(CFG.webroot)+"'></div>"
       "<div class='fg'><label class='fl'>Main URL (used for file View links)</label>"
       "<input class='fi' name='mainUrl' placeholder='http://192.168.1.10:"+std::to_string(CFG.port)+"' value='"+escH(CFG.mainUrl)+"'></div>"
       "<div class='fg'><label class='fl'>Log File</label>"
       "<input class='fi' name='logFile' value='"+escH(CFG.logFile)+"'></div>"
       "<div class='fg'><label class='fl'>Max Log Entries</label>"
       "<input class='fi' name='maxLog' type='number' value='"+std::to_string(CFG.maxLogEntries)+"'></div>"
       "<button type='submit' class='btn btn-primary'>💾 Save Core Settings</button>"
       "</form><hr class='dv'>"
       +togRow("Directory Listing","Show file list when no index found",CFG.dirListing,"dirListing")
       +togRow("Access Logging","Write all requests to log file",CFG.accessLog,"accessLog")
       +togRow("Colored Console Output","ANSI colors in terminal",CFG.colorConsole,"colorConsole")
       +"</div>"
       "<div>"
       "<div class='card' style='margin-bottom:16px'><div class='card-title'>Allowed HTTP Methods</div>"
       "<form action='/pronoadmin/savemethods' method='GET'>";
    for(auto& m:{"GET","HEAD","POST","PUT","DELETE","OPTIONS","PATCH"}){
        bool on=CFG.allowedMethods.count(m)>0;
        h+="<div class='tog-row'><div class='tog-info'><div class='tt'>"+std::string(m)+"</div></div>"
           "<input type='checkbox' name='m_"+std::string(m)+"'"+(on?" checked":"")
           +" style='width:20px;height:20px;cursor:pointer;accent-color:var(--accent2)'></div>";
    }
    h+="<button type='submit' class='btn btn-primary' style='margin-top:10px'>Save Methods</button>"
       "</form></div>"
       "</div>" // right col
       "</div></div>\n"; // config


    // ── REWRITES ────────────────────────────────────────────────────────
    h+="<div class='section' id='s-rewrites'>"
       "<div class='sec-title'>URL Rewrites & Redirects</div>"
       "<div class='sec-desc'>Regex pattern → replacement. Capture groups $1, $2 supported.</div>"
       "<div class='grid g2'>"
       "<div class='card'><div class='card-title'>Active Rules</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Pattern</th><th>Replacement</th><th>Code</th><th></th></tr></thead>"
       "<tbody>"+rwRows+"</tbody></table></div></div>"
       "<div class='card'><div class='card-title'>Add Rewrite Rule</div>"
       "<form action='/pronoadmin/addrewrite' method='GET'>"
       "<div class='fg'><label class='fl'>Regex Pattern</label><input class='fi' name='pattern' placeholder='^/old/(.*)$' required></div>"
       "<div class='fg'><label class='fl'>Replacement</label><input class='fi' name='replacement' placeholder='/new/$1' required></div>"
       "<div class='fg'><label class='fl'>Response Code</label>"
       "<select class='fs' name='code'>"
       "<option value='301'>301 Permanent Redirect</option>"
       "<option value='302' selected>302 Temporary Redirect</option>"
       "<option value='0'>0 — Internal rewrite (no redirect)</option>"
       "</select></div>"
       "<button type='submit' class='btn btn-primary'>+ Add Rule</button>"
       "</form></div>"
       "</div></div>\n"; // rewrites

    // ── HEADERS ─────────────────────────────────────────────────────────
    h+="<div class='section' id='s-headers'>"
       "<div class='sec-title'>HTTP Response Headers</div>"
       "<div class='sec-desc'>Security headers, CORS, and custom headers added to every response.</div>"
       "<div class='grid g2'>"
       "<div>"
       "<div class='card' style='margin-bottom:16px'><div class='card-title'>Security Headers</div>"
       +togRow("X-Content-Type-Options: nosniff","",CFG.xcto,"xcto")
       +togRow("X-Frame-Options: SAMEORIGIN","",CFG.xframe,"xframe")
       +togRow("X-XSS-Protection: 1; mode=block","",CFG.xss,"xss")
       +togRow("Strict-Transport-Security (HSTS)","Requires HTTPS",CFG.hsts,"hsts")
       +togRow("Content-Security-Policy","",CFG.csp,"csp")
       +"<form action='/pronoadmin/savecsp' method='GET' style='margin-top:10px'>"
       "<div class='fg'><label class='fl'>CSP Value</label>"
       "<input class='fi' name='csp' value='"+escH(CFG.cspValue)+"'></div>"
       "<div class='fg'><label class='fl'>Referrer-Policy</label>"
       "<select class='fs' name='ref'>";
    for(auto& rv:{"strict-origin-when-cross-origin","no-referrer","same-origin","unsafe-url","origin"})
        h+="<option value='"+std::string(rv)+"'"+(CFG.referrerPolicy==rv?" selected":"")+">"
           +std::string(rv)+"</option>";
    h+="</select></div>"
       "<button type='submit' class='btn btn-primary btn-sm'>Save Security Headers</button>"
       "</form></div>"
       "<div class='card'><div class='card-title'>Custom Response Headers</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Name</th><th>Value</th><th></th></tr></thead>"
       "<tbody>"+chRows+"</tbody></table></div>"
       "<form action='/pronoadmin/addheader' method='GET' style='margin-top:14px'>"
       "<div class='fr'>"
       "<div class='fg'><label class='fl'>Header Name</label><input class='fi' name='name' placeholder='X-Custom' required></div>"
       "<div class='fg'><label class='fl'>Value</label><input class='fi' name='value' placeholder='my-value' required></div>"
       "</div>"
       "<button type='submit' class='btn btn-primary btn-sm'>+ Add Header</button>"
       "</form></div>"
       "</div>"
       "<div class='card'><div class='card-title'>CORS</div>"
       "<form action='/pronoadmin/savecors' method='GET'>"
       +togRow("Enable CORS","Cross-Origin Resource Sharing",CFG.corsEnabled,"corsEnabled")
       +"<div style='margin-top:10px'>"
       "<div class='fg'><label class='fl'>Allow-Origin</label>"
       "<input class='fi' name='origin' value='"+escH(CFG.corsOrigin)+"'></div>"
       "<div class='fg'><label class='fl'>Allow-Methods</label>"
       "<input class='fi' name='methods' value='"+escH(CFG.corsMethods)+"'></div>"
       "<div class='fg'><label class='fl'>Allow-Headers</label>"
       "<input class='fi' name='headers' value='"+escH(CFG.corsHeaders)+"'></div>"
       "<div class='fg'><label class='fl'>Max-Age (seconds)</label>"
       "<input class='fi' name='maxage' type='number' value='"+std::to_string(CFG.corsMaxAge)+"'></div>"
       +togRow("Allow-Credentials","",CFG.corsCredentials,"corsCredentials")
       +"<button type='submit' class='btn btn-primary' style='margin-top:8px'>Save CORS</button>"
       "</div></form></div>"
       "</div></div>\n"; // headers



    // ── CACHE ───────────────────────────────────────────────────────────
    h+="<div class='section' id='s-cache'>"
       "<div class='sec-title'>Cache Control</div>"
       "<div class='sec-desc'>Cache-Control headers sent per file extension.</div>"
       "<div class='grid g2'>"
       "<div class='card'><div class='card-title'>Cache Rules</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Extension</th><th>Cache-Control Value</th><th></th></tr></thead>"
       "<tbody>"+crRows+"</tbody></table></div>"
       "<form action='/pronoadmin/addcache' method='GET' style='margin-top:14px'>"
       "<div class='fr'>"
       "<div class='fg'><label class='fl'>Extension (e.g. .png)</label><input class='fi' name='ext' placeholder='.png' required></div>"
       "<div class='fg'><label class='fl'>Cache-Control Value</label><input class='fi' name='val' placeholder='public, max-age=86400' required></div>"
       "</div>"
       "<button type='submit' class='btn btn-primary btn-sm'>+ Add Rule</button>"
       "</form></div>"
       "</div></div>\n"; // cache

    // ── RATE LIMIT ──────────────────────────────────────────────────────
    h+="<div class='section' id='s-ratelimit'>"
       "<div class='sec-title'>Rate Limiting</div>"
       "<div class='sec-desc'>Per-IP sliding-window rate limiting. Excess requests receive 429.</div>"
       "<div class='grid g2'>"
       "<div class='card'><div class='card-title'>Settings</div>"
       "<form action='/pronoadmin/saverl' method='GET'>"
       +togRow("Enable Rate Limiting","Block excess requests with 429",CFG.rateLimitEnabled,"rateLimitEnabled")
       +"<div class='fg' style='margin-top:10px'><label class='fl'>Max Requests / minute per IP</label>"
       "<input class='fi' name='rlRate' type='number' value='"+std::to_string(CFG.rateLimitPerMin)+"'></div>"
       "<div class='fg'><label class='fl'>Burst Allowance</label>"
       "<input class='fi' name='rlBurst' type='number' value='"+std::to_string(CFG.rateLimitBurst)+"'></div>"
       "<button type='submit' class='btn btn-primary'>Save</button>"
       "</form></div>"
       "<div class='card'><div class='card-title'>Current State</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>IP</th><th>Reqs this min</th></tr></thead>"
       "<tbody>"+rlRows+"</tbody></table></div>"
       "<a href='/pronoadmin/clearrl' class='btn btn-danger btn-sm' style='margin-top:12px'>Clear State</a>"
       "</div></div></div>\n"; // ratelimit



    // ── FIREWALL ────────────────────────────────────────────────────────
    h+="<div class='section' id='s-firewall'>"
       "<div class='sec-title'>Firewall & IP Management</div>"
       "<div class='sec-desc'>Blocked IPs receive 403 immediately before any request is processed. "
       "Path traversal and dotfile access are always blocked automatically.</div>"
       "<div class='card'><div class='card-title'>Blocked IPs</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>IP Address</th><th></th></tr></thead>"
       "<tbody>"+blkRows+"</tbody></table></div>"
       "<form action='/pronoadmin/block' method='GET' style='display:flex;gap:8px;margin-top:14px'>"
       "<input class='fi' name='ip' placeholder='1.2.3.4' required>"
       "<button type='submit' class='btn btn-danger'>Block IP</button>"
       "</form></div>"
       "</div>\n"; // firewall

    // ── STATS ───────────────────────────────────────────────────────────
    h+="<div class='section' id='s-stats'>"
       "<div class='sec-title'>Statistics</div>"
       "<div class='sec-desc'>Per-path, per-IP, and status code breakdowns.</div>"
       "<div class='grid g4' style='margin-bottom:16px'>"
       "<div class='mc blue'><div class='mc-val' style='color:var(--accent)'>"+std::to_string(total)+"</div><div class='mc-key'>Total Requests</div></div>"
       "<div class='mc green'><div class='mc-val' style='color:var(--green)'>"+fmtBytes(bytesSent.load())+"</div><div class='mc-key'>Data Sent</div></div>"
       "<div class='mc orange'><div class='mc-val' style='color:var(--orange)'>"+std::to_string(e4)+"</div><div class='mc-key'>4xx Errors</div></div>"
       "<div class='mc red'><div class='mc-val' style='color:var(--red)'>"+std::to_string(e5)+"</div><div class='mc-key'>5xx Errors</div></div>"
       "</div>"
       "<div class='grid g3'>"
       "<div class='card'><div class='card-title'>Top Paths</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Path</th><th>Hits</th></tr></thead>"
       "<tbody>"+pathRows+"</tbody></table></div></div>"
       "<div class='card'><div class='card-title'>Top Client IPs</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>IP</th><th>Hits</th></tr></thead>"
       "<tbody>"+ipRows+"</tbody></table></div></div>"
       "<div class='card'><div class='card-title'>Status Codes</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Code</th><th>Count</th></tr></thead>"
       "<tbody>"+statusRows+"</tbody></table></div></div>"
       "</div>"
       "<div style='margin-top:16px'>"
       "<a href='/pronoadmin/resetstats' class='btn btn-danger' onclick=\"return confirm('Reset all stats?')\">Reset Stats</a>"
       "</div></div>\n"; // stats

    // ── FILES ───────────────────────────────────────────────────────────
    h+="<div class='section' id='s-files'>"
       "<div class='sec-title'>Web Root Files</div>"
       "<div class='sec-desc'>Browse and manage files in <code style='color:var(--accent)'>"+escH(CFG.webroot)+"</code></div>"
       "<div class='card'>"
       "<div class='card-title'>"
       "📂 "+escH(CFG.webroot+(fileDir.empty()?"":" / "+fileDir))
       +"<div style='display:flex;gap:8px'><a href='/pronoadmin/files' class='btn btn-sm'>⌂ Root</a></div></div>"
       "<div class='tbl-wrap'><table>"
       "<thead><tr><th></th><th>Name</th><th>Size</th><th>Type</th><th>Modified</th><th>Actions</th></tr></thead>"
       "<tbody>"+fileRows(fileDir)+"</tbody>"
       "</table></div></div></div>\n"; // files

    // ── ERROR PAGES ─────────────────────────────────────────────────────
    h+="<div class='section' id='s-errorpages'>"
       "<div class='sec-title'>Custom Error Pages</div>"
       "<div class='sec-desc'>Override any HTTP error code with a custom HTML file from your webroot.</div>"
       "<div class='grid g2'>"
       "<div class='card'><div class='card-title'>Overrides</div>"
       "<div class='tbl-wrap'><table><thead><tr><th>Code</th><th>File (relative to webroot)</th><th></th></tr></thead>"
       "<tbody>"+epRows+"</tbody></table></div>"
       "<form action='/pronoadmin/seterrorpage' method='GET' style='margin-top:14px'>"
       "<div class='fr'>"
       "<div class='fg'><label class='fl'>HTTP Code</label><input class='fi' name='code' type='number' placeholder='404' required></div>"
       "<div class='fg'><label class='fl'>Path (e.g. /errors/404.html)</label><input class='fi' name='path' placeholder='/errors/404.html' required></div>"
       "</div>"
       "<button type='submit' class='btn btn-primary btn-sm'>+ Set Override</button>"
       "</form></div>"
       "<div class='card'><div class='card-title'>Preview Built-in Error Pages</div>"
       "<div style='display:flex;flex-wrap:wrap;gap:8px'>";
    for(auto& e:ERRS)
        h+="<a href='/pronoadmin/previewerror?code="+std::to_string(e.code)
          +"' target='_blank' class='btn btn-sm'>"+std::to_string(e.code)+"</a>";
    h+="</div></div></div></div>\n"; // errorpages

    h+="</div>\n"; // content

    // FOOTER
    h+="<div class='footer'>"
       "<img src='https://cdn.brandfetch.io/idD7af6BB5/w/500/h/500/theme/dark/logo.png?c=1dxbfHSJFAPEGdCLU4o5B' alt=''>"
       "<span>Prono Server &mdash; Built by <a href='https://probloxworld.com' target='_blank'>probloxworld</a> &mdash; v1.0</span>"
       "</div>\n";

    // JS
    h+=R"RAWJS(<script>
// Clock
function tick(){document.getElementById('clock').textContent=new Date().toLocaleTimeString();}
tick();setInterval(tick,1000);

// Tab switching — persisted in URL hash
function showTab(id){
  document.querySelectorAll('.section').forEach(function(s){s.classList.remove('active');});
  document.querySelectorAll('.nt').forEach(function(t){t.classList.remove('active');});
  var sec=document.getElementById('s-'+id);
  if(sec)sec.classList.add('active');
  document.querySelectorAll('.nt').forEach(function(t){
    if(t.getAttribute('onclick')&&t.getAttribute('onclick').indexOf("'"+id+"'")>-1)
      t.classList.add('active');
  });
  history.replaceState(null,'','#'+id);
}
// Restore tab from URL hash on load
(function(){
  var h=location.hash.replace('#','');
  if(h&&document.getElementById('s-'+h))showTab(h);
  else showTab(')RAWJS"+activeTab+R"RAWJS(');
})();

// Toast
function toast(msg,type){
  var d=document.createElement('div');
  d.className='tmsg t'+(type||'ok');
  d.textContent=msg;
  document.getElementById('toast').appendChild(d);
  setTimeout(function(){if(d.parentNode)d.parentNode.removeChild(d);},3500);
}
)RAWJS";

    // Emit flash if any
    if(!flash.empty()){
        std::string ft=(flashType=="err")?"err":(flashType=="info")?"info":"ok";
        h+="toast("+("'"+escH(flash)+"'")+",'"+ft+"');\n";
    }

    h+=R"RAWJS(
// AJAX toggle — hits /pronoadmin/toggle?key=X&val=0/1
function doToggle(key,el){
  el.classList.toggle('on');
  var val=el.classList.contains('on')?'1':'0';
  fetch('/pronoadmin/toggle?key='+encodeURIComponent(key)+'&val='+val)
    .then(function(r){return r.text();})
    .then(function(t){toast(t,'ok');})
    .catch(function(){toast('Toggle failed — check connection','err');});
}

// Auto-refresh overview every 20s
setInterval(function(){
  if(document.getElementById('s-overview').classList.contains('active'))location.reload();
},20000);
</script>
</body></html>
)RAWJS";

    return h;
}

// ═══════════════════════════════════════════════════════════════════════════
// SHARED REQUEST PARSER
// ═══════════════════════════════════════════════════════════════════════════
struct ParsedReq {
    std::string method,rawPath,cleanPath,queryStr,body,ip;
    std::map<std::string,std::string> hdrs,params;
};

bool parseRequest(tcp::socket& socket, ParsedReq& req)
{
    try{
        boost::asio::streambuf buf;
        boost::asio::read_until(socket,buf,"\r\n\r\n");
        std::istream rs(&buf);
        rs>>req.method>>req.rawPath;
        std::string ver; rs>>ver; rs.ignore(1);
        std::string ln;
        while(std::getline(rs,ln)&&ln!="\r"){
            auto c=ln.find(':');
            if(c!=std::string::npos){
                std::string k=ln.substr(0,c),v=ln.substr(c+2);
                if(!v.empty()&&v.back()=='\r')v.pop_back();
                std::transform(k.begin(),k.end(),k.begin(),::tolower);
                req.hdrs[k]=v;
            }
        }
        if(req.hdrs.count("content-length")){
            int cl=0;
            try{cl=std::stoi(req.hdrs["content-length"]);}catch(...){}
            if(cl>0&&cl<16*1024*1024){
                req.body.resize(cl);
                auto already=(int)buf.in_avail();
                if(already>0){
                    int take=std::min(already,cl);
                    std::istream(&buf).read(&req.body[0],take);
                    if(take<cl)boost::asio::read(socket,boost::asio::buffer(&req.body[take],cl-take));
                }else boost::asio::read(socket,boost::asio::buffer(req.body));
            }
        }
        req.ip=socket.remote_endpoint().address().to_string();
        auto qp=req.rawPath.find('?');
        if(qp!=std::string::npos){
            req.cleanPath=req.rawPath.substr(0,qp);
            req.queryStr =req.rawPath.substr(qp);
        } else req.cleanPath=req.rawPath;
        req.cleanPath=urlDec(req.cleanPath);
        req.params=parseQS(req.queryStr);
        if(req.method=="POST"&&req.hdrs.count("content-type")&&
           req.hdrs["content-type"].find("application/x-www-form-urlencoded")!=std::string::npos)
            for(auto& kv:parseQS("?"+req.body))
                if(!req.params.count(kv.first))req.params[kv.first]=kv.second;
        return true;
    }catch(...){return false;}
}

// ═══════════════════════════════════════════════════════════════════════════
// ADMIN HANDLER  — listens on CFG.adminPort, serves only /pronoadmin/*
// ═══════════════════════════════════════════════════════════════════════════
void handleAdmin(tcp::socket socket)
{
    try{
        ParsedReq req;
        if(!parseRequest(socket,req))return;

        auto& cleanPath=req.cleanPath;
        auto& params   =req.params;
        auto& ip       =req.ip;

        // IP blocklist applies to admin too
        {
            std::lock_guard<std::mutex> lk(cfgMutex);
            if(CFG.blockedIPs.count(ip)){
                boost::asio::write(socket,boost::asio::buffer(
                    buildResp(errorPage(403,"Your IP ("+ip+") is blocked."),"text/html",403)));
                return;
            }
        }

        // Bare / → redirect to dashboard
        if(cleanPath=="/"||cleanPath.empty()){
            boost::asio::write(socket,boost::asio::buffer(redir("/pronoadmin")));
            return;
        }

        std::string resp,flash,flashType="ok";

        if(cleanPath=="/pronoadmin"||cleanPath=="/pronoadmin/"){
            resp=buildResp(dashboard());
        }
        else if(cleanPath=="/pronoadmin/files"){
            resp=buildResp(dashboard("files",params.count("dir")?params["dir"]:""));
        }
        // ── Config endpoints ──────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/savecfg"){
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                if(params.count("serverName")&&!params["serverName"].empty())CFG.serverName=params["serverName"];
                if(params.count("webroot")&&!params["webroot"].empty())CFG.webroot=params["webroot"];
                if(params.count("logFile")&&!params["logFile"].empty())CFG.logFile=params["logFile"];
                if(params.count("maxLog"))try{CFG.maxLogEntries=std::stoi(params["maxLog"]);}catch(...){}
                if(params.count("mainUrl"))CFG.mainUrl=params["mainUrl"];
                saveConfig(); // persist while lock is held
            } // lock released here before dashboard() / buildResp()
            flash="Core settings saved.";
            resp=buildResp(dashboard("config","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/savemethods"){
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.allowedMethods.clear();
                for(auto& m:{"GET","HEAD","POST","PUT","DELETE","OPTIONS","PATCH"})
                    if(params.count(std::string("m_")+m))CFG.allowedMethods.insert(m);
                saveConfig();
            }
            flash="Allowed methods updated.";
            resp=buildResp(dashboard("config","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/toggle"){
            std::string key=params.count("key")?params["key"]:"";
            bool val=params.count("val")&&params["val"]=="1";
            std::string msg=key+" → "+(val?"on":"off");
            std::lock_guard<std::mutex> lk(cfgMutex);
            if     (key=="dirListing")       CFG.dirListing=val;
            else if(key=="accessLog")        CFG.accessLog=val;
            else if(key=="colorConsole")     CFG.colorConsole=val;
            else if(key=="xcto")             CFG.xcto=val;
            else if(key=="xframe")           CFG.xframe=val;
            else if(key=="xss")              CFG.xss=val;
            else if(key=="hsts")             CFG.hsts=val;
            else if(key=="csp")              CFG.csp=val;
            else if(key=="corsEnabled")      CFG.corsEnabled=val;
            else if(key=="corsCredentials")  CFG.corsCredentials=val;
            else if(key=="basicAuthEnabled") CFG.basicAuthEnabled=val;
            else if(key=="rateLimitEnabled") CFG.rateLimitEnabled=val;
            else if(key=="gzipEnabled")      CFG.gzipEnabled=val;
            else if(key=="brotliEnabled")    CFG.brotliEnabled=val;
            else msg="Unknown key: "+key;
            resp=buildResp(msg,"text/plain");
        }
        else if(cleanPath=="/pronoadmin/savecsp"){
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                if(params.count("csp"))CFG.cspValue=params["csp"];
                if(params.count("ref"))CFG.referrerPolicy=params["ref"];
                saveConfig();
            }
            flash="Security header settings saved.";
            resp=buildResp(dashboard("headers","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/savecors"){
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                if(params.count("origin")) CFG.corsOrigin=params["origin"];
                if(params.count("methods"))CFG.corsMethods=params["methods"];
                if(params.count("headers"))CFG.corsHeaders=params["headers"];
                if(params.count("maxage"))try{CFG.corsMaxAge=std::stoi(params["maxage"]);}catch(...){}
                saveConfig();
            }
            flash="CORS settings saved.";
            resp=buildResp(dashboard("headers","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/addheader"){
            if(params.count("name")&&params.count("value")&&!params["name"].empty()){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.customHeaders[params["name"]]=params["value"];
                saveConfig();
                flash="Header added: "+params["name"];
            }else{flash="Header name and value required.";flashType="err";}
            resp=buildResp(dashboard("headers","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/removeheader"){
            if(params.count("name")){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.customHeaders.erase(params["name"]);
                saveConfig();
                flash="Header removed.";
            }
            resp=buildResp(dashboard("headers","",flash,flashType));
        }
        // ── Vhosts ────────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/addvhost"){
            if(params.count("host")&&params.count("root")&&!params["host"].empty()&&!params["root"].empty()){
                {
                    std::lock_guard<std::mutex> lk(cfgMutex);
                    CFG.vhosts[params["host"]]=params["root"];
                    fs::create_directories(params["root"]);
                    saveConfig();
                }
                flash="Vhost added: "+params["host"];
            }else{flash="Hostname and root are required.";flashType="err";}
            resp=buildResp(dashboard("vhosts","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/removevhost"){
            if(params.count("host")){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.vhosts.erase(params["host"]);
                saveConfig();
                flash="Vhost removed.";
            }
            resp=buildResp(dashboard("vhosts","",flash,flashType));
        }
        // ── Rewrites ───────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/addrewrite"){
            if(params.count("pattern")&&params.count("replacement")&&!params["pattern"].empty()){
                RewriteRule rr;
                rr.pattern=params["pattern"];
                rr.replacement=params["replacement"];
                rr.code=params.count("code")?std::stoi(params["code"]):302;
                {
                    std::lock_guard<std::mutex> lk(cfgMutex);
                    CFG.rewrites.push_back(rr);
                    saveConfig();
                }
                flash="Rewrite rule added.";
            }else{flash="Pattern and replacement are required.";flashType="err";}
            resp=buildResp(dashboard("rewrites","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/removerewrite"){
            if(params.count("i")){
                std::lock_guard<std::mutex> lk(cfgMutex);
                int idx=std::stoi(params["i"]);
                if(idx>=0&&idx<(int)CFG.rewrites.size())
                    CFG.rewrites.erase(CFG.rewrites.begin()+idx);
                saveConfig();
                flash="Rewrite rule removed.";
            }
            resp=buildResp(dashboard("rewrites","",flash,flashType));
        }
        // ── Cache ──────────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/addcache"){
            if(params.count("ext")&&params.count("val")&&!params["ext"].empty()){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.cacheRules[params["ext"]]=params["val"];
                saveConfig();
                flash="Cache rule added for "+params["ext"];
            }else{flash="Extension and value are required.";flashType="err";}
            resp=buildResp(dashboard("cache","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/removecache"){
            if(params.count("ext")){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.cacheRules.erase(params["ext"]);
                saveConfig();
                flash="Cache rule removed.";
            }
            resp=buildResp(dashboard("cache","",flash,flashType));
        }
        // ── Rate limit ─────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/saverl"){
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                if(params.count("rlRate")) try{CFG.rateLimitPerMin=std::stoi(params["rlRate"]);}catch(...){}
                if(params.count("rlBurst"))try{CFG.rateLimitBurst=std::stoi(params["rlBurst"]);}catch(...){}
                saveConfig();
            }
            flash="Rate limit settings saved.";
            resp=buildResp(dashboard("ratelimit","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/clearrl"){
            {std::lock_guard<std::mutex> lk(rlMutex);rlState.clear();}
            flash="Rate limit state cleared.";
            resp=buildResp(dashboard("ratelimit","",flash,flashType));
        }
        // ── Firewall        // ── Firewall ───────────────────────────────────────────────────
        // ── Firewall ───────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/block"){
            if(params.count("ip")&&!params["ip"].empty()){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.blockedIPs.insert(params["ip"]);
                saveConfig();
                flash="IP blocked: "+params["ip"];
            }else{flash="IP address required.";flashType="err";}
            resp=buildResp(dashboard("firewall","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/unblock"){
            if(params.count("ip")){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.blockedIPs.erase(params["ip"]);
                saveConfig();
                flash="IP unblocked: "+params["ip"];
            }
            resp=buildResp(dashboard("firewall","",flash,flashType));
        }
        // ── Stats ──────────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/resetstats"){
            requestCount=0;bytesSent=0;bytesRecv=0;err4xx=0;err5xx=0;req2xx=0;
            {std::lock_guard<std::mutex> lk(statsMutex);
             pathHits.clear();ipHits.clear();statusCounts.clear();methodCounts.clear();}
            flash="All stats reset.";
            resp=buildResp(dashboard("stats","",flash,flashType));
        }
        // ── Logs ───────────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/clearlog"){
            {std::lock_guard<std::mutex> lk(logMutex);accessLogs.clear();}
            std::ofstream(CFG.logFile,std::ios::trunc).close();
            flash="Logs cleared.";
            resp=buildResp(dashboard("logs","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/downloadlog"){
            std::lock_guard<std::mutex> lk(logMutex);
            std::string lc;for(auto& l:accessLogs)lc+=l+"\n";
            resp=buildResp(lc,"text/plain",200,
                "Content-Disposition: attachment; filename=\"prono.log\"\r\n");
        }
        // ── Error pages ────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/seterrorpage"){
            if(params.count("code")&&params.count("path")&&!params["path"].empty()){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.errorPages[std::stoi(params["code"])]=params["path"];
                saveConfig();
                flash="Error page override set.";
            }else{flash="Code and path required.";flashType="err";}
            resp=buildResp(dashboard("errorpages","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/removeerrorpage"){
            if(params.count("code")){
                std::lock_guard<std::mutex> lk(cfgMutex);
                CFG.errorPages.erase(std::stoi(params["code"]));
                saveConfig();
                flash="Override removed.";
            }
            resp=buildResp(dashboard("errorpages","",flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/previewerror"){
            int code=params.count("code")?std::stoi(params["code"]):404;
            resp=buildResp(errorPage(code),"text/html",code);
        }
        // ── Test config ────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/testconfig"){
            std::string r2;
            r2+="<!DOCTYPE html><html><head><title>Config Test</title><style>"+CSS
               +"body{padding:28px;}</style></head><body>"
               "<div class='sec-title'>Config Test Results</div><br>";
            r2+="<div class='notice "+std::string(fs::exists(CFG.webroot)?"n-ok":"n-err")+"'>"
               +std::string(fs::exists(CFG.webroot)?"✓":"✕")+" Webroot: "+escH(CFG.webroot)+"</div>";
            r2+="<div class='notice n-ok'>✓ Server Name: "+escH(CFG.serverName)+"</div>";
            r2+="<div class='notice n-ok'>✓ Web Port: "+std::to_string(CFG.port)+"</div>";
            r2+="<div class='notice n-ok'>✓ Admin Port: "+std::to_string(CFG.adminPort)+"</div>";
            r2+="<div class='notice n-info'>ℹ Vhosts: "+std::to_string(CFG.vhosts.size())+"</div>";
            r2+="<div class='notice n-info'>ℹ Rewrites: "+std::to_string(CFG.rewrites.size())+"</div>";
            r2+="<div class='notice n-info'>ℹ Blocked IPs: "+std::to_string(CFG.blockedIPs.size())+"</div>";
            r2+="<div class='notice n-info'>ℹ Rate Limit: "+std::string(CFG.rateLimitEnabled?"enabled":"disabled")+"</div>";
            r2+="<br><a href='/pronoadmin' class='btn btn-primary'>← Back to Dashboard</a>"
               "</body></html>";
            resp=buildResp(r2);
        }
        // ── File ops ───────────────────────────────────────────────────
        else if(cleanPath=="/pronoadmin/delete"){
            if(params.count("file")&&safePath(params["file"])){
                std::string fp=CFG.webroot+params["file"];
                if(fs::exists(fp)&&fs::is_regular_file(fp)){
                    fs::remove(fp);flash="Deleted: "+params["file"];
                }else{flash="File not found.";flashType="err";}
            }
            std::string d=params.count("dir")?params["dir"]:"";
            resp=buildResp(dashboard("files",d,flash,flashType));
        }
        else if(cleanPath=="/pronoadmin/download"){
            if(params.count("file")&&safePath(params["file"])){
                std::string fp=CFG.webroot+params["file"];
                if(fs::exists(fp)&&fs::is_regular_file(fp)){
                    std::string content=readFile(fp);
                    std::string fname=fs::path(fp).filename().string();
                    resp=buildResp(content,"application/octet-stream",200,
                        "Content-Disposition: attachment; filename=\""+fname+"\"\r\n");
                }else resp=buildResp(errorPage(404),"text/html",404);
            }else resp=redir("/pronoadmin/files");
        }
        else{
            // Unknown admin path
            resp=buildResp(errorPage(404,"Admin route not found."),"text/html",404);
        }

        boost::asio::write(socket,boost::asio::buffer(resp));
    }catch(...){}
}

// ═══════════════════════════════════════════════════════════════════════════
// STATIC FILE HANDLER  — listens on CFG.port, serves webroot only
// ═══════════════════════════════════════════════════════════════════════════
void handleClient(tcp::socket socket)
{
    try{
        ParsedReq req;
        if(!parseRequest(socket,req))return;

        auto& cleanPath=req.cleanPath;
        auto& params   =req.params;
        auto& method   =req.method;
        auto& rawPath  =req.rawPath;
        auto& hdrs     =req.hdrs;
        auto& ip       =req.ip;

        // IP blocklist
        {
            std::lock_guard<std::mutex> lk(cfgMutex);
            if(CFG.blockedIPs.count(ip)){
                boost::asio::write(socket,boost::asio::buffer(
                    buildResp(errorPage(403,"Your IP ("+ip+") is blocked."),"text/html",403)));
                return;
            }
        }
        // Rate limit
        if(!rlAllow(ip)){
            boost::asio::write(socket,boost::asio::buffer(
                buildResp(errorPage(429),"text/html",429)));
            return;
        }

        // Stats + log
        requestCount++;
        bytesRecv+=(long long)rawPath.size()+(long long)req.body.size();
        {
            std::lock_guard<std::mutex> lk(statsMutex);
            pathHits[cleanPath]++;
            ipHits[ip]++;
            methodCounts[method]++;
        }
        std::string logEntry=nowTs()+" | "+ip+" | "+method+" "+rawPath;
        addLog(logEntry);
        if(CFG.colorConsole)std::cout<<"\033[32m"<<logEntry<<"\033[0m\n";
        else std::cout<<logEntry<<"\n";

        std::string resp;

        if(!safePath(cleanPath)){
            err4xx++;statusCounts[403]++;
            resp=buildResp(errorPage(403,"Path traversal blocked."),"text/html",403);
        }
        else if(CFG.allowedMethods.find(method)==CFG.allowedMethods.end()){
            err4xx++;statusCounts[405]++;
            resp=buildResp(errorPage(405),"text/html",405);
        }
        else{
            // Apply rewrite rules
            std::string servePath=cleanPath;
            int rwCode=0;
            {
                std::lock_guard<std::mutex> lk(cfgMutex);
                for(auto& rr:CFG.rewrites){
                    try{
                        std::regex re(rr.pattern);
                        if(std::regex_search(servePath,re)){
                            std::string rep=std::regex_replace(servePath,re,rr.replacement);
                            rwCode=rr.code;servePath=rep;break;
                        }
                    }catch(...){}
                }
            }
            if(rwCode>0){resp=redir(servePath,rwCode);}
            else{
                // Vhost selection
                std::string docroot=CFG.webroot;
                {
                    std::lock_guard<std::mutex> lk(cfgMutex);
                    if(hdrs.count("host")){
                        std::string host=hdrs["host"];
                        auto cp=host.find(':');
                        if(cp!=std::string::npos)host=host.substr(0,cp);
                        if(CFG.vhosts.count(host))docroot=CFG.vhosts[host];
                    }
                }

                // Basic auth check
                bool needAuth=false;
                {
                    std::lock_guard<std::mutex> lk(cfgMutex);
                    if(CFG.basicAuthEnabled)
                        for(auto& pp:CFG.authPaths)
                            if(servePath.substr(0,pp.size())==pp){needAuth=true;break;}
                }
                if(needAuth&&!hdrs.count("authorization")){
                    resp=buildResp(errorPage(401),"text/html",401,
                        "WWW-Authenticate: Basic realm=\""+CFG.authRealm+"\"\r\n");
                    goto send;
                }

                std::string filePath=docroot+servePath;

                // Directory → try index files
                if(fs::exists(filePath)&&fs::is_directory(filePath)){
                    bool found=false;
                    for(auto& idx:{"index.html","index.htm","default.html"}){
                        std::string t=filePath+"/"+std::string(idx);
                        if(fs::exists(t)){filePath=t;found=true;break;}
                    }
                    if(!found){
                        if(CFG.dirListing){
                            std::string rel=servePath.size()>1?servePath.substr(1):"";
                            std::string lst=
                                "<!DOCTYPE html><html><head>"
                                "<title>Index of "+escH(servePath)+"</title>"
                                "<style>"+CSS+"body{padding:24px;}</style></head><body>"
                                "<div class='sec-title'>📂 Index of "+escH(servePath)+"</div>"
                                "<div class='card' style='margin-top:16px'>"
                                "<div class='tbl-wrap'><table>"
                                "<thead><tr><th></th><th>Name</th><th>Size</th><th>Type</th><th>Modified</th></tr></thead>"
                                "<tbody>"+fileRows(rel)+"</tbody></table></div></div>"
                                "<div style='margin-top:16px;font-size:12px;color:var(--text3)'>"
                                "Prono Server &mdash; <a href='https://probloxworld.com'>probloxworld</a></div>"
                                "</body></html>";
                            req2xx++;statusCounts[200]++;
                            resp=buildResp(lst);
                        }else{
                            err4xx++;statusCounts[403]++;
                            resp=buildResp(errorPage(403,"Directory listing is disabled."),"text/html",403);
                        }
                    }
                }

                if(resp.empty()){
                    if(fs::exists(filePath)&&fs::is_regular_file(filePath)){
                        try{
                            std::string content=readFile(filePath);
                            std::string mime=mimeType(filePath);
                            bytesSent+=(long long)content.size();
                            req2xx++;statusCounts[200]++;
                            std::string ext=fs::path(filePath).extension().string();
                            std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
                            std::string cc,extra;
                            {
                                std::lock_guard<std::mutex> lk(cfgMutex);
                                if(CFG.cacheRules.count(ext))cc=CFG.cacheRules.at(ext);
                            }
                            if(!cc.empty())extra+="Cache-Control: "+cc+"\r\n";
                            resp=buildResp(content,mime,200,extra);
                        }catch(...){
                            err5xx++;statusCounts[500]++;
                            resp=buildResp(errorPage(500),"text/html",500);
                        }
                    }else{
                        err4xx++;statusCounts[404]++;
                        addLog("  [404] "+servePath);
                        resp=buildResp(errorPage(404,servePath),"text/html",404);
                    }
                }
            }
        }

        send:
        boost::asio::write(socket,boost::asio::buffer(resp));

    }catch(...){}
}


// ═══════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc,char* argv[])
{
    // ── Resolve the directory the executable lives in ────────────────────
    // This is critical for systemctl: CWD is "/" so relative paths fail.
    std::string exeDir;
    {
        char buf[PATH_MAX]={};
        ssize_t n=readlink("/proc/self/exe",buf,sizeof(buf)-1);
        if(n>0){
            buf[n]='\0';
            exeDir=std::string(dirname(buf));
        } else {
            exeDir=fs::current_path().string();
        }
    }

    if(argc>=2)CFG.port      =std::stoi(argv[1]);
    if(argc>=3)CFG.webroot   =argv[2];
    if(argc>=4)CFG.adminPort =std::stoi(argv[3]);

    // Make webroot, logFile, configFile absolute relative to exe dir
    auto makeAbs=[&](const std::string& p)->std::string{
        if(p.empty()||p[0]=='/')return p;
        return exeDir+"/"+p;
    };
    CFG.webroot    = makeAbs(CFG.webroot);
    CFG.logFile    = makeAbs(CFG.logFile);
    CFG.configFile = makeAbs("prono.cfg");

    // Load persisted config (overrides defaults, but CLI args override config)
    {
        int cliPort      = (argc>=2)?std::stoi(argv[1]):-1;
        int cliAdminPort = (argc>=4)?std::stoi(argv[3]):-1;
        std::string cliWebroot = (argc>=3)?std::string(argv[2]):"";
        loadConfig(CFG.configFile);
        // CLI args take precedence over saved config
        if(cliPort>0)       CFG.port=cliPort;
        if(cliAdminPort>0)  CFG.adminPort=cliAdminPort;
        if(!cliWebroot.empty())CFG.webroot=makeAbs(cliWebroot);
        // Re-absolutify after loadConfig (config may store relative paths)
        CFG.webroot = makeAbs(CFG.webroot);
        CFG.logFile = makeAbs(CFG.logFile);
    }

    fs::create_directories(CFG.webroot);
    if(!fs::exists(CFG.webroot+"/index.html")){
        std::ofstream f(CFG.webroot+"/index.html");
        f<<"<!DOCTYPE html><html><head><title>Prono</title>"
          "<style>"+CSS+"body{display:flex;flex-direction:column;align-items:center;"
          "justify-content:center;min-height:100vh;gap:16px;}"
          "h1{font-family:'Space Grotesk',sans-serif;font-weight:800;font-size:48px;color:var(--orange);}"
          "</style></head><body>"
          "<img src='https://cdn.brandfetch.io/idD7af6BB5/w/500/h/500/theme/dark/logo.png?c=1dxbfHSJFAPEGdCLU4o5B'"
          " style='width:64px;border-radius:12px' alt='probloxworld'>"
          "<h1>Prono Server</h1>"
          "<p style='color:var(--text2)'>Place your files in "
          "<code style='background:var(--bg2);padding:3px 8px;border-radius:5px;color:var(--accent)'>./www/</code></p>"
          "<p style='font-size:12px;color:var(--text3)'>Built by <a href='https://probloxworld.com'>probloxworld</a></p>"
          "</body></html>";
    }

    // Detect local LAN IP
    std::string lanIP="0.0.0.0";
    try{
        boost::asio::io_context tmp;
        tcp::resolver res(tmp);
        char host[256]={};gethostname(host,sizeof(host));
        for(auto& r:res.resolve(host,"")){
            auto a=r.endpoint().address();
            if(a.is_v4()&&!a.is_loopback()){lanIP=a.to_string();break;}
        }
    }catch(...){}

    std::cout<<"\n"
        "╔══════════════════════════════════════════════════════╗\n"
        "║  🔥  PRONO WEB SERVER  v1.0                          ║\n"
        "║      by probloxworld                                 ║\n"
        "╠══════════════════════════════════════════════════════╣\n"
        "║  WebRoot    : "<<CFG.webroot<<"\n"
        "║  Config     : "<<CFG.configFile<<"\n"
        "║  Web port   : "<<CFG.port<<"  → http://"<<lanIP<<":"<<CFG.port<<"/\n"
        "║  Admin port : "<<CFG.adminPort<<"  → http://"<<lanIP<<":"<<CFG.adminPort<<"/pronoadmin\n"
        "╚══════════════════════════════════════════════════════╝\n\n"
        "Usage: ./prono [webPort=8080] [webroot=./www] [adminPort=8081]\n\n";

    try{
        boost::asio::io_context io;

        // ── Web acceptor (static files) ──────────────────────────────
        tcp::acceptor webAcceptor(io);
        {
            tcp::endpoint ep(boost::asio::ip::address_v4::any(), CFG.port);
            webAcceptor.open(ep.protocol());
            webAcceptor.set_option(tcp::acceptor::reuse_address(true));
            webAcceptor.bind(ep);
            webAcceptor.listen(boost::asio::socket_base::max_listen_connections);
        }

        // ── Admin acceptor (dashboard) ───────────────────────────────
        tcp::acceptor adminAcceptor(io);
        {
            tcp::endpoint ep(boost::asio::ip::address_v4::any(), CFG.adminPort);
            adminAcceptor.open(ep.protocol());
            adminAcceptor.set_option(tcp::acceptor::reuse_address(true));
            adminAcceptor.bind(ep);
            adminAcceptor.listen(boost::asio::socket_base::max_listen_connections);
        }

        std::cout<<"[prono] Web server  listening on 0.0.0.0:"<<CFG.port<<"\n";
        std::cout<<"[prono] Admin panel listening on 0.0.0.0:"<<CFG.adminPort<<"\n\n";

        // Run admin acceptor loop on a dedicated thread
        std::thread adminThread([&]{
            while(true){
                tcp::socket sock(io);
                adminAcceptor.accept(sock);
                std::thread(handleAdmin,std::move(sock)).detach();
            }
        });
        adminThread.detach();

        // Main thread: web acceptor loop
        while(true){
            tcp::socket sock(io);
            webAcceptor.accept(sock);
            std::thread(handleClient,std::move(sock)).detach();
        }

    }catch(std::exception& e){
        std::cerr<<"\033[31mFatal: "<<e.what()<<"\033[0m\n";
        return 1;
    }
    return 0;
}
