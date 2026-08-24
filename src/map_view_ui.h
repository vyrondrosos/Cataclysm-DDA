#pragma once
#ifndef CATA_SRC_MAP_VIEW_UI_H
#define CATA_SRC_MAP_VIEW_UI_H

#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "coordinates.h"

class map;
class map_viewpoint;
class Creature;

/** A one-cell marker drawn over a visible map tile. */
struct map_view_ui_overlay {
    tripoint_abs_ms pos;
    std::string symbol;
    nc_color color;
    std::string description;
    /** Live creature represented by this marker, when one is available. */
    const Creature *creature = nullptr;
};

struct map_view_ui_params {
    std::string title;
    bool select = false;
    /** Percentage of the terminal occupied along each axis. */
    int window_percentage = 100;
    /** Initial display plane; defaults to the viewpoint origin. */
    std::optional<tripoint_abs_ms> center;
    std::vector<map_view_ui_overlay> overlays;
};

/** View a loaded map without changing the avatar or the reality bubble. */
std::optional<tripoint_abs_ms> query_map_view( map &viewed_map,
        const map_viewpoint &viewpoint, const map_view_ui_params &params );

#endif // CATA_SRC_MAP_VIEW_UI_H
