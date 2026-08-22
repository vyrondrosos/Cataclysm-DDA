#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ammo_effect.h"
#include "avatar.h"
#include "ballistics.h"
#include "calendar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "coordinates.h"
#include "damage.h"
#include "dispersion.h"
#include "faction.h"
#include "game.h"
#include "gun_mode.h"
#include "item.h"
#include "item_location.h"
#include "line.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "map_scale_constants.h"
#include "monster.h"
#include "npc.h"
#include "overwatch.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "pocket_type.h"
#include "projectile.h"
#include "ranged.h"
#include "timed_event.h"
#include "type_id.h"

static const activity_id ACT_PROVIDE_OVERWATCH( "ACT_PROVIDE_OVERWATCH" );

static const ammo_effect_str_id ammo_effect_JET( "JET" );

static const faction_id faction_your_followers( "your_followers" );

static const damage_type_id damage_bullet( "bullet" );

static const furn_str_id furn_f_counter( "f_counter" );

static const gun_mode_id gun_mode_AUTO( "AUTO" );
static const gun_mode_id gun_mode_DEFAULT( "DEFAULT" );
static const gun_mode_id gun_mode_M203_DEFAULT( "M203_DEFAULT" );

static const itype_id itype_40x46mm_m433( "40x46mm_m433" );
static const itype_id itype_556( "556" );
static const itype_id itype_chemical_thrower( "chemical_thrower" );
static const itype_id itype_flamethrower( "flamethrower" );
static const itype_id itype_gas_chloramine( "gas_chloramine" );
static const itype_id itype_glock_19( "glock_19" );
static const itype_id itype_m203( "m203" );
static const itype_id itype_m79( "m79" );
static const itype_id itype_m107a1( "m107a1" );
static const itype_id itype_m2browning( "m2browning" );
static const itype_id itype_modular_m16a4( "modular_m16a4" );
static const itype_id itype_modular_m16_auto_rifle( "modular_m16_auto_rifle" );
static const itype_id itype_pressurized_tank_chem( "pressurized_tank_chem" );
static const itype_id itype_rifle_scope( "rifle_scope" );
static const itype_id itype_stanag30( "stanag30" );

static const skill_id skill_gun( "gun" );
static const skill_id skill_launcher( "launcher" );
static const skill_id skill_rifle( "rifle" );

static const ter_str_id ter_t_brick_wall( "t_brick_wall" );
static const ter_str_id ter_t_floor( "t_floor" );
static const ter_str_id ter_t_open_air( "t_open_air" );

namespace
{

static npc &make_overwatch_gunner( const tripoint_bub_ms &pos = tripoint_bub_ms( 50, 60, 0 ) )
{
    clear_avatar();
    clear_map();
    build_test_map( ter_t_floor.id() );
    set_time_to_day();

    npc &gunner = spawn_npc( pos.xy(), "test_talker" );
    clear_character( gunner, true );
    gunner.setpos( get_map(), pos );
    gunner.set_attitude( NPCATT_FOLLOW );
    gunner.set_fac( faction_your_followers );
    gunner.set_str_base( 20 );
    gunner.set_skill_level( skill_gun, 10 );
    gunner.set_skill_level( skill_rifle, 10 );
    gunner.set_skill_level( skill_launcher, 10 );
    gunner.recalc_sight_limits();
    REQUIRE( gunner.is_player_ally() );
    return gunner;
}

static const overwatch::firing_mode *find_mode(
    const std::vector<overwatch::firing_mode> &modes, const gun_mode_id &id )
{
    const auto found = std::find_if( modes.begin(), modes.end(), [&id]( const auto & mode ) {
        return mode.id == id;
    } );
    return found == modes.end() ? nullptr : &*found;
}

static void clear_overwatch_events()
{
    timed_event_manager &events = get_timed_events();
    std::vector<std::pair<timed_event_type, std::string>> keys;
    for( const timed_event &event : events.get_all() ) {
        if( event.type == timed_event_type::OVERWATCH_FIRE ||
            event.type == timed_event_type::OVERWATCH_RELOAD ) {
            keys.emplace_back( event.type, event.key );
        }
    }
    for( const std::pair<timed_event_type, std::string> &entry : keys ) {
        events.remove( entry.first, entry.second );
    }
}

static item_location wield_empty_rifle( npc &gunner )
{
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    item_location rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    rifle->ammo_unset();
    REQUIRE( rifle->ammo_remaining() == 0 );
    return rifle;
}

static item_location give_loaded_rifle_magazine( npc &gunner, const int rounds )
{
    item magazine( itype_stanag30 );
    magazine.ammo_set( itype_556, rounds );
    item_location result = gunner.i_add( magazine );
    REQUIRE( result );
    REQUIRE( result->ammo_remaining() == rounds );
    return result;
}

static void make_clear_remote_corridor( const tripoint_abs_ms &from,
                                        const tripoint_abs_ms &to )
{
    map &reality = get_map();
    std::unique_ptr<map> remote;
    map *last_map = nullptr;
    const std::vector<tripoint> line = line_to( from.raw(), to.raw() );
    for( const tripoint &raw_point : line ) {
        const tripoint_abs_ms point( raw_point );
        map *segment = &reality;
        if( !reality.inbounds( point ) ) {
            if( !remote || !remote->inbounds( point ) ) {
                if( remote ) {
                    remote->save();
                }
                remote = std::make_unique<map>();
                const tripoint_abs_sm origin = project_to<coords::sm>( point ) -
                                               point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
                remote->load( origin, false );
            }
            segment = remote.get();
        }
        const tripoint_bub_ms bub = segment->get_bub( point );
        REQUIRE( segment->inbounds( bub ) );
        segment->ter_set( bub, ter_t_floor );
        segment->furn_set( bub, furn_str_id::NULL_ID() );
        if( segment->inbounds( bub + tripoint::above ) ) {
            segment->ter_set( bub + tripoint::above, ter_t_open_air );
        }
        last_map = segment;
    }
    if( remote ) {
        remote->save();
    }
    reality.invalidate_map_cache( from.z() );
    reality.build_map_cache( from.z(), true );
    if( last_map != nullptr && last_map != &reality ) {
        last_map->build_map_cache( to.z(), true );
    }
}

static void set_remote_terrain( const tripoint_abs_ms &point, const ter_str_id &terrain )
{
    map &reality = get_map();
    if( reality.inbounds( point ) ) {
        reality.ter_set( reality.get_bub( point ), terrain );
        reality.invalidate_map_cache( point.z() );
        reality.build_map_cache( point.z(), true );
        return;
    }

    map remote;
    const tripoint_abs_sm origin = project_to<coords::sm>( point ) -
                                   point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
    remote.load( origin, false );
    const tripoint_bub_ms local = remote.get_bub( point );
    REQUIRE( remote.inbounds( local ) );
    remote.ter_set( local, terrain );
    remote.save();
    reality.invalidate_map_cache( point.z() );
}

static void set_remote_furniture( const tripoint_abs_ms &point, const furn_str_id &furniture )
{
    map &reality = get_map();
    if( reality.inbounds( point ) ) {
        reality.furn_set( reality.get_bub( point ), furniture );
        reality.invalidate_map_cache( point.z() );
        reality.build_map_cache( point.z(), true );
        return;
    }

    map remote;
    const tripoint_abs_sm origin = project_to<coords::sm>( point ) -
                                   point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
    remote.load( origin, false );
    const tripoint_bub_ms local = remote.get_bub( point );
    REQUIRE( remote.inbounds( local ) );
    remote.furn_set( local, furniture );
    remote.save();
    reality.invalidate_map_cache( point.z() );
}

} // namespace

