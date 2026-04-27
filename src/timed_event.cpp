#include "timed_event.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "ammo_effect.h"
#include "avatar.h"
#include "avatar_action.h"
#include "character.h"
#include "coordinates.h"
#include "current_map.h"
#include "debug.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "explosion.h"
#include "game.h"
#include "iuse_actor.h"
#include "item.h"
#include "itype.h"
#include "line.h"
#include "magic.h"
#include "map.h"
#include "map_extras.h"
#include "map_iterator.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "mapgen_functions.h"
#include "mapgendata.h"
#include "mdarray.h"
#include "memorial_logger.h"
#include "messages.h"
#include "monster.h"
#include "rng.h"
#include "sounds.h"
#include "text_snippets.h"
#include "translation.h"
#include "translations.h"
#include "trap.h"
#include "type_id.h"

static const itype_id itype_landmine( "landmine" );
static const itype_id itype_petrified_eye( "petrified_eye" );

static const map_extra_id map_extra_mx_dsa_alrp( "mx_dsa_alrp" );

static const morale_type morale_scream( "morale_scream" );

static const mtype_id mon_amigara_horror( "mon_amigara_horror" );
static const mtype_id mon_dark_wyrm( "mon_dark_wyrm" );
static const mtype_id mon_dermatik( "mon_dermatik" );
static const mtype_id mon_dsa_alien_dispatch( "mon_dsa_alien_dispatch" );
static const mtype_id mon_sewer_snake( "mon_sewer_snake" );
static const mtype_id mon_spider_cellar_giant( "mon_spider_cellar_giant" );
static const mtype_id mon_spider_widow_giant( "mon_spider_widow_giant" );

static const spell_id spell_dks_summon_alrp( "dks_summon_alrp" );

static const ter_str_id ter_t_fault( "t_fault" );
static const ter_str_id ter_t_grate( "t_grate" );
static const ter_str_id ter_t_rock_floor( "t_rock_floor" );
static const ter_str_id ter_t_root_wall( "t_root_wall" );
static const ter_str_id ter_t_stairs_down( "t_stairs_down" );
static const ter_str_id ter_t_underbrush( "t_underbrush" );
static const ter_str_id ter_t_water_dp( "t_water_dp" );
static const ter_str_id ter_t_water_sh( "t_water_sh" );

static const trap_str_id tr_landmine( "tr_landmine" );

static int round_to_nearest( const double value, const int quantum )
{
    const int step = std::max( 1, quantum );
    return std::max( 0, static_cast<int>( std::round( value / step ) ) * step );
}

static std::string format_fpv_duration( int seconds )
{
    const int clamped = std::max( 0, seconds );
    if( clamped > 60 ) {
        return string_format( "%d:%02d", clamped / 60, clamped % 60 );
    }
    return string_format( n_gettext( "%d second", "%d seconds", clamped ), clamped );
}

struct mortar_impact_key {
    tripoint_abs_ms target = tripoint_abs_ms::invalid;
    std::optional<int> drone_pilot_skill;
};

struct fpv_payload_drop_data {
    std::string payload_id;
    std::string operator_name;
};

static std::optional<mortar_impact_key> parse_mortar_impact_key( const std::string &key )
{
    const size_t separator = key.find( '|' );
    const std::string target_key = key.substr( 0, separator );

    int x = 0;
    int y = 0;
    int z = 0;
    if( std::sscanf( target_key.c_str(), "%d,%d,%d", &x, &y, &z ) != 3 ) {
        return std::nullopt;
    }

    mortar_impact_key parsed;
    parsed.target = tripoint_abs_ms( x, y, z );
    if( separator != std::string::npos ) {
        int pilot_skill = 0;
        if( std::sscanf( key.c_str() + separator + 1, "%d", &pilot_skill ) == 1 ) {
            parsed.drone_pilot_skill = std::clamp( pilot_skill, 0, 10 );
        }
    }
    return parsed;
}

