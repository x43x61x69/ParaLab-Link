#pragma once

#include "catalog.h"
#include <vector>

namespace catalog {
constexpr int neonFilter = 3;
constexpr int favoriteFilter = 4;
constexpr int allFavorites = 9;

inline bool favorite(const uint8_t* favorites, unsigned index) {
    return favorites[index / 8] & (1 << (index % 8));
}

inline std::vector<unsigned> select(int kind, int filter, const uint8_t* favorites) {
    std::vector<unsigned> result;
    for (unsigned i = 0; i < catalogCount; ++i) {
        const auto& item = catalogItems[i];
        if (kind == allFavorites) {
            if (favorite(favorites, i)) result.push_back(i);
            continue;
        }
        if (item.kind != kind) continue;
        if (kind == 0 && filter >= 0) {
            if (filter < neonFilter && item.category != filter) continue;
            if (filter == neonFilter && i < neonGiftStart) continue;
            if (filter == favoriteFilter && !favorite(favorites, i)) continue;
        }
        if (kind == 2 && filter >= 0 && item.field != filter) continue;
        result.push_back(i);
    }
    return result;
}
}
