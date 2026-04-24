#pragma once
#include <string>
#include <vector>

namespace matata {

struct Category {
    std::wstring              name;        // e.g. L"video"
    std::wstring              subdir;      // subfolder to route into
    std::vector<std::wstring> extensions;  // lowercase, no leading dot
};

// Default IDM-style categories: video/music/archive/program/document.
// Any unknown extension -> "other".
const std::vector<Category>& defaultCategories();

// Return the category that matches the given filename (by extension).
// Never returns null — unmatched files get the synthetic "other" category.
const Category* categorize(const std::wstring& filename);

}