TEST_CASE( "overwatch_long_range_accuracy_distance", "[overwatch][ballistics]" )
{
    constexpr double normal_range = 60.0;
    const std::array<std::pair<double, double>, 7> examples = {{
            { 0.0, 0.0 },
            { 60.0, 60.0 },
            { 61.0, 60.5 },
            { 120.0, 90.0 },
            { 121.0, 90.0 + 1.0 / 3.0 },
            { 180.0, 110.0 },
            { 240.0, 130.0 },
        }
    };

    for( const std::pair<double, double> &example : examples ) {
        CAPTURE( example.first );
        CHECK( discounted_range( example.first, normal_range ) == Approx( example.second ) );
    }

    CHECK( discounted_range( normal_range - 0.001, normal_range ) <
           discounted_range( normal_range, normal_range ) );
    CHECK( discounted_range( normal_range, normal_range ) <
           discounted_range( normal_range + 0.001, normal_range ) );
    CHECK( discounted_range( normal_range * 2.0 - 0.001, normal_range ) <
           discounted_range( normal_range * 2.0, normal_range ) );
    CHECK( discounted_range( normal_range * 2.0, normal_range ) <
           discounted_range( normal_range * 2.0 + 0.001, normal_range ) );
}

TEST_CASE( "absolute_projectile_trace_crosses_map_windows_and_hits_remote_obstacles",
           "[overwatch][ballistics][projectile]" )
{
    clear_avatar();
    clear_map();
    build_test_map( ter_t_floor.id() );

    map &here = get_map();
    const tripoint_abs_ms source = here.get_abs( tripoint_bub_ms( 10, 10, 0 ) );
    const tripoint_abs_ms target = source + tripoint( 160, 0, 0 );
    const tripoint_abs_ms wall = source + tripoint( 145, 0, 0 );
    REQUIRE( here.inbounds( source ) );
    REQUIRE_FALSE( here.inbounds( wall ) );
    REQUIRE_FALSE( here.inbounds( target ) );

    make_clear_remote_corridor( source, target );

    projectile test_projectile;
    test_projectile.speed = 1000;
    test_projectile.range = rl_dist( source, target );
    test_projectile.impact = damage_instance( damage_bullet, 1 );
    test_projectile.critical_multiplier = 1.0f;

    dealt_projectile_attack attack;
    SECTION( "an unobstructed projectile reaches its absolute target" ) {
        projectile_attack( attack, test_projectile, &here, source, target,
                           dispersion_sources(), rl_dist( source, target ) );
        CHECK( attack.end_point_abs == target );
    }

    SECTION( "a wall in a remote map window stops the projectile" ) {
        set_remote_terrain( wall, ter_t_brick_wall );
        projectile_attack( attack, test_projectile, &here, source, target,
                           dispersion_sources(), rl_dist( source, target ) );
        CHECK( attack.end_point_abs == wall - tripoint::east );
        CHECK( attack.end_point_abs != target );
    }
}

