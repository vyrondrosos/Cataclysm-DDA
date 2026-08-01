#pragma once
#ifndef CATA_SRC_MAP_VIEWPOINT_H
#define CATA_SRC_MAP_VIEWPOINT_H

#include "coordinates.h"

class map;

/**
 * A geometric viewpoint at an explicit map-square location.
 *
 * This only considers range and map line of sight.  Character senses and
 * creature detection are separate concerns for callers to apply.
 * The supplied map must have current LOS caches; call map::build_los_cache()
 * after loading or changing it.
 */
class map_viewpoint
{
    public:
        /** @param range Maximum map-square range; a negative value is unlimited. */
        map_viewpoint( const tripoint_abs_ms &origin, int range );

        const tripoint_abs_ms &origin() const;
        void set_origin( const tripoint_abs_ms &origin );

        int range() const;
        void set_range( int range );

        bool sees( const map &here, const tripoint_bub_ms &target ) const;

    private:
        tripoint_abs_ms origin_;
        int range_;
};

#endif // CATA_SRC_MAP_VIEWPOINT_H
