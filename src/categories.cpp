#include "matata/categories.hpp"

#include <algorithm>
#include <cwctype>

namespace matata {

namespace {

const std::vector<Category> kDefaults = {
    { L"video",    L"Video",    { L"mp4", L"mkv", L"avi", L"mov", L"webm", L"flv",
                                  L"wmv", L"mpg", L"mpeg", L"m4v", L"ts", L"3gp" } },
    { L"music",    L"Music",    { L"mp3", L"flac", L"wav", L"m4a", L"aac", L"ogg",
                                  L"opus", L"wma", L"alac", L"aiff" } },
    { L"archive",  L"Archives", { L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2",
                                  L"xz", L"zst", L"iso", L"cab" } },
    { L"program",  L"Programs", { L"exe", L"msi", L"apk", L"dmg", L"pkg", L"deb",
                                  L"rpm", L"appimage" } },
    { L"document", L"Documents",{ L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt",
                                  L"pptx", L"odt", L"ods", L"txt", L"rtf", L"epub",
                                  L"mobi" } },
};

const Category kOther = { L"other", L"Other", {} };

std::wstring lowerExt(const std::wstring& filename) {
    auto dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= filename.size()) return L"";
    std::wstring ext = filename.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext;
}

} // anon

const std::vector<Category>& defaultCategories() { return kDefaults; }

const Category* categorize(const std::wstring& filename) {
    std::wstring ext = lowerExt(filename);
    if (ext.empty()) return &kOther;
    for (auto& cat : kDefaults) {
        if (std::find(cat.extensions.begin(), cat.extensions.end(), ext)
              != cat.extensions.end()) {
            return &cat;
        }
    }
    return &kOther;
}

}
