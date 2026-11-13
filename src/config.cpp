#include "config.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>

#include "spdlog/spdlog.h"

std::vector<std::tuple<int, std::string>> dlcs;
mINI::INIStructure ini;

static std::atomic<bool> g_config_loaded{false};

bool is_dlc_unlocked(AppId_t appID) {
    return std::find_if(
        std::begin(dlcs),
        std::end(dlcs),
        [&](const std::tuple<int, std::string>& a) { return std::get<0>(a) == appID; }) != std::end(dlcs);
}

bool is_unlockall() {
    return ini["config"]["unlockall"] == "true";
}

// Parses one ini line. mINI's parser is public but its reader only accepts a
// filename, so the Android path (config next to the .so, no writable cwd)
// needs this small in-memory equivalent.
static void parse_into(const std::string& text, const std::string& source) {
    ini.clear();
    dlcs.clear();

    std::string section;
    bool in_section = false;
    mINI::INIParser::T_ParseValues parse_data;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto result = mINI::INIParser::parseLine(line, parse_data);
        if (result == mINI::INIParser::PDataType::PDATA_SECTION) {
            in_section = true;
            section = parse_data.first;
            ini[section];
        } else if (in_section && result == mINI::INIParser::PDataType::PDATA_KEYVALUE) {
            ini[section][parse_data.first] = parse_data.second;
        }
    }

    for (const auto& entry : ini["dlc"]) {
        try {
            int id = std::stoi(entry.first);
            dlcs.emplace_back(id, entry.second);
            spdlog::info("Added dlc with id: {0}, name: {1}", entry.first, entry.second);
        } catch (const std::exception&) {
            spdlog::warn("Ignoring non-numeric DLC id '{}' in {}", entry.first, source);
        }
    }

    if (ini["config"]["logging"] == "false") {
        spdlog::set_level(spdlog::level::off);
    }
}

bool config_is_loaded() {
    return g_config_loaded.load();
}

void load_config(const std::string& creaminipath) {
    spdlog::info("Reading config from {}", creaminipath);

    std::ifstream file(creaminipath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        spdlog::warn("Could not open config file {}", creaminipath);
        ini.clear();
        dlcs.clear();
        return;
    }

    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    load_config_string(text, creaminipath);
}

void load_config_string(const std::string& text, const std::string& source) {
    parse_into(text, source);
    g_config_loaded.store(true);
}

std::string resolve_config_path(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return path + "/cream_api.ini";
    }
    return path;
}

bool config_file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string own_library_path() {
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&own_library_path), &info) != 0 &&
        info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
        return std::string(info.dli_fname);
    }
    return {};
}

std::string own_library_dir() {
    const std::string path = own_library_path();
    if (path.empty()) {
        return ".";
    }
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return path.substr(0, slash);
}

bool load_config_from_library_trailer() {
    const std::string path = own_library_path();
    if (path.empty()) {
        return false;
    }

    std::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    constexpr char kMagic[8] = {'C', 'R', 'E', 'A', 'M', 'C', 'F', 'G'};
    const auto size = static_cast<std::uint64_t>(file.tellg());
    if (size < 12) {
        return false;
    }

    file.seekg(static_cast<std::streamoff>(size - 12), std::ios::beg);
    char footer[12];
    if (!file.read(footer, sizeof(footer))) {
        return false;
    }
    if (std::memcmp(footer, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }

    const auto length = static_cast<std::uint32_t>(
        static_cast<unsigned char>(footer[8])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(footer[9])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(footer[10])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(footer[11])) << 24);
    if (length == 0 || length > size - 12) {
        return false;
    }

    file.seekg(static_cast<std::streamoff>(size - 12 - length), std::ios::beg);
    std::string text(length, '\0');
    if (!file.read(&text[0], length)) {
        return false;
    }

    spdlog::info("Reading embedded config trailer from {}", path);
    load_config_string(text, "embedded trailer");
    return true;
}

std::string default_config_path() {
    const char* env = std::getenv("CREAM_CONFIG_PATH");
    if (env != nullptr && env[0] != '\0') {
        return resolve_config_path(env);
    }
#ifdef __ANDROID__
    // An Android app process starts in "/" and cannot rely on the current
    // directory. The proxy and its ini live together in the app's native
    // library directory.
    return own_library_dir() + "/cream_api.ini";
#else
    return "cream_api.ini";
#endif
}