TEST_CASE( "off_bubble_gunner_uses_bipod_on_mountable_furniture",
           "[overwatch][ballistics][bipod]" )
{
    npc &gunner = make_overwatch_gunner();
    map &here = get_map();
    const tripoint_abs_ms original_position = gunner.pos_abs();
    // Keep the temporary source map wholly separate from the reality bubble.  Loading a centered
    // map immediately beside the bubble would overlap its live submaps and make the fixture itself
    // ambiguous rather than exercising ordinary off-bubble map loading.
    const tripoint_abs_ms source = here.get_abs( tripoint_bub_ms::zero ) -
                                   tripoint( MAPSIZE_X + 24, 0, 0 );
    const tripoint_abs_ms target = source + tripoint( 8, 0, 0 );
    REQUIRE_FALSE( here.inbounds( source ) );
    REQUIRE_FALSE( here.inbounds( target ) );
    make_clear_remote_corridor( source, target );
    gunner.setpos( source, false );

    ranged_attack_context context;
    context.projectile_range = 8;
    context.accuracy_distance = 8.0;

    arm_shooter( gunner, itype_m107a1 );
    item_location rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    const int unmounted_weapon_recoil = rifle->gun_recoil( gunner, false );
    const int mounted_weapon_recoil = rifle->gun_recoil( gunner, true );
    REQUIRE( mounted_weapon_recoil < unmounted_weapon_recoil );
    gunner.recoil = 0;
    gunner.set_moves( 1000 );
    REQUIRE( gunner.fire_gun( target, 1, *rifle, context ) == 1 );
    const double recoil_on_floor = gunner.recoil;

    arm_shooter( gunner, itype_m107a1 );
    rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    set_remote_furniture( source, furn_f_counter );
    gunner.recoil = 0;
    gunner.set_moves( 1000 );
    REQUIRE( gunner.fire_gun( target, 1, *rifle, context ) == 1 );
    const double recoil_on_mountable_furniture = gunner.recoil;

    CHECK( recoil_on_mountable_furniture < recoil_on_floor );
    gunner.setpos( original_position, false );
}

