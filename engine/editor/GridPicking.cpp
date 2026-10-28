#include "engine/editor/GridPicking.h"
#include <cmath>
#include <limits>

namespace fe {
std::optional<GridHit> pickGrid(const NavGrid& grid,glm::vec3 origin,glm::vec3 direction) {
    if (!std::isfinite(glm::length(origin)) || !std::isfinite(glm::length(direction)) || glm::length(direction)<.0001f) return {};
    direction=glm::normalize(direction);std::optional<GridHit> closest;
    // Editor maps are deliberately compact; exact cell planes also cover ramps.
    for (int z=0;z<grid.depth();++z) for (int x=0;x<grid.width();++x) {
        const auto& cell=grid.at(x,z);float slopeX=0,slopeZ=0,constant=cell.height;
        switch (cell.ramp) {
        case Ramp::East:slopeX=1;constant-=x;break;
        case Ramp::West:slopeX=-1;constant+=x+1;break;
        case Ramp::South:slopeZ=1;constant-=z;break;
        case Ramp::North:slopeZ=-1;constant+=z+1;break;
        default:break;
        }
        const float denominator=direction.y-slopeX*direction.x-slopeZ*direction.z;
        if (std::abs(denominator)<1e-7f) continue;
        const float distance=(constant+slopeX*origin.x+slopeZ*origin.z-origin.y)/denominator;
        if (distance<0 || (closest && distance>=closest->distance)) continue;
        const auto p=origin+direction*distance;
        if (p.x>=x && p.x<=x+1 && p.z>=z && p.z<=z+1) closest=GridHit{{x,z},p,distance};
    }
    return closest;
}
}
