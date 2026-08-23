#include "map_viewpoint.h"

#include "map.h"

map_viewpoint::map_viewpoint( const tripoint_abs_ms &origin, const int range ) :
    origin_( origin ), range_( range )
{
}

const tripoint_abs_ms &map_viewpoint::origin() const
{
    return origin_;
}

void map_viewpoint::set_origin( const tripoint_abs_ms &origin )
{
    origin_ = origin;
}

int map_viewpoint::range() const
{
    return range_;
}

void map_viewpoint::set_range( const int range )
{
    range_ = range;
}

bool map_viewpoint::sees( const map &here, const tripoint_bub_ms &target ) const
{
    if( !here.inbounds( origin_ ) || !here.inbounds( target ) ) {
        return false;
    }

    const tripoint_bub_ms origin = here.get_bub( origin_ );

    // map::sees() deliberately considers an opaque endpoint visible.  For a
    // downward view, however, the endpoint is the square *below* a floor, not
    // the visible upper surface of that floor.  Check the target column so a
    // viewpoint on a higher z-level cannot see the interior below a roof.
    for( int z = target.z() + 1; z <= origin.z(); ++z ) {
        const tripoint_bub_ms boundary( target.xy(), z );
        if( !here.inbounds( boundary ) || here.has_floor_or_support( boundary ) ) {
            return false;
        }
    }

    return here.sees( origin, target, range_ );
}