static fpv_payload_drop_data parse_fpv_payload_drop_string_id( const std::string &string_id )
{
    fpv_payload_drop_data parsed;
    const size_t separator = string_id.find( '\n' );
    if( separator == std::string::npos ) {
        parsed.payload_id = string_id;
    } else {
        parsed.payload_id = string_id.substr( 0, separator );
        parsed.operator_name = string_id.substr( separator + 1 );
    }
    return parsed;
}

static void apply_mortar_field( map &target_map, const tripoint_abs_ms &center_abs,
                                const field_type_str_id &field_type, const int intensity,
                                const int radius, const time_duration &age = 0_turns )
{
    const tripoint_bub_ms center = target_map.get_bub( center_abs );
    for( const tripoint_bub_ms &pt : points_in_radius_circ( center, radius ) ) {
        target_map.add_field( pt, field_type, intensity, age, false );
    }
}

static void apply_timed_explosion( Creature *source, map &here, const tripoint_abs_ms &impact_abs,
                                   const explosion_data &data )
{
    if( here.inbounds( impact_abs ) ) {
        explosion_handler::explosion( source, here.get_bub( impact_abs ), data );
        return;
    }

    map target_map;
    const tripoint_abs_sm origin( project_to<coords::sm>( impact_abs ) -
                                  point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE } );
    target_map.load( origin, true, false );
    swap_map swap( target_map );
    target_map.spawn_monsters( true, true );
    g->load_npcs( &target_map );
    explosion_handler::_make_explosion( &target_map, source, target_map.get_bub( impact_abs ), data );
    target_map.process_falling();
}

static bool detonate_fpv_payload_if_explosive( Creature *source, map &here, const item &payload,
        const tripoint_abs_ms &impact_abs )
{
    if( !payload.ammo_data() ) {
        return false;
    }

    bool detonated = false;
    for( const ammo_effect_str_id &ammo_eff : payload.ammo_data()->ammo->ammo_effects ) {
        const ammo_effect &effect = ammo_eff.obj();
        if( effect.aoe_explosion_data.power > 0 ) {
            apply_timed_explosion( source, here, impact_abs, effect.aoe_explosion_data );
            detonated = true;
        }
    }
    return detonated;
}

static const item_transformation *fpv_payload_arming_transform( const itype &payload_type )
{
    if( payload_type.transform_into ) {
        return &payload_type.transform_into.value();
    }
    const use_function *transform_use = payload_type.get_use( "transform" );
    if( transform_use == nullptr ) {
        return nullptr;
    }
    const iuse_transform *transform_actor = dynamic_cast<const iuse_transform *>
                                            ( transform_use->get_actor_ptr() );
    return transform_actor != nullptr ? &transform_actor->transform : nullptr;
}

static void place_live_fpv_payload( map &target_map, const itype_id &payload_id,
                                    const tripoint_abs_ms &impact_abs )
{
    const tripoint_bub_ms impact = target_map.get_bub( impact_abs );
    if( payload_id == itype_landmine ) {
        target_map.trap_set( impact, tr_landmine );
        return;
    }

    item payload( payload_id, calendar::turn, 1 );
    if( payload.ammo_data() && !payload.ammo_data()->ammo->drop.is_null() ) {
        const bool drop_active = payload.ammo_data()->ammo->drop_active;
        payload = item( payload.ammo_data()->ammo->drop, calendar::turn, 1 );
        if( drop_active ) {
            payload.activate();
        }
        target_map.add_item_or_charges( impact, payload, true );
        return;
    }

    if( const item_transformation *transform = fpv_payload_arming_transform( payload_id.obj() ) ) {
        transform->transform( nullptr, payload, true );
    } else {
        payload.activate();
    }
    target_map.add_item_or_charges( impact, payload, true );
}