TEST_CASE( "assigned_off_bubble_gunner_fires_across_map_windows_into_reality_bubble",
           "[overwatch][ballistics][timed_event]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_m107a1 );
    item_location rifle = gunner.get_wielded_item();
    REQUIRE( rifle );

    map &here = get_map();
    monster &target = spawn_test_monster( "mon_zombie_hulk", tripoint_bub_ms( 100, 60, 0 ), false );
    const tripoint_abs_ms source = target.pos_abs() - tripoint( 3 * MAX_VIEW_DISTANCE, 0, 0 );
    REQUIRE( here.inbounds( target.pos_abs() ) );
    REQUIRE_FALSE( here.inbounds( source ) );
    REQUIRE( rl_dist( source, target.pos_abs() ) == 3 * MAX_VIEW_DISTANCE );

    make_clear_remote_corridor( source, target.pos_abs() );
    set_remote_furniture( source, furn_f_counter );
    gunner.setpos( source, false );
    REQUIRE( overwatch::assign( gunner ) );
    REQUIRE( overwatch::observer_can_see( gunner, target ) );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_order( gunner, target, false ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *event = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( event != nullptr );
    const overwatch_fire_event_data *queued = event->get_data<overwatch_fire_event_data>();
    REQUIRE( queued != nullptr );
    const overwatch_fire_event_data fire_data = *queued;
    events.remove( timed_event_type::OVERWATCH_FIRE, key );

    const int ammunition_before = rifle->ammo_remaining();
    REQUIRE( overwatch::actualize_fire_event( fire_data ) );
    rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    CHECK( rifle->ammo_remaining() == ammunition_before - 1 );
    CHECK( gunner.pos_abs() == source );
    CHECK( here.inbounds( target.pos_abs() ) );
}

TEST_CASE( "overwatch_weapon_mode_eligibility", "[overwatch][gun]" )
{
    npc &gunner = make_overwatch_gunner();

    SECTION( "single and true automatic rifle modes are available" ) {
        arm_shooter( gunner, itype_modular_m16_auto_rifle );

        const std::vector<overwatch::firing_mode> modes = overwatch::eligible_modes( gunner );
        const overwatch::firing_mode *single = find_mode( modes, gun_mode_DEFAULT );
        const overwatch::firing_mode *automatic = find_mode( modes, gun_mode_AUTO );
        REQUIRE( single != nullptr );
        REQUIRE( automatic != nullptr );
        CHECK( single->shots == 1 );
        CHECK_FALSE( single->automatic );
        CHECK( automatic->shots > 1 );
        CHECK( automatic->automatic );

        REQUIRE( overwatch::assign( gunner ) );
        REQUIRE( overwatch::select_fire_mode( gunner, false ) );
        CHECK( gunner.get_value( "overwatch_mode" ).str() == gun_mode_DEFAULT.str() );
        REQUIRE( overwatch::select_fire_mode( gunner, true ) );
        CHECK( gunner.get_value( "overwatch_mode" ).str() == gun_mode_AUTO.str() );
    }

    SECTION( "burst is not treated as fully automatic" ) {
        arm_shooter( gunner, itype_modular_m16a4 );
        std::string failure;
        CHECK_FALSE( overwatch::select_fire_mode( gunner, true, &failure ) );
        CHECK_FALSE( failure.empty() );
    }

    SECTION( "standalone launcher mode is available" ) {
        arm_shooter( gunner, itype_m79 );
        const std::vector<overwatch::firing_mode> modes = overwatch::eligible_modes( gunner );
        const overwatch::firing_mode *mode = find_mode( modes, gun_mode_DEFAULT );
        REQUIRE( mode != nullptr );
        CHECK( mode->shots == 1 );
        CHECK_FALSE( mode->automatic );
    }

    SECTION( "auxiliary launcher is inspected through its contributed gun mode" ) {
        arm_shooter( gunner, itype_modular_m16_auto_rifle );
        item_location rifle = gunner.get_wielded_item();
        REQUIRE( rifle );

        item launcher( itype_m203 );
        launcher.put_in( item( itype_40x46mm_m433, calendar::turn, 1 ), pocket_type::MAGAZINE );
        rifle->force_insert_item( launcher, pocket_type::MOD );
        REQUIRE( rifle->gun_all_modes().count( gun_mode_M203_DEFAULT ) == 1 );

        const std::vector<overwatch::firing_mode> modes = overwatch::eligible_modes( gunner );
        const overwatch::firing_mode *mode = find_mode( modes, gun_mode_M203_DEFAULT );
        REQUIRE( mode != nullptr );
        CHECK( mode->shots == 1 );
        CHECK_FALSE( mode->automatic );
        CHECK( overwatch::mode_is_eligible( gunner, gun_mode_M203_DEFAULT ) );
    }

    SECTION( "non-rifle firearm is rejected" ) {
        arm_shooter( gunner, itype_glock_19 );
        CHECK( overwatch::eligible_modes( gunner ).empty() );
    }

    SECTION( "flamethrower is rejected even though it uses launcher skill" ) {
        gunner.set_wielded_item( item( itype_flamethrower ) );
        CHECK( overwatch::eligible_modes( gunner ).empty() );
    }

    SECTION( "unloaded chemical spray is rejected from its accepted ammo types" ) {
        gunner.set_wielded_item( item( itype_chemical_thrower ) );
        REQUIRE( gunner.get_wielded_item() );
        REQUIRE( gunner.get_wielded_item()->ammo_remaining() == 0 );
        CHECK( overwatch::eligible_modes( gunner ).empty() );
    }

    SECTION( "loaded chemical spray mode is rejected by ammo effects" ) {
        item chemical_thrower( itype_chemical_thrower );
        item tank( itype_pressurized_tank_chem );
        tank.put_in( item( itype_gas_chloramine, calendar::turn, 100 ),
                     pocket_type::MAGAZINE );
        chemical_thrower.put_in( tank, pocket_type::MAGAZINE_WELL );
        gunner.set_wielded_item( chemical_thrower );

        const gun_mode mode = gunner.get_wielded_item()->gun_current_mode();
        REQUIRE( mode );
        REQUIRE( mode->ammo_effects().count( ammo_effect_JET ) == 1 );
        CHECK( overwatch::eligible_modes( gunner ).empty() );
    }

    SECTION( "mounted gun requires a mountable surface" ) {
        gunner.set_wielded_item( item( itype_m2browning ) );
        REQUIRE( gunner.get_wielded_item() );
        CHECK( overwatch::eligible_modes( gunner ).empty() );

        map &here = get_map();
        here.furn_set( gunner.pos_bub( here ), furn_f_counter );
        here.invalidate_map_cache( gunner.pos_bub( here ).z() );
        here.build_map_cache( gunner.pos_bub( here ).z(), true );

        const std::vector<overwatch::firing_mode> modes = overwatch::eligible_modes( gunner );
        REQUIRE( find_mode( modes, gun_mode_DEFAULT ) != nullptr );
        REQUIRE( find_mode( modes, gun_mode_AUTO ) != nullptr );
        CHECK( overwatch::assign( gunner ) );
    }
}

TEST_CASE( "overwatch_assignment_roots_npc_at_post", "[overwatch][npc][activity]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    const tripoint_abs_ms assigned_post = gunner.pos_abs();

    std::string failure;
    REQUIRE( overwatch::assign( gunner, &failure ) );
    CHECK( failure.empty() );
    CHECK( overwatch::is_assigned( gunner ) );
    CHECK( gunner.activity.id() == ACT_PROVIDE_OVERWATCH );
    REQUIRE( gunner.get_value( "overwatch_post" ).is_tripoint() );
    CHECK( gunner.get_value( "overwatch_post" ).tripoint() == assigned_post );
    CHECK( gunner.mission == NPC_MISSION_ACTIVITY );

    gunner.set_moves( 100 );
    gunner.activity.do_turn( gunner );
    CHECK( gunner.pos_abs() == assigned_post );
    CHECK( overwatch::is_assigned( gunner ) );

    gunner.setpos( assigned_post + tripoint::east, false );
    gunner.activity.do_turn( gunner );
    CHECK_FALSE( gunner.activity );
    CHECK_FALSE( overwatch::is_assigned( gunner ) );
    CHECK( gunner.get_value( "overwatch_post" ).is_empty() );
}

TEST_CASE( "overwatch_observer_visibility_checks_light_and_walls", "[overwatch][vision]" )
{
    npc &gunner = make_overwatch_gunner();
    map &here = get_map();
    const tripoint_bub_ms target_pos = gunner.pos_bub( here ) + tripoint( 8, 0, 0 );
    monster &target = spawn_test_monster( "mon_zombie", target_pos, false );

    REQUIRE( overwatch::observer_can_see( gunner, target ) );

    SECTION( "opaque terrain blocks the lane" ) {
        REQUIRE( here.ter_set( gunner.pos_bub( here ) + tripoint( 4, 0, 0 ),
                               ter_t_brick_wall ) );
        here.invalidate_map_cache( 0 );
        here.build_map_cache( 0, true );
        CHECK_FALSE( overwatch::observer_can_see( gunner, target ) );
    }

    SECTION( "an unlit target is not visible at night" ) {
        set_time( calendar::turn_zero );
        gunner.recalc_sight_limits();
        CHECK_FALSE( overwatch::observer_can_see( gunner, target ) );

        set_time_to_day();
        gunner.recalc_sight_limits();
        CHECK( overwatch::observer_can_see( gunner, target ) );
    }
}

TEST_CASE( "overwatch_order_honors_maximum_range_boundary", "[overwatch][range]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle, { itype_rifle_scope } );
    REQUIRE( overwatch::assign( gunner ) );

    map &here = get_map();
    monster &target = spawn_test_monster( "mon_zombie_hulk",
                                         gunner.pos_bub( here ) + tripoint::east, false );
    const tripoint_abs_ms limit = gunner.pos_abs() + tripoint( overwatch::max_range, 0, 0 );
    make_clear_remote_corridor( gunner.pos_abs(), limit + tripoint::east );
    target.setpos( limit, false );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );

    CAPTURE( gunner.pos_abs() );
    CAPTURE( target.pos_abs() );
    REQUIRE( rl_dist( gunner.pos_abs(), target.pos_abs() ) == overwatch::max_range );
    REQUIRE( overwatch::observer_can_see( gunner, target ) );
    CHECK( overwatch::issue_order( gunner, target, false ) );
    overwatch::cancel_order( gunner, false );

    target.setpos( limit + tripoint::east, false );
    REQUIRE( rl_dist( gunner.pos_abs(), target.pos_abs() ) == overwatch::max_range + 1 );
    std::string failure;
    CHECK_FALSE( overwatch::issue_order( gunner, target, false, &failure ) );
    CHECK_FALSE( failure.empty() );
}

