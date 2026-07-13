#include "erwt3d/slice.hpp"
#include "erwt3d/morton.hpp"
#include <algorithm>
#include <cstring>

namespace erwt3d {

// Compute 2D dst index: row * out_dim0 + col
// X slice: row=z, col=y -> dst = z*ny + y
// Y slice: row=z, col=x -> dst = z*nx + x
// Z slice: row=y, col=x -> dst = y*nx + x

SlicePlan planSlice(const ERWT3DHeader& header, const SliceRequest& request) {
    SlicePlan plan;
    
    const uint64_t nx = header.nx;
    const uint64_t ny = header.ny;
    const uint64_t nz = header.nz;
    const uint64_t sx = header.super_x;
    const uint64_t sy = header.super_y;
    const uint64_t super_z = header.super_z;
    const uint64_t lx = header.leaf_x;
    const uint64_t ly = header.leaf_y;
    const uint64_t lz = header.leaf_z;
    
    const uint64_t superBytes = getSuperblockBytes(header);
    const uint64_t leafBytes = getLeafBytes(header);
    const uint64_t leafsPerSuperX = getLeafsPerSuperX(header);
    const uint64_t leafsPerSuperY = getLeafsPerSuperY(header);
    const uint64_t leafsPerSuperZ = getLeafsPerSuperZ(header);
    
    switch (request.axis) {
        case SliceAxis::X: {
            plan.axis = SliceAxis::X;
            plan.out_dim0 = ny;
            plan.out_dim1 = nz;
            plan.out_dim2 = 1;
            
            uint64_t x = request.index;
            uint64_t superX = x / sx;
            uint64_t localX = x % sx;
            uint64_t leafX = localX / lx;
            uint64_t inLeafX = localX % lx;
            
            for (uint64_t szi = 0; szi < getSuperGridZ(header); ++szi) {
                for (uint64_t syi = 0; syi < getSuperGridY(header); ++syi) {
                    uint64_t superIdx = (szi * getSuperGridY(header) + syi) * getSuperGridX(header) + superX;
                    uint64_t superOffset = superblockFileOffset(header, szi, syi, superX);
                    
                    for (uint64_t lz2 = 0; lz2 < leafsPerSuperZ; ++lz2) {
                        for (uint64_t ly2 = 0; ly2 < leafsPerSuperY; ++ly2) {
                            uint64_t leafMorton = morton3D(leafX, ly2, lz2);
                            uint64_t leafOffset = superOffset + leafMorton * leafBytes;
                            
                            uint64_t dstY = syi * sy + ly2 * ly;
                            uint64_t dstZ = szi * super_z + lz2 * lz;
                            if (dstY >= ny || dstZ >= nz) continue;
                            
                            plan.extents.emplace_back(leafOffset, leafBytes);
                            
                            SlicePlan::CopyInstr instr;
                            instr.src_offset = plan.extents.size() - 1;
                            // X slice: row=z, col=y -> dst = z*ny + y
                            instr.base_dst_idx = dstZ * ny + dstY;
                            instr.size_x = 1;
                            instr.size_y = std::min(ly, ny - dstY);
                            instr.size_z = std::min(lz, nz - dstZ);
                            instr.src_off_x = inLeafX;
                            instr.src_off_y = 0;
                            instr.src_off_z = 0;
                            plan.copies.push_back(instr);
                        }
                    }
                }
            }
            break;
        }
        
        case SliceAxis::Y: {
            plan.axis = SliceAxis::Y;
            plan.out_dim0 = nx;
            plan.out_dim1 = nz;
            plan.out_dim2 = 1;
            
            uint64_t y = request.index;
            uint64_t superY = y / sy;
            uint64_t localY = y % sy;
            uint64_t leafY = localY / ly;
            uint64_t inLeafY = localY % ly;
            
            for (uint64_t szi = 0; szi < getSuperGridZ(header); ++szi) {
                for (uint64_t sxi = 0; sxi < getSuperGridX(header); ++sxi) {
                    uint64_t superIdx = (szi * getSuperGridY(header) + superY) * getSuperGridX(header) + sxi;
                    uint64_t superOffset = superblockFileOffset(header, szi, superY, sxi);
                    
                    for (uint64_t lz2 = 0; lz2 < leafsPerSuperZ; ++lz2) {
                        for (uint64_t lx2 = 0; lx2 < leafsPerSuperX; ++lx2) {
                            uint64_t leafMorton = morton3D(lx2, leafY, lz2);
                            uint64_t leafOffset = superOffset + leafMorton * leafBytes;
                            
                            uint64_t dstX = sxi * sx + lx2 * lx;
                            uint64_t dstZ = szi * super_z + lz2 * lz;
                            if (dstX >= nx || dstZ >= nz) continue;
                            
                            plan.extents.emplace_back(leafOffset, leafBytes);
                            
                            SlicePlan::CopyInstr instr;
                            instr.src_offset = plan.extents.size() - 1;
                            // Y slice: row=z, col=x -> dst = z*nx + x
                            instr.base_dst_idx = dstZ * nx + dstX;
                            instr.size_x = std::min(lx, nx - dstX);
                            instr.size_y = 1;
                            instr.size_z = std::min(lz, nz - dstZ);
                            instr.src_off_x = 0;
                            instr.src_off_y = inLeafY;
                            instr.src_off_z = 0;
                            plan.copies.push_back(instr);
                        }
                    }
                }
            }
            break;
        }
        
        case SliceAxis::Z: {
            plan.axis = SliceAxis::Z;
            plan.out_dim0 = nx;
            plan.out_dim1 = ny;
            plan.out_dim2 = 1;
            
            uint64_t z = request.index;
            uint64_t superZ = z / super_z;
            uint64_t localZ = z % super_z;
            uint64_t leafZ = localZ / lz;
            uint64_t inLeafZ = localZ % lz;
            
            for (uint64_t syi = 0; syi < getSuperGridY(header); ++syi) {
                for (uint64_t sxi = 0; sxi < getSuperGridX(header); ++sxi) {
                    uint64_t superIdx = (superZ * getSuperGridY(header) + syi) * getSuperGridX(header) + sxi;
                    uint64_t superOffset = superblockFileOffset(header, superZ, syi, sxi);
                    
                    for (uint64_t ly2 = 0; ly2 < leafsPerSuperY; ++ly2) {
                        for (uint64_t lx2 = 0; lx2 < leafsPerSuperX; ++lx2) {
                            uint64_t leafMorton = morton3D(lx2, ly2, leafZ);
                            uint64_t leafOffset = superOffset + leafMorton * leafBytes;
                            
                            uint64_t dstX = sxi * sx + lx2 * lx;
                            uint64_t dstY = syi * sy + ly2 * ly;
                            if (dstX >= nx || dstY >= ny) continue;
                            
                            plan.extents.emplace_back(leafOffset, leafBytes);
                            
                            SlicePlan::CopyInstr instr;
                            instr.src_offset = plan.extents.size() - 1;
                            // Z slice: row=y, col=x -> dst = y*nx + x
                            instr.base_dst_idx = dstY * nx + dstX;
                            instr.size_x = std::min(lx, nx - dstX);
                            instr.size_y = std::min(ly, ny - dstY);
                            instr.size_z = 1;
                            instr.src_off_x = 0;
                            instr.src_off_y = 0;
                            instr.src_off_z = inLeafZ;
                            plan.copies.push_back(instr);
                        }
                    }
                }
            }
            break;
        }
    }
    
    return plan;
}

// Legacy path — not used in current performance readSlice path (use executePreparedSlice instead)
void executeSlice(const ERWT3DHeader& header, const SlicePlan& plan, 
                  const void* readBuffer, void* outputBuffer) {
    const uint64_t lx = header.leaf_x;
    const uint64_t ly = header.leaf_y;
    const uint64_t nx = header.nx;
    const uint64_t ny = header.ny;
    
    float* out = static_cast<float*>(outputBuffer);
    const uint8_t* readBuf = static_cast<const uint8_t*>(readBuffer);
    
    std::vector<Extent> mergedExtents = mergeExtents(plan.extents);
    
    std::vector<uint64_t> mergedStarts;
    uint64_t totalSize = 0;
    for (const auto& ext : mergedExtents) {
        mergedStarts.push_back(totalSize);
        totalSize += ext.size;
    }
    
    for (const auto& copy : plan.copies) {
        const Extent& origExt = plan.extents[copy.src_offset];
        
        uint64_t srcStart = 0;
        for (size_t i = 0; i < mergedExtents.size(); ++i) {
            if (origExt.offset >= mergedExtents[i].offset && 
                origExt.offset < mergedExtents[i].end()) {
                srcStart = mergedStarts[i] + (origExt.offset - mergedExtents[i].offset);
                break;
            }
        }
        
        const float* src = reinterpret_cast<const float*>(readBuf + srcStart);
        
        for (uint64_t dz = 0; dz < copy.size_z; ++dz) {
            for (uint64_t dy = 0; dy < copy.size_y; ++dy) {
                for (uint64_t dx = 0; dx < copy.size_x; ++dx) {
                    uint64_t srcIdx = ((copy.src_off_z + dz) * ly + (copy.src_off_y + dy)) * lx + (copy.src_off_x + dx);
                    uint64_t dstIdx = 0;
                    
                    switch (plan.axis) {
                        case SliceAxis::X:
                            // output layout: z * ny + y
                            dstIdx = ((copy.base_dst_idx / ny) + dz) * ny + (copy.base_dst_idx % ny + dy);
                            break;
                        case SliceAxis::Y:
                            // output layout: z * nx + x
                            dstIdx = ((copy.base_dst_idx / nx) + dz) * nx + (copy.base_dst_idx % nx + dx);
                            break;
                        case SliceAxis::Z:
                            // output layout: y * nx + x
                            dstIdx = ((copy.base_dst_idx / nx) + dy) * nx + (copy.base_dst_idx % nx + dx);
                            break;
                    }
                    
                    out[dstIdx] = src[srcIdx];
                }
            }
        }
    }
}

void executePreparedSlice(const ERWT3DHeader& header, const SlicePlan& plan,
                          const void* readBuffer, const std::vector<uint64_t>& mergedStarts,
                          const std::vector<Extent>& mergedExtents,
                          size_t batchStart, size_t batchEnd, void* outputBuffer) {
    const uint64_t lx = header.leaf_x;
    const uint64_t ly = header.leaf_y;
    const uint64_t nx = header.nx;
    
    float* out = static_cast<float*>(outputBuffer);
    const uint8_t* readBuf = static_cast<const uint8_t*>(readBuffer);
    
    for (size_t ci = 0; ci < plan.copies.size(); ++ci) {
        uint32_t mi = plan.copy_merged_idx[ci];
        if (mi < batchStart || mi >= batchEnd) continue;
        
        const auto& copy = plan.copies[ci];
        const uint8_t* src = readBuf + (plan.copy_merged_offset[ci] - mergedStarts[batchStart]);
        
        for (uint64_t dz = 0; dz < copy.size_z; ++dz) {
            for (uint64_t dy = 0; dy < copy.size_y; ++dy) {
                for (uint64_t dx = 0; dx < copy.size_x; ++dx) {
                    uint64_t srcIdx = ((copy.src_off_z + dz) * ly + (copy.src_off_y + dy)) * lx + (copy.src_off_x + dx);
                    uint64_t d = 0;
                    switch (plan.axis) {
                        case SliceAxis::X:
                            d = ((copy.base_dst_idx / header.ny) + dz) * header.ny + (copy.base_dst_idx % header.ny + dy);
                            break;
                        case SliceAxis::Y:
                            d = ((copy.base_dst_idx / nx) + dz) * nx + (copy.base_dst_idx % nx + dx);
                            break;
                        case SliceAxis::Z:
                            d = ((copy.base_dst_idx / nx) + dy) * nx + (copy.base_dst_idx % nx + dx);
                            break;
                    }
                    out[d] = reinterpret_cast<const float*>(src)[srcIdx];
                }
            }
        }
    }
}

void prepareSlicePlan(SlicePlan& plan) {
    if (plan.prepared) return;
    plan.prepared = true;
    plan.merged_extents = mergeExtents(plan.extents);
    
    // Compute buffer offset for each merged extent
    plan.merged_buffer_offsets.resize(plan.merged_extents.size());
    uint64_t total = 0;
    for (size_t i = 0; i < plan.merged_extents.size(); ++i) {
        plan.merged_buffer_offsets[i] = total;
        total += plan.merged_extents[i].size;
    }
    
    // Precompute mapping: for each copy, which merged extent + offset within it
    plan.copy_merged_idx.resize(plan.copies.size());
    plan.copy_merged_offset.resize(plan.copies.size());
    
    for (size_t ci = 0; ci < plan.copies.size(); ++ci) {
        const Extent& orig = plan.extents[plan.copies[ci].src_offset];
        bool found = false;
        for (size_t mi = 0; mi < plan.merged_extents.size(); ++mi) {
            if (orig.offset >= plan.merged_extents[mi].offset &&
                orig.offset < plan.merged_extents[mi].end()) {
                plan.copy_merged_idx[ci] = static_cast<uint32_t>(mi);
                plan.copy_merged_offset[ci] = plan.merged_buffer_offsets[mi] + 
                    (orig.offset - plan.merged_extents[mi].offset);
                found = true;
                break;
            }
        }
        if (!found) {
            plan.copy_merged_idx[ci] = 0;
            plan.copy_merged_offset[ci] = 0;
        }
    }
}

} // namespace erwt3d