timed_event::timed_event( timed_event_type e_t, const time_point &w, int f_id, tripoint_abs_ms p,
                          int s, std::string key )
    : type( e_t )
    , when( w )
    , faction_id( f_id )
    , map_square( p )
    , strength( s )
    , key( std::move( key ) )
{
    map_point = project_to<coords::sm>( map_square );
}

timed_event::timed_event( timed_event_type e_t, const time_point &w, int f_id, tripoint_abs_ms p,
                          int s, std::string s_id, std::string key )
    : type( e_t )
    , when( w )
    , faction_id( f_id )
    , map_square( p )
    , strength( s )
    , string_id( std::move( s_id ) )
    , key( std::move( key ) )
{
    map_point = project_to<coords::sm>( map_square );
}

timed_event::timed_event( timed_event_type e_t, const time_point &w, int f_id, tripoint_abs_ms p,
                          int s, std::string s_id, submap sr, std::string key )
    : type( e_t )
    , when( w )
    , faction_id( f_id )
    , map_square( p )
    , strength( s )
    , string_id( std::move( s_id ) )
    , key( std::move( key ) )
    , revert( std::move( sr ) )
{
    map_point = project_to<coords::sm>( map_square );
}

timed_event::timed_event( timed_event_type e_t, const time_point &w, const tripoint_abs_ms &p,
                          const explosion_data explos_data )
    : type( e_t )
    , when( w )
    , faction_id( -1 )
    , map_square( p )
    , strength( -1 )
{
    map_point = project_to<coords::sm>( map_square );
    expl_data = explos_data;
}