TEST_CASE( "overwatch_single_order_event_lifecycle", "[overwatch][timed_event]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    REQUIRE( overwatch::assign( gunner ) );

    map &here = get_map();
    monster &target = spawn_test_monster( "mon_zombie_hulk",
                                         gunner.pos_bub( here ) + tripoint( 8, 0, 0 ), false );
    REQUIRE( overwatch::observer_can_see( gunner, target ) );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_order( gunner, target, false ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    REQUIRE_FALSE( key.empty() );
    timed_event *event = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( event != nullptr );
    const overwatch_fire_event_data *queued = event->get_data<overwatch_fire_event_data>();
    REQUIRE( queued != nullptr );
    CHECK( queued->gunner_id == gunner.getID() );
    CHECK( queued->target_monster >= 0 );
    CHECK( queued->target_pos == target.pos_abs() );
    CHECK( queued->mode_id == gun_mode_DEFAULT.str() );
    CHECK( queued->order_key == key );
    CHECK_FALSE( queued->repeat );
    REQUIRE( queued->aim_moves > 0 );
    const int expected_aim_turns = std::max( 1, ( queued->aim_moves + gunner.get_speed() - 1 ) /
                                             gunner.get_speed() );
    CHECK( event->when == calendar::turn + time_duration::from_turns( expected_aim_turns ) );

    const overwatch_fire_event_data fire_data = *queued;
    events.remove( timed_event_type::OVERWATCH_FIRE, key );
    const int ammunition_before = gunner.get_wielded_item()->ammo_remaining();
    REQUIRE( overwatch::actualize_fire_event( fire_data ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == ammunition_before - 1 );
    CHECK( gunner.get_value( "overwatch_order_key" ).is_empty() );
    CHECK( events.get( timed_event_type::OVERWATCH_FIRE, key ) == nullptr );
    CHECK( overwatch::is_assigned( gunner ) );
}

TEST_CASE( "overwatch_repeat_order_reschedules_and_stops_on_lost_sight",
           "[overwatch][timed_event]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    gunner.set_speed_base( 1 );
    REQUIRE( gunner.get_speed() == 1 );
    REQUIRE( overwatch::assign( gunner ) );

    map &here = get_map();
    monster &target = spawn_test_monster( "mon_zombie_hulk",
                                         gunner.pos_bub( here ) + tripoint( 8, 0, 0 ), false );
    REQUIRE( overwatch::observer_can_see( gunner, target ) );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_order( gunner, target, true ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *first = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( first != nullptr );
    const overwatch_fire_event_data *first_payload =
        first->get_data<overwatch_fire_event_data>();
    REQUIRE( first_payload != nullptr );
    REQUIRE( first_payload->repeat );

    const overwatch_fire_event_data first_fire = *first_payload;
    events.remove( timed_event_type::OVERWATCH_FIRE, key );
    const int ammunition_before = gunner.get_wielded_item()->ammo_remaining();
    REQUIRE( overwatch::actualize_fire_event( first_fire ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == ammunition_before - 1 );
    CHECK( gunner.get_value( "overwatch_order_key" ).str() == key );

    timed_event *second = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( second != nullptr );
    const overwatch_fire_event_data *second_payload =
        second->get_data<overwatch_fire_event_data>();
    REQUIRE( second_payload != nullptr );
    CHECK( second_payload->repeat );
    CHECK( second_payload->target_monster == first_fire.target_monster );
    REQUIRE( second_payload->aim_moves > 0 );
    const int aim_only_turns = std::max( 1,
                                        ( second_payload->aim_moves + gunner.get_speed() - 1 ) /
                                        gunner.get_speed() );
    CHECK( second->when > calendar::turn + time_duration::from_turns( aim_only_turns ) );

    const overwatch_fire_event_data second_fire = *second_payload;
    events.remove( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( here.ter_set( gunner.pos_bub( here ) + tripoint( 4, 0, 0 ),
                           ter_t_brick_wall ) );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, true );
    const int ammunition_after_first = gunner.get_wielded_item()->ammo_remaining();
    CHECK_FALSE( overwatch::actualize_fire_event( second_fire ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == ammunition_after_first );
    CHECK( gunner.get_value( "overwatch_order_key" ).is_empty() );
    CHECK( events.get( timed_event_type::OVERWATCH_FIRE, key ) == nullptr );
    CHECK( overwatch::is_assigned( gunner ) );
}

TEST_CASE( "overwatch_repeat_shot_recovery_precedes_automatic_reload",
           "[overwatch][reload][timed_event]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    gunner.set_speed_base( 1 );
    REQUIRE( gunner.get_speed() == 1 );
    item_location rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    REQUIRE( rifle->magazine_current() != nullptr );
    rifle->magazine_current()->ammo_set( itype_556, 1 );
    give_loaded_rifle_magazine( gunner, 6 );
    REQUIRE( overwatch::assign( gunner ) );

    map &here = get_map();
    monster &target = spawn_test_monster( "mon_zombie_hulk",
                                         gunner.pos_bub( here ) + tripoint( 8, 0, 0 ), false );
    REQUIRE( overwatch::observer_can_see( gunner, target ) );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_order( gunner, target, true ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *fire_event = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( fire_event != nullptr );
    const overwatch_fire_event_data *fire_payload =
        fire_event->get_data<overwatch_fire_event_data>();
    REQUIRE( fire_payload != nullptr );
    const overwatch_fire_event_data fire_data = *fire_payload;
    events.remove( timed_event_type::OVERWATCH_FIRE, key );

    REQUIRE( overwatch::actualize_fire_event( fire_data ) );
    CHECK( rifle->ammo_remaining() == 0 );
    timed_event *reload_event = events.get( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( reload_event != nullptr );
    const overwatch_fire_event_data *reload_payload =
        reload_event->get_data<overwatch_fire_event_data>();
    REQUIRE( reload_payload != nullptr );
    CHECK( reload_payload->continue_fire );
    CHECK( reload_payload->repeat );
    REQUIRE( reload_payload->reload_moves > 0 );
    const int reload_only_turns = std::max(
                                      1, ( reload_payload->reload_moves +
                                           gunner.get_speed() - 1 ) /
                                      gunner.get_speed() );
    CHECK( reload_event->when > calendar::turn +
           time_duration::from_turns( reload_only_turns ) );

    const overwatch_fire_event_data reload_data = *reload_payload;
    events.remove( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( overwatch::actualize_reload_event( reload_data ) );
    rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    CHECK( rifle->ammo_remaining() == 6 );
    fire_event = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( fire_event != nullptr );
    fire_payload = fire_event->get_data<overwatch_fire_event_data>();
    REQUIRE( fire_payload != nullptr );
    CHECK( fire_payload->repeat );
}

TEST_CASE( "overwatch_explicit_reload_uses_normal_selection_and_time",
           "[overwatch][reload]" )
{
    npc &gunner = make_overwatch_gunner();
    gunner.set_speed_base( 80 );
    REQUIRE( gunner.get_speed() == 80 );
    item_location rifle = wield_empty_rifle( gunner );
    give_loaded_rifle_magazine( gunner, 5 );
    give_loaded_rifle_magazine( gunner, 20 );
    REQUIRE( overwatch::assign( gunner ) );
    const tripoint_abs_ms assigned_post = gunner.pos_abs();

    const item::reload_option expected = gunner.select_ammo( rifle );
    REQUIRE( expected );
    REQUIRE( expected.ammo->ammo_remaining() == 20 );
    const int expected_moves = expected.moves();
    REQUIRE( expected_moves > 0 );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_reload( gunner ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *event = events.get( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( event != nullptr );
    const overwatch_fire_event_data *queued = event->get_data<overwatch_fire_event_data>();
    REQUIRE( queued != nullptr );
    CHECK( queued->reload_moves == expected_moves );
    const int expected_turns = std::max( 1, ( expected_moves + gunner.get_speed() - 1 ) /
                                         gunner.get_speed() );
    CHECK( event->when == calendar::turn + time_duration::from_turns( expected_turns ) );
    CHECK_FALSE( queued->continue_fire );
    CHECK( queued->target_pos == assigned_post );

    const overwatch_fire_event_data reload_data = *queued;
    events.remove( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( overwatch::actualize_reload_event( reload_data ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == 20 );
    CHECK( gunner.pos_abs() == assigned_post );
    CHECK( overwatch::is_assigned( gunner ) );
    CHECK( gunner.get_value( "overwatch_order_key" ).is_empty() );
}

TEST_CASE( "overwatch_explicit_reload_replaces_a_partial_magazine",
           "[overwatch][reload]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    item_location rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    REQUIRE( rifle->magazine_current() != nullptr );
    rifle->magazine_current()->ammo_set( itype_556, 2 );
    REQUIRE( rifle->ammo_remaining() == 2 );
    give_loaded_rifle_magazine( gunner, 20 );
    REQUIRE( overwatch::assign( gunner ) );

    const item::reload_option expected = gunner.select_ammo( rifle );
    REQUIRE( expected );
    REQUIRE( expected.ammo->ammo_remaining() == 20 );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_reload( gunner ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *event = events.get( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( event != nullptr );
    const overwatch_fire_event_data *queued = event->get_data<overwatch_fire_event_data>();
    REQUIRE( queued != nullptr );
    CHECK( queued->reload_moves == expected.moves() );

    const overwatch_fire_event_data reload_data = *queued;
    events.remove( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( overwatch::actualize_reload_event( reload_data ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == 20 );
    CHECK( overwatch::is_assigned( gunner ) );
}

TEST_CASE( "overwatch_empty_weapon_order_reloads_then_aims_and_fires",
           "[overwatch][reload][timed_event]" )
{
    npc &gunner = make_overwatch_gunner();
    item_location rifle = wield_empty_rifle( gunner );
    give_loaded_rifle_magazine( gunner, 6 );
    REQUIRE( overwatch::assign( gunner ) );
    const tripoint_abs_ms assigned_post = gunner.pos_abs();

    map &here = get_map();
    monster &target = spawn_test_monster( "mon_zombie_hulk",
                                         gunner.pos_bub( here ) + tripoint( 8, 0, 0 ), false );
    REQUIRE( overwatch::observer_can_see( gunner, target ) );
    const item::reload_option expected_reload = gunner.select_ammo( rifle );
    REQUIRE( expected_reload );

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_order( gunner, target, false ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *reload_event = events.get( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( reload_event != nullptr );
    const overwatch_fire_event_data *reload_payload =
        reload_event->get_data<overwatch_fire_event_data>();
    REQUIRE( reload_payload != nullptr );
    CHECK( reload_payload->reload_moves == expected_reload.moves() );
    CHECK( reload_payload->continue_fire );
    CHECK_FALSE( reload_payload->repeat );
    CHECK( reload_payload->target_pos == target.pos_abs() );

    const overwatch_fire_event_data reload_data = *reload_payload;
    events.remove( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( overwatch::actualize_reload_event( reload_data ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == 6 );
    CHECK( gunner.pos_abs() == assigned_post );

    timed_event *fire_event = events.get( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( fire_event != nullptr );
    const overwatch_fire_event_data *fire_payload =
        fire_event->get_data<overwatch_fire_event_data>();
    REQUIRE( fire_payload != nullptr );
    CHECK( fire_payload->aim_moves >= 0 );
    CHECK_FALSE( fire_payload->repeat );
    CHECK( fire_payload->target_monster == reload_data.target_monster );

    const overwatch_fire_event_data fire_data = *fire_payload;
    events.remove( timed_event_type::OVERWATCH_FIRE, key );
    REQUIRE( overwatch::actualize_fire_event( fire_data ) );
    CHECK( gunner.get_wielded_item()->ammo_remaining() == 5 );
    CHECK( gunner.pos_abs() == assigned_post );
    CHECK( overwatch::is_assigned( gunner ) );
    CHECK( gunner.get_value( "overwatch_order_key" ).is_empty() );
}

TEST_CASE( "overwatch_reload_without_compatible_ammo_fails_at_post",
           "[overwatch][reload]" )
{
    npc &gunner = make_overwatch_gunner();
    wield_empty_rifle( gunner );
    REQUIRE( overwatch::assign( gunner ) );
    const tripoint_abs_ms assigned_post = gunner.pos_abs();

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );

    std::string failure;
    CHECK_FALSE( overwatch::issue_reload( gunner, &failure ) );
    CHECK_FALSE( failure.empty() );
    CHECK( gunner.pos_abs() == assigned_post );
    CHECK( overwatch::is_assigned( gunner ) );
    CHECK( gunner.get_value( "overwatch_order_key" ).is_empty() );
    CHECK_FALSE( get_timed_events().queued( timed_event_type::OVERWATCH_RELOAD ) );

    monster &target = spawn_test_monster( "mon_zombie_hulk",
                                         gunner.pos_bub( get_map() ) + tripoint( 8, 0, 0 ), false );
    failure.clear();
    CHECK_FALSE( overwatch::issue_order( gunner, target, false, &failure ) );
    CHECK_FALSE( failure.empty() );
    CHECK( gunner.pos_abs() == assigned_post );
    CHECK( overwatch::is_assigned( gunner ) );
    CHECK( gunner.get_value( "overwatch_order_key" ).is_empty() );
    CHECK_FALSE( get_timed_events().queued( timed_event_type::OVERWATCH_RELOAD ) );
    CHECK_FALSE( get_timed_events().queued( timed_event_type::OVERWATCH_FIRE ) );
}

TEST_CASE( "overwatch_auxiliary_launcher_reload_bills_mode_target",
           "[overwatch][reload][gunmod]" )
{
    npc &gunner = make_overwatch_gunner();
    arm_shooter( gunner, itype_modular_m16_auto_rifle );
    item_location rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    const int rifle_ammunition = rifle->ammo_remaining();

    rifle->force_insert_item( item( itype_m203 ), pocket_type::MOD );
    REQUIRE( rifle->gun_all_modes().count( gun_mode_M203_DEFAULT ) == 1 );
    gun_mode launcher_mode = rifle->gun_get_mode( gun_mode_M203_DEFAULT );
    REQUIRE( launcher_mode );
    REQUIRE( launcher_mode.target != rifle.get_item() );
    REQUIRE( launcher_mode->ammo_remaining() == 0 );
    gunner.i_add( item( itype_40x46mm_m433, calendar::turn, 1 ) );

    REQUIRE( overwatch::assign( gunner ) );
    REQUIRE( overwatch::select_fire_mode( gunner, gun_mode_M203_DEFAULT ) );
    const item_location launcher_location( rifle, launcher_mode.target );
    const item::reload_option expected = gunner.select_ammo( launcher_location );
    REQUIRE( expected );
    REQUIRE( expected.ammo->typeId() == itype_40x46mm_m433 );
    const int expected_moves = expected.moves();

    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_reload( gunner ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *event = events.get( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( event != nullptr );
    const overwatch_fire_event_data *queued = event->get_data<overwatch_fire_event_data>();
    REQUIRE( queued != nullptr );
    CHECK( queued->mode_id == gun_mode_M203_DEFAULT.str() );
    CHECK( queued->reload_moves == expected_moves );

    const overwatch_fire_event_data reload_data = *queued;
    events.remove( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( overwatch::actualize_reload_event( reload_data ) );

    rifle = gunner.get_wielded_item();
    REQUIRE( rifle );
    launcher_mode = rifle->gun_get_mode( gun_mode_M203_DEFAULT );
    REQUIRE( launcher_mode );
    CHECK( launcher_mode->ammo_remaining() == 1 );
    CHECK( rifle->ammo_remaining() == rifle_ammunition );
    CHECK( overwatch::is_assigned( gunner ) );
}

TEST_CASE( "off_bubble_overwatch_reload_persists_nearby_ground_ammo",
           "[overwatch][reload][map]" )
{
    npc &gunner = make_overwatch_gunner();
    item_location rifle = wield_empty_rifle( gunner );
    REQUIRE( rifle );

    map &reality = get_map();
    const tripoint_abs_ms source = reality.get_abs( tripoint_bub_ms::zero ) -
                                   tripoint( MAPSIZE_X + 24, 0, 0 );
    REQUIRE_FALSE( reality.inbounds( source ) );
    set_remote_terrain( source, ter_t_floor );
    gunner.setpos( source, false );

    constexpr int magazine_rounds = 12;
    {
        map source_map;
        const tripoint_abs_sm origin = project_to<coords::sm>( source ) -
                                       point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
        source_map.load( origin, false );
        const tripoint_bub_ms local = source_map.get_bub( source );
        item magazine( itype_stanag30 );
        magazine.ammo_set( itype_556, magazine_rounds );
        source_map.add_item_or_charges( local, magazine );
        source_map.save();
    }

    REQUIRE( overwatch::assign( gunner ) );
    clear_overwatch_events();
    const auto remove_events = on_out_of_scope( []() {
        clear_overwatch_events();
    } );
    timed_event_manager &events = get_timed_events();

    REQUIRE( overwatch::issue_reload( gunner ) );
    const std::string key = gunner.get_value( "overwatch_order_key" ).str();
    timed_event *event = events.get( timed_event_type::OVERWATCH_RELOAD, key );
    REQUIRE( event != nullptr );
    const overwatch_fire_event_data *queued = event->get_data<overwatch_fire_event_data>();
    REQUIRE( queued != nullptr );
    const overwatch_fire_event_data reload_data = *queued;
    events.remove( timed_event_type::OVERWATCH_RELOAD, key );

    REQUIRE( overwatch::actualize_reload_event( reload_data ) );
    CHECK( rifle->ammo_remaining() == magazine_rounds );

    map verification_map;
    const tripoint_abs_sm origin = project_to<coords::sm>( source ) -
                                   point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
    verification_map.load( origin, false );
    const tripoint_bub_ms local = verification_map.get_bub( source );
    int ground_rounds = 0;
    for( const item &it : verification_map.i_at( local ) ) {
        if( it.typeId() == itype_stanag30 ) {
            ground_rounds += it.ammo_remaining();
        }
    }
    CHECK( ground_rounds == 0 );
}
