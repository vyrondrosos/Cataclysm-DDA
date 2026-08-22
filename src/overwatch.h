#pragma once
#ifndef CATA_SRC_OVERWATCH_H
#define CATA_SRC_OVERWATCH_H

#include <string>
#include <vector>

#include "map_scale_constants.h"
#include "type_id.h"

class Creature;
class npc;
struct overwatch_fire_event_data;

namespace overwatch
{

constexpr int max_range = 4 * MAX_VIEW_DISTANCE;

struct firing_mode {
    gun_mode_id id;
    std::string name;
    int shots = 1;
    bool automatic = false;
};

bool is_assigned( const npc &gunner );
std::vector<firing_mode> eligible_modes( const npc &gunner );
bool mode_is_eligible( const npc &gunner, const gun_mode_id &mode_id,
                       std::string *failure = nullptr );

bool assign( npc &gunner, std::string *failure = nullptr );
bool select_fire_mode( npc &gunner, const gun_mode_id &mode_id,
                       std::string *failure = nullptr );
bool select_fire_mode( npc &gunner, bool automatic, std::string *failure = nullptr );
bool issue_order( npc &gunner, Creature &target, bool repeat,
                  std::string *failure = nullptr );
bool issue_reload( npc &gunner, std::string *failure = nullptr );
void cancel_order( npc &gunner, bool notify = true );
void stand_down( npc &gunner, bool notify = true );

std::string status( const npc &gunner );
void report( const npc &gunner );

bool observer_can_see( const npc &gunner, const Creature &target );
bool actualize_fire_event( const overwatch_fire_event_data &event_data );
bool actualize_reload_event( const overwatch_fire_event_data &event_data );

} // namespace overwatch

#endif // CATA_SRC_OVERWATCH_H
