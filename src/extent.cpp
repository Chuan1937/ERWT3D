#include "erwt3d/extent.hpp"
#include <algorithm>

namespace erwt3d {

std::vector<Extent> mergeExtents(const std::vector<Extent>& extents) {
    if (extents.empty()) {
        return {};
    }
    
    // Make a copy and sort by offset
    std::vector<Extent> sortedExtents = extents;
    std::sort(sortedExtents.begin(), sortedExtents.end(), 
              [](const Extent& a, const Extent& b) { return a.offset < b.offset; });
    
    std::vector<Extent> merged;
    merged.push_back(sortedExtents[0]);
    
    for (size_t i = 1; i < sortedExtents.size(); ++i) {
        Extent& last = merged.back();
        const Extent& curr = sortedExtents[i];
        
        // Check if current extent is adjacent or overlapping
        if (curr.offset <= last.end()) {
            // Merge: extend the last extent
            uint64_t newEnd = std::max(last.end(), curr.end());
            last.size = newEnd - last.offset;
        } else {
            // Add new extent
            merged.push_back(curr);
        }
    }
    
    return merged;
}

} // namespace erwt3d