void timed_event::actualize()
{
    avatar &player_character = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms pos = player_character.pos_bub( here );

    switch( type ) {
        case timed_event_type::HELP:
            debugmsg( "Currently disabled while NPC and monster factions are being rewritten." );
            break;

        case timed_event_type::SPAWN_WYRMS: {
            if( here.get_abs_sub().z() >= 0 ) {
                return;
            }
            get_memorial().add(
                pgettext( "memorial_male", "Drew the attention of more dark wyrms!" ),
                pgettext( "memorial_female", "Drew the attention of more dark wyrms!" ) );

            // 50% chance to spawn a dark wyrm near every orifice on the level.
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                if( here.ter( p ) == ter_id( "t_orifice" ) ) {
                    g->place_critter_around( mon_dark_wyrm, p, 1 );
                }
            }

            // You could drop the flag, you know.
            if( player_character.has_amount( itype_petrified_eye, 1 ) ) {
                sounds::sound( pos, MAX_VIEW_DISTANCE, sounds::sound_t::alert,
                               _( "a tortured scream!" ),
                               false,
                               "shout",
                               "scream_tortured" );
                if( !player_character.is_deaf() ) {
                    add_msg( _( "The eye you're carrying lets out a tortured scream!" ) );
                    player_character.add_morale( morale_scream, -15, 0, 30_minutes, 30_seconds );
                }
            }

        }
        break;

        case timed_event_type::AMIGARA: {
            get_event_bus().send<event_type::angers_amigara_horrors>();
            int num_horrors = rng( 3, 5 );
            std::optional<tripoint_bub_ms> fault_point;
            bool horizontal = false;
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                if( here.ter( p ) == ter_t_fault ) {
                    fault_point = p;
                    horizontal = here.ter( p + tripoint::east ) == ter_t_fault ||
                                 here.ter( p + tripoint::west ) == ter_t_fault;
                    break;
                }
            }
            for( int i = 0; fault_point && i < num_horrors; i++ ) {
                for( int tries = 0; tries < 10; ++tries ) {
                    tripoint_bub_ms monp = pos;
                    if( horizontal ) {
                        monp.x() = rng( fault_point->x(), fault_point->x() + 2 * SEEX - 8 );
                        for( int n = -1; n <= 1; n++ ) {
                            if( here.ter( point_bub_ms( monp.x(), fault_point->y() + n ) ) == ter_t_rock_floor ) {
                                monp.y() = fault_point->y() + n;
                            }
                        }
                    } else {
                        // Vertical fault
                        monp.y() = rng( fault_point->y(), fault_point->y() + 2 * SEEY - 8 );
                        for( int n = -1; n <= 1; n++ ) {
                            if( here.ter( point_bub_ms( fault_point->x() + n, monp.y() ) ) == ter_t_rock_floor ) {
                                monp.x() = fault_point->x() + n;
                            }
                        }
                    }
                    if( g->place_critter_at( mon_amigara_horror, monp ) ) {
                        break;
                    }
                }
            }
        }
        break;

        case timed_event_type::ROOTS_DIE:
            get_event_bus().send<event_type::destroys_triffid_grove>();
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                if( here.ter( p ) == ter_t_root_wall && one_in( 3 ) ) {
                    here.ter_set( p, ter_t_underbrush );
                }
            }
            break;

        case timed_event_type::TEMPLE_OPEN: {
            get_event_bus().send<event_type::opens_temple>();
            bool saw_grate = false;
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                if( here.ter( p ) == ter_t_grate ) {
                    here.ter_set( p, ter_t_stairs_down );
                    if( !saw_grate && player_character.sees( here, p ) ) {
                        saw_grate = true;
                    }
                }
            }
            if( saw_grate ) {
                add_msg( _( "The nearby grates open to reveal a staircase!" ) );
            }
        }
        break;

        case timed_event_type::TEMPLE_FLOOD: {
            bool flooded = false;

            cata::mdarray<ter_id, point_bub_ms> flood_buf;
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                flood_buf[p.x()][p.y()] = here.ter( p );
            }
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                if( here.ter( p ) == ter_t_water_sh ) {
                    bool deepen = false;
                    for( const tripoint_bub_ms &w : points_in_radius( p, 1 ) ) {
                        if( here.ter( w ) == ter_t_water_dp ) {
                            deepen = true;
                            break;
                        }
                    }
                    if( deepen ) {
                        flood_buf[p.x()][p.y()] = ter_t_water_dp;
                        flooded = true;
                    }
                } else if( here.ter( p ) == ter_t_rock_floor ) {
                    bool flood = false;
                    for( const tripoint_bub_ms &w : points_in_radius( p, 1 ) ) {
                        const ter_id &t = here.ter( w );
                        if( t == ter_t_water_dp || t == ter_t_water_sh ) {
                            flood = true;
                            break;
                        }
                    }
                    if( flood ) {
                        flood_buf[p.x()][p.y()] = ter_t_water_sh;
                        flooded = true;
                    }
                }
            }
            if( !flooded ) {
                // We finished flooding the entire chamber!
                return;
            }
            // Check if we should print a message
            if( flood_buf[pos.x()][pos.y()] != here.ter(
                    pos ) ) {
                if( flood_buf[pos.x()][pos.y()] == ter_t_water_sh ) {
                    add_msg( m_warning, _( "Water quickly floods up to your knees." ) );
                    get_memorial().add(
                        pgettext( "memorial_male", "Water level reached knees." ),
                        pgettext( "memorial_female", "Water level reached knees." ) );
                } else {
                    // Must be deep water!
                    add_msg( m_warning, _( "Water fills nearly to the ceiling!" ) );
                    get_memorial().add(
                        pgettext( "memorial_male", "Water level reached the ceiling." ),
                        pgettext( "memorial_female", "Water level reached the ceiling." ) );
                    avatar_action::swim( here, player_character, pos );
                }
            }
            // flood_buf is filled with correct tiles; now copy them back to here
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                here.ter_set( p, flood_buf[p.x()][p.y()] );
            }
            get_timed_events().add( timed_event_type::TEMPLE_FLOOD,
                                    calendar::turn + rng( 2_turns, 3_turns ) );
        }
        break;

        case timed_event_type::TEMPLE_SPAWN: {
            static const std::array<mtype_id, 4> temple_monsters = { {
                    mon_sewer_snake, mon_dermatik, mon_spider_widow_giant, mon_spider_cellar_giant
                }
            };
            const mtype_id &montype = random_entry( temple_monsters );
            g->place_critter_around( montype, pos, 2 );
        }
        break;

        case timed_event_type::MORTAR_FIRE_MESSAGE:
            if( string_id.empty() ) {
                add_msg( m_info, _( "Over the radio, you hear, \"Shot out.\"" ) );
            } else {
                add_msg( m_info, _( "Over the radio, %s reports, \"Shot out.\"" ), string_id );
            }
            break;

        case timed_event_type::MORTAR_IMPACT_MESSAGE: {
            const std::optional<mortar_impact_key> report = parse_mortar_impact_key( key );
            if( !report ) {
                debugmsg( "Mortar impact message missing target key: %s", key );
                break;
            }

            const int dx = map_square.x() - report->target.x();
            const int dy = map_square.y() - report->target.y();
            const std::optional<int> drone_pilot_skill = report->drone_pilot_skill;
            int miss_quantization = 10;
            if( drone_pilot_skill ) {
                if( *drone_pilot_skill >= 8 ) {
                    miss_quantization = 5;
                } else if( *drone_pilot_skill < 4 ) {
                    miss_quantization = 20;
                }
            }
            const int miss_distance = round_to_nearest( std::hypot( dx, dy ), miss_quantization );
            const bool in_bubble = here.inbounds( map_square );
            const int player_distance = rl_dist( player_character.pos_abs(), map_square );
            const std::string cue = !in_bubble ? _( "heard in the far distance" ) :
                                    player_distance > MAX_VIEW_DISTANCE ? _( "heard in the distance" ) :
                                    _( "observed" );
            const std::string recipient = string_id.empty() ? _( "the mortar team" ) : string_id;

            if( miss_distance == 0 ) {
                if( drone_pilot_skill ) {
                    add_msg( m_info, _( "Over the radio, drone spotter relays to %1$s: \"Splash %2$s, on target.\"" ),
                             recipient, cue );
                } else {
                    add_msg( m_info, _( "You radio back to %1$s: \"Splash %2$s, on target.\"" ),
                             recipient, cue );
                }
            } else {
                const std::string miss_direction = direction_name( direction_from( point::zero,
                                                   point( dx, dy ) ) );
                if( drone_pilot_skill ) {
                    if( *drone_pilot_skill >= 8 ) {
                        const std::string correction_direction = direction_name( direction_from( point::zero,
                                                               point( -dx, -dy ) ) );
                        add_msg( m_info,
                                 _( "Over the radio, drone spotter relays to %1$s: \"Splash %2$s, %3$d meters %4$s of target; correct %3$d meters %5$s.\"" ),
                                 recipient, cue, miss_distance, miss_direction, correction_direction );
                    } else if( *drone_pilot_skill >= 4 ) {
                        add_msg( m_info,
                                 _( "Over the radio, drone spotter relays to %1$s: \"Splash %2$s, about %3$d meters %4$s of target.\"" ),
                                 recipient, cue, miss_distance, miss_direction );
                    } else {
                        add_msg( m_info,
                                 _( "Over the radio, drone spotter relays to %1$s: \"Splash %2$s, target miss %3$s.\"" ),
                                 recipient, cue, miss_direction );
                    }
                } else {
                    add_msg( m_info,
                             _( "You radio back to %1$s: \"Splash %2$s, about %3$d meters %4$s of target.\"" ),
                             recipient, cue, miss_distance, miss_direction );
                }
            }
        }
        break;

        case timed_event_type::MORTAR_FIELD: {
            int radius = 0;
            int age_seconds = 0;
            std::sscanf( key.c_str(), "%d,%d", &radius, &age_seconds );
            radius = std::max( 0, radius );
            const time_duration age = age_seconds == 0 ? 0_turns :
                                      -time_duration::from_seconds( age_seconds );
            const field_type_str_id field_type( string_id );
            if( string_id.empty() || !field_type.is_valid() ) {
                debugmsg( "Mortar field event has invalid field type: %s", string_id );
                break;
            }

            if( here.inbounds( map_square ) ) {
                apply_mortar_field( here, map_square, field_type, strength, radius, age );
            } else {
                map tm;
                tm.load( project_to<coords::sm>( map_square - point{ radius, radius } ), false );
                apply_mortar_field( tm, map_square, field_type, strength, radius, age );
                tm.save();
            }
        }
        break;

        case timed_event_type::FPV_DRONE_ARRIVAL_MESSAGE:
            if( string_id.empty() ) {
                add_msg( m_info, _( "Over the radio, you hear, \"Drone is on station.  Time on station: %s.\"" ),
                         format_fpv_duration( strength ) );
            } else {
                add_msg( m_info,
                         _( "Over the radio, %1$s reports, \"Drone is on station.  Time on station: %2$s.\"" ),
                         string_id, format_fpv_duration( strength ) );
            }
            break;

        case timed_event_type::FPV_DRONE_STATUS_MESSAGE:
            if( string_id.empty() ) {
                add_msg( m_info, _( "Over the radio, you hear, \"Drone station time remaining: %s.\"" ),
                         format_fpv_duration( strength ) );
            } else {
                add_msg( m_info,
                         _( "Over the radio, %1$s reports, \"Drone station time remaining: %2$s.\"" ),
                         string_id, format_fpv_duration( strength ) );
            }
            break;

        case timed_event_type::FPV_DRONE_RETURN_MESSAGE:
            if( string_id.empty() ) {
                add_msg( m_info,
                         _( "Over the radio, you hear, \"Drone is bingo battery and returning.  Recovery ETA: %s.\"" ),
                         format_fpv_duration( strength ) );
            } else {
                add_msg( m_info,
                         _( "Over the radio, %1$s reports, \"Drone is bingo battery and returning.  Recovery ETA: %2$s.\"" ),
                         string_id, format_fpv_duration( strength ) );
            }
            break;

        case timed_event_type::FPV_DRONE_RECOVERED_MESSAGE:
            if( string_id.empty() ) {
                add_msg( m_info, _( "Over the radio, you hear, \"Drone recovered.\"" ) );
            } else {
                add_msg( m_info, _( "Over the radio, %s reports, \"Drone recovered.\"" ), string_id );
            }
            break;

        case timed_event_type::FPV_DRONE_LOST_MESSAGE:
            if( string_id.empty() ) {
                add_msg( m_info,
                         _( "Over the radio, you hear, \"Drone battery exhausted.  Airframe lost.\"" ) );
            } else {
                add_msg( m_info,
                         _( "Over the radio, %s reports, \"Drone battery exhausted.  Airframe lost.\"" ),
                         string_id );
            }
            break;

        case timed_event_type::FPV_DRONE_IMPACT_MESSAGE: {
            const bool in_bubble = here.inbounds( map_square );
            const int player_distance = rl_dist( player_character.pos_abs(), map_square );
            const std::string cue = !in_bubble ? _( "in the far distance" ) :
                                    player_distance > MAX_VIEW_DISTANCE ? _( "in the distance" ) :
                                    _( "nearby" );
            if( string_id.empty() ) {
                add_msg( m_info, _( "The FPV drone detonates %s." ), cue );
            } else {
                add_msg( m_info, _( "%1$s radios, \"FPV impact.\"  The drone detonates %2$s." ),
                         string_id, cue );
            }
        }
        break;

        case timed_event_type::FPV_DRONE_PAYLOAD_DROP: {
            const bool in_bubble = here.inbounds( map_square );
            const int player_distance = rl_dist( player_character.pos_abs(), map_square );
            const std::string cue = !in_bubble ? _( "in the far distance" ) :
                                    player_distance > MAX_VIEW_DISTANCE ? _( "in the distance" ) :
                                    _( "nearby" );
            const fpv_payload_drop_data payload_data = parse_fpv_payload_drop_string_id( string_id );
            const itype_id payload_id( payload_data.payload_id );
            if( !payload_id.is_valid() ) {
                debugmsg( "FPV payload drop event has invalid payload: %s", payload_data.payload_id );
                break;
            }

            item payload( payload_id, calendar::turn, 1 );
            if( !detonate_fpv_payload_if_explosive( player_character.as_avatar(), here, payload,
                                                    map_square ) ) {
                if( in_bubble ) {
                    place_live_fpv_payload( here, payload_id, map_square );
                } else {
                    map tm;
                    tm.load( project_to<coords::sm>( map_square ), false );
                    place_live_fpv_payload( tm, payload_id, map_square );
                    tm.save();
                }
            }

            const std::string &speaker = payload_data.operator_name.empty() ? key :
                                         payload_data.operator_name;
            if( speaker.empty() ) {
                add_msg( m_info, _( "A drone payload drops %s." ), cue );
            } else {
                add_msg( m_info, _( "%1$s radios, \"Payload away.\"  A drone payload drops %2$s." ),
                         speaker, cue );
            }
        }
        break;

        case timed_event_type::EXPLOSION: {
            apply_timed_explosion( player_character.as_avatar(), here, map_square, expl_data );
        }
        break;

        case timed_event_type::DSA_ALRP_SUMMON: {
            const tripoint_abs_sm u_pos = player_character.pos_abs_sm();
            if( rl_dist( u_pos, map_point ) <= 4 ) {
                const tripoint_bub_ms spot = here.get_bub( project_to<coords::ms>( map_point ) );
                monster dispatcher( mon_dsa_alien_dispatch );
                fake_spell summoning( spell_dks_summon_alrp, true, 12 );
                summoning.get_spell( player_character ).cast_all_effects( dispatcher, spot );
            } else {
                const tripoint_abs_omt omt_point = project_to<coords::omt>( map_point );
                tinymap mx_map;
                mx_map.load( omt_point, false );
                MapExtras::apply_function( map_extra_mx_dsa_alrp, mx_map, omt_point );
                g->load_npcs();
                here.invalidate_map_cache( map_point.z() );
            }
        }
        break;

        case timed_event_type::TRANSFORM_RADIUS: {
            map tm;
            tm.load( project_to<coords::sm>( map_square - point{ strength, strength} ), false );
            tm.transform_radius( ter_furn_transform_id( string_id ), strength,
                                 map_square );
            break;
        }
        case timed_event_type::UPDATE_MAPGEN:
            run_mapgen_update_func(
                update_mapgen_id( string_id ), project_to<coords::omt>( map_point ), {}, nullptr );
            set_queued_points();
            reality_bubble().invalidate_map_cache( map_point.z() );
            break;

        case timed_event_type::REVERT_SUBMAP: {
            submap *sm = MAPBUFFER.lookup_submap( map_point );
            sm->revert_submap( revert );
            reality_bubble().invalidate_map_cache( map_point.z() );
            break;
        }

        default:
            // Nothing happens for other events
            break;
    }
}

