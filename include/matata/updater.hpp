#pragma once
#include <cstdint>
#include <string>

namespace matata {

struct UpdateInfo {
    bool         available   = false;    // true if remote > local
    std::wstring latestVersion;          // remote-advertised version
    std::wstring downloadUrl;            // URL of the installer
    std::wstring notes;                  // optional release notes
};

// Semantic-version compare: returns <0, 0, >0 like strcmp.
int compareVersions(const std::wstring& a, const std::wstring& b);

// Fetch a JSON manifest over HTTPS. Expected shape:
//   { "version": "0.7.2", "url": "https://.../matata-0.7.2-setup.exe",
//     "notes":   "..." }
// Compares against `currentVersion`. Returns true on a successful fetch;
// `info.available` reflects whether an upgrade was found.
bool checkForUpdate(const std::wstring& manifestUrl,
                    const std::wstring& currentVersion,
                    UpdateInfo& info, std::wstring& err);

// Current matata version baked in at compile time.
constexpr const wchar_t* kMatataVersion = L"0.9.9.11";

}
