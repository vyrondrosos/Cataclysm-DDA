#include "avatar.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "monster.h"
#include "player_helpers.h"
#include "timed_event.h"

TEST_CASE( "FPV_terminal_impact_tracks_a_moving_creature", "[timed_event][npc][drone]" )
{
    clear_avatar();
    clear_map_without_vision();
    clear_creatures();

    timed_event_manager &events = get_timed_events();
    events.remove( timed_event_type::FPV_DRONE_TERMINAL_IMPACT, "" );
    const auto remove_event = on_out_of_scope( [&events]() {
        events.remove( timed_event_type::FPV_DRONE_TERMINAL_IMPACT, "" );
    } );

    avatar &you = get_avatar();
    const tripoint_bub_ms start = you.pos_bub() + tripoint_rel_ms{ 5, 0, 0 };
    monster &target = spawn_test_monster( "mon_zombie", start );
    const tripoint_abs_ms initial_target = target.pos_abs();

    fpv_terminal_impact_event_data impact_data;
    impact_data.target_monster = get_creature_tracker().temporary_id( target );
    impact_data.miss = tripoint_rel_ms{ 2, -1, 0 };
    events.add_fpv_terminal_impact( calendar::turn + 1_hours, initial_target, "",
                                    impact_data, explosion_data() );

    timed_event *event = events.get( timed_event_type::FPV_DRONE_TERMINAL_IMPACT );
    REQUIRE( event != nullptr );
    target.setpos( target.pos_abs() + tripoint_rel_ms::east );
    event->per_turn();

    CHECK( event->map_square == target.pos_abs() );
    const fpv_terminal_impact_event_data *tracked =
        event->get_data<fpv_terminal_impact_event_data>();
    REQUIRE( tracked != nullptr );
    CHECK( tracked->target_monster == get_creature_tracker().temporary_id( target ) );
    CHECK( tracked->miss == impact_data.miss );
}