void timed_event::per_turn()
{
    Character &player_character = get_player_character();
    map &here = get_map();
    switch( type ) {
        case timed_event_type::SPAWN_WYRMS:
            if( here.get_abs_sub().z() >= 0 ) {
                when -= 1_turns;
                return;
            }
            if( calendar::once_every( time_duration::from_seconds( rng( 2, 3 ) ) ) &&
                !player_character.is_deaf() ) {
                add_msg( m_warning, _( "You hear screeches from the rock above and around you!" ) );
            }
            break;

        case timed_event_type::AMIGARA:
            if( calendar::once_every( time_duration::from_seconds( rng( 2, 3 ) ) ) ) {
                add_msg( m_warning, _( "The entire cavern shakes!" ) );
            }
            break;

        case timed_event_type::AMIGARA_WHISPERS: {
            bool faults = false;
            for( const tripoint_bub_ms &p : here.points_on_zlevel() ) {
                if( here.ter( p ) == ter_t_fault ) {
                    faults = true;
                    break;
                }
            }

            if( calendar::once_every( time_duration::from_seconds( 10 ) ) && faults ) {
                add_msg( m_info, _( "You hear someone whispering \"%s\"" ),
                         SNIPPET.random_from_category( "amigara_whispers" ).value_or( translation() ) );
            }
        }
        break;

        case timed_event_type::TEMPLE_OPEN:
            if( calendar::once_every( time_duration::from_seconds( rng( 2, 3 ) ) ) ) {
                add_msg( m_warning, _( "The earth rumbles." ) );
            }
            break;

        default:
            // Nothing happens for other events
            break;
    }
}

