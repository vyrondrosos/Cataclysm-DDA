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
    return here.inbounds( origin_ ) && here.inbounds( target ) &&
           here.sees( here.get_bub( origin_ ), target, range_ );
}