void timed_event_manager::process()
{
    for( auto it = events.begin(); it != events.end(); ) {
        it->per_turn();
        if( it->when <= calendar::turn ) {
            it->actualize();
            it = events.erase( it );
        } else {
            ++it;
        }
    }
}

void timed_event_manager::add( timed_event_type type, const time_point &when,
                               const int faction_id, int strength, const std::string &key )
{
    add( type, when, faction_id, get_player_character().pos_abs(), strength, "", key );
}

void timed_event_manager::add( timed_event_type type, const time_point &when,
                               const int faction_id,
                               const tripoint_abs_ms &where,
                               int strength, const std::string &key )
{
    events.emplace_back( type, when, faction_id, where, strength, key );
}

void timed_event_manager::add( timed_event_type type, const time_point &when,
                               const int faction_id,
                               const tripoint_abs_ms &where,
                               int strength, const std::string &string_id,
                               const std::string &key )
{
    events.emplace_back( type, when, faction_id, where, strength, string_id, key );
}

void timed_event_manager::add( timed_event_type type, const time_point &when,
                               const tripoint_abs_ms &where, const explosion_data expl_data )
{
    events.emplace_back( type, when, where, expl_data );
}

void timed_event_manager::add( timed_event_type type, const time_point &when,
                               const int faction_id,
                               const tripoint_abs_ms &where,
                               int strength, const std::string &string_id, submap sr,
                               const std::string &key )
{
    events.emplace_back( type, when, faction_id, where, strength, string_id, std::move( sr ), key );
}

bool timed_event_manager::queued( const timed_event_type type ) const
{
    return const_cast<timed_event_manager &>( *this ).get( type ) != nullptr;
}

timed_event *timed_event_manager::get( const timed_event_type type )
{
    for( timed_event &e : events ) {
        if( e.type == type ) {
            return &e;
        }
    }
    return nullptr;
}

timed_event *timed_event_manager::get( const timed_event_type type, const std::string &key )
{
    for( timed_event &e : events ) {
        if( e.type == type && e.key == key ) {
            return &e;
        }
    }
    return nullptr;
}

std::list<timed_event> const &timed_event_manager::get_all() const
{
    return events;
}

void timed_event_manager::remove( const timed_event_type type, const std::string &key )
{
    events.remove_if( [type, &key]( const timed_event & event ) {
        return event.type == type && event.key == key;
    } );
}

void timed_event_manager::set_all( const std::string &key, time_duration time_in_future )
{
    for( timed_event &e : events ) {
        if( e.key == key ) {
            e.when = calendar::turn + time_in_future;
        }
    }
}

bool you_know_where_you_are()
{
    return !get_timed_events().get( timed_event_type::OVERRIDE_PLACE );
}
