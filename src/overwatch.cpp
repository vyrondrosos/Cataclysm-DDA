#include "overwatch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "ammo_effect.h"
#include "avatar.h"
#include "ballistics.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "current_map.h"
#include "creature_tracker.h"
#include "debug.h"
#include "effect.h"
#include "game.h"
#include "flag.h"
#include "gun_mode.h"
#include "item.h"
#include "lightmap.h"
#include "line.h"
#include "map.h"
#include "math_parser_diag_value.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "player_activity.h"
#include "ranged.h"
#include "string_formatter.h"
#include "timed_event.h"
#include "translations.h"
#include "vpart_position.h"

namespace
{

static const activity_id ACT_PROVIDE_OVERWATCH( "ACT_PROVIDE_OVERWATCH" );
static const ammo_effect_str_id ammo_effect_JET( "JET" );
static const ammo_effect_str_id ammo_effect_STREAM( "STREAM" );
static const ammo_effect_str_id ammo_effect_STREAM_BIG( "STREAM_BIG" );
static const ammo_effect_str_id ammo_effect_STREAM_TINY( "STREAM_TINY" );
static const ammotype ammo_chemical_spray( "chemical_spray" );
static const ammotype ammo_flammable( "flammable" );
static const flag_id flag_MOUNTED_GUN( "MOUNTED_GUN" );
static const skill_id skill_launcher( "launcher" );
static const skill_id skill_rifle( "rifle" );

constexpr const char *assignment_key = "overwatch_assignment";
constexpr const char *mode_key = "overwatch_mode";
constexpr const char *post_key = "overwatch_post";
constexpr const char *order_key = "overwatch_order_key";
constexpr const char *sight_lost_key = "overwatch_sight_lost";

static const efftype_id effect_no_sight( "no_sight" );
static const efftype_id effect_narcosis( "narcosis" );
static const flag_id flag_INVISIBLE( "INVISIBLE" );

enum class target_kind : int {
    character,
    monster
};

struct target_reference {
    target_kind kind = target_kind::monster;
    character_id character;
    int monster = -1;
    tripoint_abs_ms pos = tripoint_abs_ms::invalid;
};

bool is_auto_mode_id( const gun_mode_id &id )
{
    const std::string &value = id.str();
    return value == "AUTO" || ( value.size() > 5 && value.compare( value.size() - 5, 5,
                                    "_AUTO" ) == 0 );
}

bool is_spray_mode( const gun_mode &mode )
{
    if( !mode || mode.melee() ) {
        return true;
    }
    const std::set<ammo_effect_str_id> effects = mode->ammo_effects();
    const std::set<ammotype> ammo_types = mode->ammo_types();
    return effects.count( ammo_effect_STREAM ) || effects.count( ammo_effect_STREAM_TINY ) ||
           effects.count( ammo_effect_STREAM_BIG ) || effects.count( ammo_effect_JET ) ||
           ammo_types.count( ammo_chemical_spray ) || ammo_types.count( ammo_flammable ) ||
           mode->typeId() == itype_id( "flamethrower" ) ||
           mode->typeId() == itype_id( "rm451_flamethrower" );
}

bool mounted_weapon_has_support( const npc &gunner, const item &weapon )
{
    if( !weapon.has_flag( flag_MOUNTED_GUN ) ) {
        return true;
    }
    map &reality = get_map();
    std::unique_ptr<map> remote;
    map *here = &reality;
    if( !reality.inbounds( gunner.pos_abs() ) ) {
        remote = std::make_unique<map>();
        const tripoint_abs_sm origin = project_to<coords::sm>( gunner.pos_abs() ) -
                                       point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
        remote->load( origin, false );
        here = remote.get();
    }
    const tripoint_bub_ms pos = gunner.pos_bub( *here );
    if( !here->inbounds( pos ) ) {
        return false;
    }
    const bool vehicle_mount = static_cast<bool>(
                                   here->veh_at( pos ).part_with_feature( "MOUNTABLE", true ) );
    return vehicle_mount || here->has_flag_ter_or_furn( ter_furn_flag::TFLAG_MOUNTABLE, pos );
}

std::optional<target_reference> make_target_reference( Creature &target )
{
    target_reference result;
    result.pos = target.pos_abs();
    if( Character *character = target.as_character() ) {
        result.kind = target_kind::character;
        result.character = character->getID();
        if( result.character.is_valid() ) {
            return result;
        }
        return std::nullopt;
    }
    if( monster *mon = target.as_monster() ) {
        result.kind = target_kind::monster;
        result.monster = get_creature_tracker().temporary_id( *mon );
        return result.monster >= 0 ? std::optional<target_reference>( result ) : std::nullopt;
    }
    return std::nullopt;
}

Creature *resolve_target( const character_id &character, const int monster_id )
{
    if( character.is_valid() ) {
        if( character == get_avatar().getID() ) {
            return &get_avatar();
        }
        return g->find_npc( character );
    }
    if( monster_id >= 0 ) {
        return get_creature_tracker().from_temporary_id( monster_id ).get();
    }
    return nullptr;
}

std::optional<target_reference> stored_target( const npc &gunner )
{
    const diag_value type = gunner.get_value( "overwatch_target_type" );
    const diag_value x = gunner.get_value( "overwatch_target_x" );
    const diag_value y = gunner.get_value( "overwatch_target_y" );
    const diag_value z = gunner.get_value( "overwatch_target_z" );
    if( type.is_empty() || !type.is_str() || !x.is_dbl() || !y.is_dbl() || !z.is_dbl() ) {
        return std::nullopt;
    }
    target_reference result;
    result.pos = tripoint_abs_ms( static_cast<int>( x.dbl() ), static_cast<int>( y.dbl() ),
                                  static_cast<int>( z.dbl() ) );
    if( type.str() == "character" ) {
        const diag_value id = gunner.get_value( "overwatch_target_character_id" );
        if( !id.is_dbl() ) {
            return std::nullopt;
        }
        result.kind = target_kind::character;
        result.character = character_id( static_cast<int>( id.dbl() ) );
        if( result.character.is_valid() ) {
            return result;
        }
        return std::nullopt;
    }
    if( type.str() == "monster" ) {
        const diag_value id = gunner.get_value( "overwatch_target_monster_id" );
        if( !id.is_dbl() || id.dbl() < 0 ) {
            return std::nullopt;
        }
        result.kind = target_kind::monster;
        result.monster = static_cast<int>( id.dbl() );
        return result;
    }
    return std::nullopt;
}

void store_target( npc &gunner, const target_reference &target )
{
    gunner.set_value( "overwatch_target_type",
                      target.kind == target_kind::character ? "character" : "monster" );
    gunner.set_value( "overwatch_target_character_id", target.character.get_value() );
    gunner.set_value( "overwatch_target_monster_id", target.monster );
    gunner.set_value( "overwatch_target_x", target.pos.x() );
    gunner.set_value( "overwatch_target_y", target.pos.y() );
    gunner.set_value( "overwatch_target_z", target.pos.z() );
}

void clear_order_values( npc &gunner )
{
    for( const char *key : {
             "overwatch_order_key", "overwatch_repeat", "overwatch_target_type",
             "overwatch_target_character_id", "overwatch_target_monster_id",
             "overwatch_target_x", "overwatch_target_y", "overwatch_target_z",
             "overwatch_ready_turn", "overwatch_sight_lost"
         } ) {
        gunner.remove_value( key );
    }
}

bool target_is_hostile( const Creature &target )
{
    const avatar &you = get_avatar();
    return target.attitude_to( you ) == Creature::Attitude::HOSTILE ||
           you.attitude_to( target ) == Creature::Attitude::HOSTILE;
}

std::unique_ptr<map> load_map_centered_on( const tripoint_abs_ms &pos )
{
    std::unique_ptr<map> result = std::make_unique<map>();
    const tripoint_abs_sm origin = project_to<coords::sm>( pos ) -
                                   point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE };
    result->load( origin, false );
    result->build_map_cache( pos.z() );
    return result;
}

map &map_containing( const tripoint_abs_ms &pos, std::unique_ptr<map> &remote )
{
    map &reality = get_map();
    if( reality.inbounds( pos ) ) {
        return reality;
    }
    if( !remote || !remote->inbounds( pos ) ) {
        remote = load_map_centered_on( pos );
    }
    return *remote;
}

Target_attributes target_attributes( const npc &gunner, const Creature &target )
{
    std::unique_ptr<map> remote;
    map &here = map_containing( target.pos_abs(), remote );
    const tripoint_bub_ms target_bub = target.pos_bub( here );
    here.build_map_cache( target_bub.z() );
    const float light = here.ambient_light_at( target_bub );
    return Target_attributes( rl_dist( gunner.pos_abs(), target.pos_abs() ),
                              target.ranged_target_size(), light, true );
}

int maximum_aim_moves( npc &gunner, const gun_mode &mode, const Creature &target )
{
    map &reality = get_map();
    std::unique_ptr<map> remote;
    map *source_map = &reality;
    if( !reality.inbounds( gunner.pos_abs() ) ) {
        remote = load_map_centered_on( gunner.pos_abs() );
        source_map = remote.get();
    }
    double starting_recoil;
    if( remote ) {
        swap_map swapped( *source_map );
        starting_recoil = gunner.recoil_total();
    } else {
        starting_recoil = gunner.recoil_total();
    }
    const int aim_limit = gunner.most_accurate_aiming_method_limit( *mode );
    return gunner.gun_engagement_moves( *source_map, *mode, aim_limit,
                                        static_cast<int>( starting_recoil ),
                                        target_attributes( gunner, target ) );
}

time_duration action_duration( const npc &gunner, const int moves )
{
    const int turns = ( std::max( 0, moves ) + std::max( 1, gunner.get_speed() ) - 1 ) /
                      std::max( 1, gunner.get_speed() );
    return time_duration::from_turns( std::max( 1, turns ) );
}

void apply_maximum_aim( npc &gunner, const item &weapon,
                        const Target_attributes &attributes, const int aim_moves )
{
    const int held_moves = gunner.get_moves();
    gunner.set_moves( aim_moves );
    std::unique_ptr<map> remote;
    map &source_map = map_containing( gunner.pos_abs(), remote );
    const aim_mods_cache aim_cache = gunner.gen_aim_mods_cache( source_map, weapon );
    while( gunner.recoil > 0.0 && gunner.get_moves() > 0 ) {
        const double improvement = gunner.aim_per_move( weapon, gunner.recoil, attributes,
                                   aim_cache );
        if( improvement <= MIN_RECOIL_IMPROVEMENT ) {
            break;
        }
        gunner.mod_moves( -1 );
        gunner.recoil = std::max( 0.0, gunner.recoil - improvement );
    }
    gunner.set_moves( held_moves );
}

std::optional<std::pair<gun_mode_id, gun_mode>> selected_mode( npc &gunner )
{
    const item_location wielded = gunner.get_wielded_item();
    const diag_value selected = gunner.get_value( mode_key );
    if( !wielded || selected.is_empty() || !selected.is_str() ) {
        return std::nullopt;
    }
    const gun_mode_id id( selected.str() );
    const std::map<gun_mode_id, gun_mode> modes = wielded->gun_all_modes();
    const auto found = modes.find( id );
    if( found == modes.end() ) {
        return std::nullopt;
    }
    return std::make_pair( id, found->second );
}

std::optional<item_location> mode_target_location( npc &gunner, const gun_mode &mode )
{
    item_location root = gunner.get_wielded_item();
    if( !root || !mode || mode.target == nullptr ) {
        return std::nullopt;
    }
    if( mode.target == root.get_item() ) {
        return root;
    }
    for( item *mod : root->gunmods() ) {
        if( mod == mode.target ) {
            return item_location( root, mod );
        }
    }
    return std::nullopt;
}

void schedule_fire( npc &gunner, Creature &target, const target_reference &target_ref,
                    const gun_mode_id &mode_id, const bool repeat, const std::string &key,
                    const int preceding_action_moves = 0 )
{
    const std::optional<std::pair<gun_mode_id, gun_mode>> mode = selected_mode( gunner );
    if( !mode || mode->first != mode_id ) {
        return;
    }
    const int aim_moves = maximum_aim_moves( gunner, mode->second, target );
    const time_point ready = calendar::turn +
                             action_duration( gunner, preceding_action_moves + aim_moves );
    overwatch_fire_event_data data;
    data.gunner_id = gunner.getID();
    data.target_character = target_ref.character;
    data.target_monster = target_ref.monster;
    data.target_pos = target.pos_abs();
    data.mode_id = mode_id.str();
    data.order_key = key;
    data.aim_moves = aim_moves;
    data.repeat = repeat;
    get_timed_events().add_overwatch_fire( ready, data.target_pos, gunner.disp_name(), key,
            data );
    gunner.set_value( "overwatch_ready_turn",
                      to_turns<int>( ready - calendar::turn_zero ) );
}

void schedule_sight_check( npc &gunner, Creature &target, const target_reference &target_ref,
                           const gun_mode_id &mode_id, const std::string &key )
{
    overwatch_fire_event_data data;
    data.gunner_id = gunner.getID();
    data.target_character = target_ref.character;
    data.target_monster = target_ref.monster;
    data.target_pos = target.pos_abs();
    data.mode_id = mode_id.str();
    data.order_key = key;
    data.repeat = true;
    const time_point ready = calendar::turn + 1_turns;
    get_timed_events().add_overwatch_fire( ready, data.target_pos, gunner.disp_name(), key, data );
    gunner.set_value( "overwatch_ready_turn",
                      to_turns<int>( ready - calendar::turn_zero ) );
}

void report_lost_sight( npc &gunner, const Creature &target )
{
    if( gunner.get_value( sight_lost_key ).is_str() ) {
        return;
    }
    gunner.set_value( sight_lost_key, "yes" );
    add_msg( _( "%1$s reports: \"I have lost sight of %2$s; holding fire.\"" ),
             gunner.disp_name(), target.disp_name() );
}

void report_regained_sight( npc &gunner, const Creature &target )
{
    if( !gunner.get_value( sight_lost_key ).is_str() ) {
        return;
    }
    gunner.remove_value( sight_lost_key );
    add_msg( _( "%1$s reports: \"I have %2$s in sight again.\"" ),
             gunner.disp_name(), target.disp_name() );
}

void report_order_abort( const npc &gunner, const std::string &reason )
{
    if( overwatch::operator_available( gunner ) ) {
        add_msg( _( "%1$s reports: \"Aborting overwatch order; %2$s.\"" ),
                 gunner.disp_name(), reason );
    }
}

std::optional<int> reload_moves_for_mode( npc &gunner, const gun_mode &mode )
{
    const auto select = [&gunner, &mode]() -> std::optional<int> {
        const std::optional<item_location> target = mode_target_location( gunner, mode );
        if( !target ) {
            return std::nullopt;
        }
        const item::reload_option option = gunner.select_ammo( *target );
        return option ? std::optional<int>( option.moves() ) : std::nullopt;
    };
    map &reality = get_map();
    if( reality.inbounds( gunner.pos_abs() ) ) {
        return select();
    }
    std::unique_ptr<map> source_map = load_map_centered_on( gunner.pos_abs() );
    swap_map swapped( *source_map );
    return select();
}

bool perform_reload( npc &gunner, const gun_mode_id &mode_id )
{
    item_location wielded = gunner.get_wielded_item();
    if( !wielded || !wielded->gun_set_mode( mode_id ) ) {
        return false;
    }
    gun_mode mode = wielded->gun_get_mode( mode_id );
    if( !mode ) {
        return false;
    }
    const auto reload = [&gunner, &mode]() {
        const std::optional<item_location> target = mode_target_location( gunner, mode );
        if( !target ) {
            return false;
        }
        const int held_moves = gunner.get_moves();
        gunner.do_reload( *target );
        // The timed event has already charged the reload duration in calendar time.
        gunner.set_moves( held_moves );
        return true;
    };
    map &reality = get_map();
    if( reality.inbounds( gunner.pos_abs() ) ) {
        if( !reload() ) {
            return false;
        }
    } else {
        std::unique_ptr<map> source_map = load_map_centered_on( gunner.pos_abs() );
        bool reloaded = false;
        {
            swap_map swapped( *source_map );
            reloaded = reload();
        }
        if( !reloaded ) {
            return false;
        }
        // Standard NPC reload selection includes nearby map items.  Persist
        // their removal (and any magazine dropped at the post) off-bubble.
        source_map->save();
    }
    wielded = gunner.get_wielded_item();
    if( !wielded ) {
        return false;
    }
    mode = wielded->gun_get_mode( mode_id );
    return mode && mode->ammo_sufficient( &gunner );
}

bool schedule_reload( npc &gunner, const gun_mode_id &mode_id,
                      const std::optional<target_reference> &target, const bool repeat,
                      const bool continue_fire, const std::string &key, std::string *failure,
                      const int preceding_action_moves = 0 )
{
    const std::optional<std::pair<gun_mode_id, gun_mode>> mode = selected_mode( gunner );
    if( !mode || mode->first != mode_id ) {
        if( failure != nullptr ) {
            *failure = _( "The selected overwatch weapon mode is no longer available." );
        }
        return false;
    }
    const std::optional<int> moves = reload_moves_for_mode( gunner, mode->second );
    if( !moves ) {
        if( failure != nullptr ) {
            *failure = string_format(
                           _( "%s cannot reload that weapon; it is already full or has no compatible "
                              "ammunition or magazine at the post." ), gunner.disp_name() );
        }
        return false;
    }
    const time_point ready = calendar::turn +
                             action_duration( gunner, preceding_action_moves + *moves );
    overwatch_fire_event_data data;
    data.gunner_id = gunner.getID();
    data.target_pos = target ? target->pos : gunner.pos_abs();
    data.target_character = target ? target->character : character_id();
    data.target_monster = target ? target->monster : -1;
    data.mode_id = mode_id.str();
    data.order_key = key;
    data.reload_moves = *moves;
    data.continue_fire = continue_fire;
    data.repeat = repeat;
    get_timed_events().add_overwatch_reload( ready, data.target_pos, gunner.disp_name(), key,
            data );
    gunner.set_value( "overwatch_ready_turn",
                      to_turns<int>( ready - calendar::turn_zero ) );
    return true;
}

void prepare_activity_npc( npc &gunner )
{
    g->add_npc_follower( gunner.getID() );
    gunner.chatbin.first_topic = gunner.chatbin.talk_friend;
    gunner.guard_pos = std::nullopt;
    gunner.clear_ai_guard_pos();
    gunner.goal = npc::no_goal_point;
    gunner.omt_path.clear();
    gunner.path.clear();
    gunner.chair_pos = std::nullopt;
    gunner.wander_pos = std::nullopt;
    gunner.clear_destination();
    gunner.clear_committed_goal();
}

} // namespace

namespace overwatch
{

bool is_assigned( const npc &gunner )
{
    const diag_value assignment = gunner.get_value( assignment_key );
    const diag_value post = gunner.get_value( post_key );
    return gunner.activity.id() == ACT_PROVIDE_OVERWATCH && !assignment.is_empty() &&
           assignment.str() == "yes" && post.is_tripoint() && post.tripoint() == gunner.pos_abs();
}

bool operator_available( const npc &gunner )
{
    return !gunner.is_dead() && !gunner.in_sleep_state() && !gunner.has_effect( effect_narcosis );
}

std::vector<firing_mode> eligible_modes( const npc &gunner )
{
    std::vector<firing_mode> result;
    const item_location wielded = gunner.get_wielded_item();
    if( !wielded || !wielded->is_gun() || wielded->is_gunmod() ) {
        return result;
    }

    for( const std::pair<const gun_mode_id, gun_mode> &entry : wielded->gun_all_modes() ) {
        const gun_mode &mode = entry.second;
        if( !mode || mode.melee() || mode.qty < 1 || is_spray_mode( mode ) ) {
            continue;
        }
        const skill_id skill = mode->gun_skill();
        if( skill != skill_rifle && skill != skill_launcher ) {
            continue;
        }
        if( !gunner.meets_requirements( *mode, *wielded ) ||
            !mounted_weapon_has_support( gunner, *mode ) ) {
            continue;
        }
        result.push_back( firing_mode{ entry.first, mode.tname(), mode.qty,
                                      mode.qty > 1 && is_auto_mode_id( entry.first ) } );
    }
    return result;
}

bool mode_is_eligible( const npc &gunner, const gun_mode_id &mode_id, std::string *failure )
{
    const std::vector<firing_mode> modes = eligible_modes( gunner );
    if( std::any_of( modes.begin(), modes.end(), [&mode_id]( const firing_mode & entry ) {
        return entry.id == mode_id;
    } ) ) {
        return true;
    }
    if( failure != nullptr ) {
        *failure = _( "The selected rifle or launcher mode is no longer usable." );
    }
    return false;
}

bool assign( npc &gunner, std::string *failure )
{
    if( !operator_available( gunner ) ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s is unable to provide overwatch right now." ),
                                      gunner.disp_name() );
        }
        return false;
    }
    if( !gunner.is_player_ally() || gunner.is_hallucination() ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s is not willing to provide overwatch." ),
                                      gunner.disp_name() );
        }
        return false;
    }
    const std::vector<firing_mode> modes = eligible_modes( gunner );
    if( modes.empty() ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s needs a usable rifle or launcher in hand." ),
                                      gunner.disp_name() );
        }
        return false;
    }

    gunner.clear_mortar_support( true );
    gunner.clear_fpv_support( true );
    gunner.clear_overwatch_support();
    if( gunner.has_player_activity() ) {
        gunner.revert_after_activity();
    }

    const tripoint_abs_ms post = gunner.pos_abs();
    gunner.set_value( assignment_key, "yes" );
    gunner.set_value( post_key, post );
    const auto preferred =
        std::find_if( modes.begin(), modes.end(), []( const firing_mode &entry ) {
            return entry.shots == 1;
        } );
    gunner.set_value( mode_key,
                      ( preferred != modes.end() ? preferred : modes.begin() )->id.str() );
    gunner.assign_activity( provide_overwatch_activity_actor( post ) );
    prepare_activity_npc( gunner );
    return true;
}

bool select_fire_mode( npc &gunner, const bool automatic, std::string *failure )
{
    const std::vector<firing_mode> modes = eligible_modes( gunner );
    const auto selected =
        std::find_if( modes.begin(), modes.end(), [automatic]( const firing_mode &mode ) {
            return automatic ? mode.automatic : mode.shots == 1;
        } );
    if( selected == modes.end() ) {
        if( failure != nullptr ) {
            *failure = automatic ? _( "That weapon has no true fully automatic mode." ) :
                       _( "That weapon has no single-shot mode." );
        }
        return false;
    }
    return select_fire_mode( gunner, selected->id, failure );
}

bool select_fire_mode( npc &gunner, const gun_mode_id &mode_id, std::string *failure )
{
    if( !operator_available( gunner ) ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s is unable to operate overwatch right now." ),
                                      gunner.disp_name() );
        }
        return false;
    }
    if( !is_assigned( gunner ) ) {
        if( failure != nullptr ) {
            *failure = _( "That follower is no longer holding an overwatch position." );
        }
        return false;
    }
    if( !mode_is_eligible( gunner, mode_id, failure ) ) {
        return false;
    }
    cancel_order( gunner, false );
    gunner.set_value( mode_key, mode_id.str() );
    return true;
}

bool observer_can_see( const npc &gunner, const Creature &target )
{
    const int distance = rl_dist( gunner.pos_abs(), target.pos_abs() );
    if( target.is_dead_state() || target.is_hallucination() || gunner.is_hallucination() ||
        distance > max_range ||
        gunner.pos_abs().z() != target.pos_abs().z() ) {
        return false;
    }

    map &reality = get_map();
    if( distance <= MAX_VIEW_DISTANCE ) {
        if( reality.inbounds( gunner.pos_abs() ) && reality.inbounds( target.pos_abs() ) ) {
            reality.build_map_cache( gunner.pos_abs().z() );
            return gunner.sees( reality, target );
        }
        std::unique_ptr<map> local = load_map_centered_on( gunner.pos_abs() );
        if( !local->inbounds( target.pos_abs() ) ) {
            return false;
        }
        swap_map swapped( *local );
        return gunner.sees( *local, target );
    }

    if( gunner.is_blind() || gunner.has_effect( effect_no_sight ) ||
        target.has_effect_with_flag( flag_INVISIBLE ) || target.has_flag( flag_INVISIBLE ) ||
        target.has_flag( mon_flag_CAMOUFLAGE ) || target.has_flag( mon_flag_WATER_CAMOUFLAGE ) ||
        target.has_flag( mon_flag_SMALL_HIDER ) ) {
        return false;
    }

    const item_location weapon = gunner.get_wielded_item();
    bool has_scope = weapon && weapon->has_flag( flag_ZOOM );
    if( weapon ) {
        for( const item *mod : weapon->gunmods() ) {
            has_scope = has_scope || mod->has_flag( flag_ZOOM );
        }
    }
    std::unique_ptr<map> remote;
    map &source_map = map_containing( gunner.pos_abs(), remote );
    const tripoint_bub_ms source_bub = source_map.get_bub( gunner.pos_abs() );
    source_map.build_map_cache( source_bub.z() );
    const float observer_light = source_map.ambient_light_at( source_bub );

    std::unique_ptr<map> target_remote;
    map &target_map = map_containing( target.pos_abs(), target_remote );
    const tripoint_bub_ms target_bub = target_map.get_bub( target.pos_abs() );
    target_map.build_map_cache( target_bub.z() );
    if( target_map.has_flag_ter_or_furn( ter_furn_flag::TFLAG_HIDE_PLACE, target_bub ) ||
        target_map.has_flag_ter_or_furn( ter_furn_flag::TFLAG_SMALL_HIDE, target_bub ) ||
        target.is_likely_underwater( target_map ) ) {
        return false;
    }
    const float target_light = target_map.ambient_light_at( target_bub );
    if( target_light <= 0.0f ) {
        return false;
    }

    double extra_attenuation = 0.0;
    const std::vector<tripoint> trajectory =
        line_to( gunner.pos_abs().raw(), target.pos_abs().raw() );
    std::unique_ptr<map> segment_map;
    for( size_t index = 0; index + 1 < trajectory.size(); ++index ) {
        const tripoint_abs_ms point( trajectory[index] );
        if( point == gunner.pos_abs() ) {
            continue;
        }
        map &segment = map_containing( point, segment_map );
        const tripoint_bub_ms bub = segment.get_bub( point );
        segment.build_map_cache( bub.z() );
        const float transparency = segment.light_transparency( bub );
        if( transparency <= LIGHT_TRANSPARENCY_SOLID ) {
            return false;
        }
        extra_attenuation += std::max( 0.0f, transparency - LIGHT_TRANSPARENCY_OPEN_AIR );
    }
    // ZOOM optics make the full four-bubble support envelope usable in clear daylight.
    // Keep smoke, fields, and other non-air opacity undiscounted.
    const double clean_air_attenuation = distance /
                                         ( has_scope ? 4.0 : 1.0 ) * LIGHT_TRANSPARENCY_OPEN_AIR;
    const double apparent_light = target_light *
                                  std::exp( -( clean_air_attenuation + extra_attenuation ) );
    return apparent_light >= gunner.get_vision_threshold( observer_light );
}

bool issue_order( npc &gunner, Creature &target, const bool repeat, std::string *failure )
{
    if( !operator_available( gunner ) ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s is unable to operate overwatch right now." ),
                                      gunner.disp_name() );
        }
        return false;
    }
    if( !is_assigned( gunner ) ) {
        if( failure != nullptr ) {
            *failure = _( "That follower is no longer holding an overwatch position." );
        }
        return false;
    }
    std::optional<std::pair<gun_mode_id, gun_mode>> mode = selected_mode( gunner );
    if( !mode || !mode_is_eligible( gunner, mode->first, failure ) ) {
        return false;
    }
    if( target.is_dead_state() || target.is_hallucination() || !target_is_hostile( target ) ) {
        if( failure != nullptr ) {
            *failure = _( "Select a living hostile creature." );
        }
        return false;
    }
    const int distance = rl_dist( gunner.pos_abs(), target.pos_abs() );
    if( distance > max_range ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "That target is beyond the overwatch limit of %d tiles." ),
                                      max_range );
        }
        return false;
    }
    if( !observer_can_see( gunner, target ) ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s cannot see that target from the overwatch post." ),
                                      gunner.disp_name() );
        }
        return false;
    }
    const std::optional<target_reference> target_ref = make_target_reference( target );
    if( !target_ref ) {
        if( failure != nullptr ) {
            *failure = _( "That target cannot be tracked for an overwatch order." );
        }
        return false;
    }

    cancel_order( gunner, false );
    const int now = to_turns<int>( calendar::turn - calendar::turn_zero );
    const std::string key = string_format( "overwatch_%d_%d", gunner.getID().get_value(), now );
    gunner.set_value( order_key, key );
    gunner.set_value( "overwatch_repeat", repeat ? "yes" : "no" );
    store_target( gunner, *target_ref );
    if( mode->second->ammo_sufficient( &gunner ) ) {
        schedule_fire( gunner, target, *target_ref, mode->first, repeat, key );
    } else if( !schedule_reload( gunner, mode->first, target_ref, repeat, true, key, failure ) ) {
        clear_order_values( gunner );
        return false;
    }
    return true;
}

bool issue_reload( npc &gunner, std::string *failure )
{
    if( !operator_available( gunner ) ) {
        if( failure != nullptr ) {
            *failure = string_format( _( "%s is unable to operate overwatch right now." ),
                                      gunner.disp_name() );
        }
        return false;
    }
    if( !is_assigned( gunner ) ) {
        if( failure != nullptr ) {
            *failure = _( "That follower is no longer holding an overwatch position." );
        }
        return false;
    }
    const std::optional<std::pair<gun_mode_id, gun_mode>> mode = selected_mode( gunner );
    if( !mode || !mode_is_eligible( gunner, mode->first, failure ) ) {
        return false;
    }
    if( !reload_moves_for_mode( gunner, mode->second ) ) {
        if( failure != nullptr ) {
            *failure = string_format(
                           _( "%s cannot reload that weapon; it is already full or has no compatible "
                              "ammunition or magazine at the post." ), gunner.disp_name() );
        }
        return false;
    }
    cancel_order( gunner, false );
    const int now = to_turns<int>( calendar::turn - calendar::turn_zero );
    const std::string key = string_format( "overwatch_reload_%d_%d",
                                          gunner.getID().get_value(), now );
    gunner.set_value( order_key, key );
    if( !schedule_reload( gunner, mode->first, std::nullopt, false, false, key, failure ) ) {
        clear_order_values( gunner );
        return false;
    }
    return true;
}

void cancel_order( npc &gunner, const bool notify )
{
    if( notify && !operator_available( gunner ) ) {
        add_msg( _( "%s is unable to operate overwatch right now." ), gunner.disp_name() );
        return;
    }
    const diag_value key = gunner.get_value( order_key );
    const bool had_order = !key.is_empty() && key.is_str();
    if( had_order ) {
        get_timed_events().remove( timed_event_type::OVERWATCH_FIRE, key.str() );
        get_timed_events().remove( timed_event_type::OVERWATCH_RELOAD, key.str() );
    }
    clear_order_values( gunner );
    if( notify && had_order ) {
        add_msg( _( "%s acknowledges the cease-fire order." ), gunner.disp_name() );
    }
}

void stand_down( npc &gunner, const bool notify )
{
    if( notify && !operator_available( gunner ) ) {
        add_msg( _( "%s is unable to operate overwatch right now." ), gunner.disp_name() );
        return;
    }
    if( gunner.activity.id() == ACT_PROVIDE_OVERWATCH ) {
        if( !notify ) {
            gunner.clear_overwatch_support( false );
        }
        gunner.revert_after_activity();
    } else {
        gunner.clear_overwatch_support( notify );
    }
}

std::string status( const npc &gunner )
{
    if( !is_assigned( gunner ) ) {
        return string_format( _( "%s is not assigned to overwatch." ), gunner.disp_name() );
    }
    const item_location weapon = gunner.get_wielded_item();
    const diag_value selected = gunner.get_value( mode_key );
    std::string weapon_text = weapon ? weapon->tname() : _( "no weapon" );
    std::string mode_text = selected.is_empty() ? _( "no firing mode" ) : selected.str();
    int ammo = 0;
    if( weapon && !selected.is_empty() ) {
        const gun_mode mode = weapon->gun_get_mode( gun_mode_id( selected.str() ) );
        if( mode ) {
            mode_text = mode.tname();
            ammo = mode->ammo_remaining();
        }
    }
    const std::optional<target_reference> target = stored_target( gunner );
    Creature *live_target = target ? resolve_target( target->character, target->monster ) : nullptr;
    const diag_value repeat = gunner.get_value( "overwatch_repeat" );
    const std::string order = live_target && gunner.get_value( sight_lost_key ).is_str() ?
                              string_format( _( "holding fire; %s is out of sight" ),
                                             live_target->disp_name() ) :
                              live_target ? string_format( repeat.is_str() && repeat.str() == "yes" ?
                                             _( "engaging %s repeatedly" ) : _( "aiming at %s" ),
                                             live_target->disp_name() ) : _( "holding fire" );
    return string_format( _( "%1$s is on overwatch with %2$s, %3$s, %4$d rounds ready; %5$s." ),
                          gunner.disp_name(), weapon_text, mode_text, ammo, order );
}

void report( const npc &gunner )
{
    if( !operator_available( gunner ) ) {
        add_msg( _( "%s is unable to operate overwatch right now." ), gunner.disp_name() );
        return;
    }
    add_msg( "%s", status( gunner ) );
}

bool actualize_fire_event( const overwatch_fire_event_data &event_data )
{
    npc *gunner = event_data.gunner_id.is_valid() ? g->find_npc( event_data.gunner_id ) : nullptr;
    if( gunner == nullptr ) {
        return false;
    }
    const diag_value active_key = gunner->get_value( order_key );
    if( active_key.is_empty() || !active_key.is_str() ||
        active_key.str() != event_data.order_key ) {
        return false;
    }
    const auto cancel_current = [gunner]( const std::string & reason = std::string() ) {
        // The currently actualizing event must not remove itself from the manager's list.
        clear_order_values( *gunner );
        if( !reason.empty() ) {
            report_order_abort( *gunner, reason );
        }
    };
    Creature *target = resolve_target( event_data.target_character, event_data.target_monster );
    if( !operator_available( *gunner ) || !is_assigned( *gunner ) ) {
        cancel_current();
        return false;
    }
    if( target == nullptr ) {
        cancel_current( _( "the target can no longer be tracked" ) );
        return false;
    }
    if( target->is_dead_state() || target->is_hallucination() || !target_is_hostile( *target ) ) {
        cancel_current( _( "the target is no longer a living hostile" ) );
        return false;
    }
    if( rl_dist( gunner->pos_abs(), target->pos_abs() ) > max_range ) {
        cancel_current( _( "the target is beyond overwatch range" ) );
        return false;
    }
    const gun_mode_id mode_id( event_data.mode_id );
    if( !observer_can_see( *gunner, *target ) ) {
        if( !event_data.repeat ) {
            cancel_current( string_format( _( "%s has left my sight" ), target->disp_name() ) );
            return false;
        }
        const std::optional<target_reference> target_ref = make_target_reference( *target );
        if( !target_ref ) {
            cancel_current( _( "the target can no longer be tracked" ) );
            return false;
        }
        store_target( *gunner, *target_ref );
        report_lost_sight( *gunner, *target );
        schedule_sight_check( *gunner, *target, *target_ref, mode_id, event_data.order_key );
        return true;
    }
    if( event_data.repeat ) {
        report_regained_sight( *gunner, *target );
    }
    std::string failure;
    const std::optional<std::pair<gun_mode_id, gun_mode>> selected = selected_mode( *gunner );
    if( !selected || selected->first != mode_id ||
        !mode_is_eligible( *gunner, mode_id, &failure ) ) {
        cancel_current( failure.empty() ? _( "the selected weapon mode is no longer available" ) : failure );
        return false;
    }
    item_location wielded = gunner->get_wielded_item();
    if( !wielded || !wielded->gun_set_mode( mode_id ) ) {
        cancel_current( _( "the selected weapon mode is no longer available" ) );
        return false;
    }
    gun_mode mode = wielded->gun_get_mode( mode_id );
    if( !mode || !mode->ammo_sufficient( gunner ) ) {
        const std::optional<target_reference> target_ref = make_target_reference( *target );
        if( !target_ref || !schedule_reload( *gunner, mode_id, target_ref,
                                            event_data.repeat, true,
                                            event_data.order_key, &failure ) ) {
            add_msg( _( "%1$s reports they are unable to continue firing: %2$s" ),
                     gunner->disp_name(), failure );
            cancel_current();
            return false;
        }
        return true;
    }

    const Target_attributes attributes = target_attributes( *gunner, *target );
    apply_maximum_aim( *gunner, *mode, attributes, event_data.aim_moves );
    if( maximum_aim_moves( *gunner, mode, *target ) > 0 ) {
        const std::optional<target_reference> target_ref = make_target_reference( *target );
        if( !target_ref ) {
            cancel_current( _( "the target can no longer be tracked" ) );
            return false;
        }
        store_target( *gunner, *target_ref );
        schedule_fire( *gunner, *target, *target_ref, mode_id, event_data.repeat,
                       event_data.order_key );
        return true;
    }

    const int distance = rl_dist( gunner->pos_abs(), target->pos_abs() );
    const int normal_range = std::max( 1, mode->gun_range( gunner ) );
    ranged_attack_context context;
    context.projectile_range = std::min( max_range, std::max( normal_range, distance ) );
    context.accuracy_distance = discounted_range( distance, normal_range );
    const int moves_before_firing = gunner->get_moves();
    const int fired = gunner->fire_gun( target->pos_abs(), mode.qty, *mode, context );
    const int firing_moves = std::max( 0, moves_before_firing - gunner->get_moves() );
    // Firing cadence is represented by the next timed event.  Do not also leave
    // move debt for the rooted activity to discard on its next turn.
    gunner->set_moves( moves_before_firing );
    if( fired <= 0 ) {
        cancel_current( _( "I was unable to fire the selected weapon" ) );
        return false;
    }
    if( gunner->is_dead_state() ) {
        cancel_current();
        return true;
    }

    if( !event_data.repeat || target->is_dead_state() ) {
        cancel_current( event_data.repeat ? _( "the target is down" ) : std::string() );
        return true;
    }
    const std::optional<target_reference> target_ref = make_target_reference( *target );
    if( !target_ref ) {
        cancel_current( _( "the target can no longer be tracked" ) );
        return true;
    }
    store_target( *gunner, *target_ref );
    if( !observer_can_see( *gunner, *target ) ) {
        report_lost_sight( *gunner, *target );
        schedule_sight_check( *gunner, *target, *target_ref, mode_id, event_data.order_key );
        return true;
    }
    wielded = gunner->get_wielded_item();
    mode = wielded ? wielded->gun_get_mode( mode_id ) : gun_mode();
    if( mode && mode->ammo_sufficient( gunner ) ) {
        schedule_fire( *gunner, *target, *target_ref, mode_id, true, event_data.order_key,
                       firing_moves );
    } else if( !schedule_reload( *gunner, mode_id, target_ref, true, true,
                                event_data.order_key, &failure, firing_moves ) ) {
        add_msg( _( "%1$s reports they are unable to continue firing: %2$s" ),
                 gunner->disp_name(), failure );
        cancel_current();
    }
    return true;
}

bool actualize_reload_event( const overwatch_fire_event_data &event_data )
{
    npc *gunner = event_data.gunner_id.is_valid() ? g->find_npc( event_data.gunner_id ) : nullptr;
    if( gunner == nullptr ) {
        return false;
    }
    const diag_value active_key = gunner->get_value( order_key );
    if( active_key.is_empty() || !active_key.is_str() ||
        active_key.str() != event_data.order_key ) {
        return false;
    }
    const auto cancel_current = [gunner]( const std::string & reason = std::string() ) {
        clear_order_values( *gunner );
        if( !reason.empty() ) {
            report_order_abort( *gunner, reason );
        }
    };
    if( !operator_available( *gunner ) || !is_assigned( *gunner ) ) {
        cancel_current();
        return false;
    }
    const gun_mode_id mode_id( event_data.mode_id );
    std::string failure;
    const std::optional<std::pair<gun_mode_id, gun_mode>> selected = selected_mode( *gunner );
    if( !selected || selected->first != mode_id ||
        !mode_is_eligible( *gunner, mode_id, &failure ) ||
        !reload_moves_for_mode( *gunner, selected->second ) ||
        !perform_reload( *gunner, mode_id ) ) {
        add_msg( _( "%s reports they can no longer reload the selected weapon mode." ),
                 gunner->disp_name() );
        cancel_current();
        return false;
    }
    if( !event_data.continue_fire ) {
        cancel_current();
        return true;
    }
    Creature *target = resolve_target( event_data.target_character, event_data.target_monster );
    if( target == nullptr ) {
        cancel_current( _( "the target can no longer be tracked" ) );
        return false;
    }
    if( target->is_dead_state() || !target_is_hostile( *target ) ) {
        cancel_current( _( "the target is no longer a living hostile" ) );
        return false;
    }
    if( rl_dist( gunner->pos_abs(), target->pos_abs() ) > max_range ) {
        cancel_current( _( "the target is beyond overwatch range" ) );
        return false;
    }
    const std::optional<target_reference> target_ref = make_target_reference( *target );
    if( !target_ref ) {
        cancel_current();
        return false;
    }
    store_target( *gunner, *target_ref );
    if( !observer_can_see( *gunner, *target ) ) {
        if( !event_data.repeat ) {
            cancel_current( string_format( _( "%s has left my sight" ), target->disp_name() ) );
            return false;
        }
        report_lost_sight( *gunner, *target );
        schedule_sight_check( *gunner, *target, *target_ref, mode_id, event_data.order_key );
        return true;
    }
    if( event_data.repeat ) {
        report_regained_sight( *gunner, *target );
    }
    schedule_fire( *gunner, *target, *target_ref, mode_id, event_data.repeat,
                   event_data.order_key );
    return true;
}

} // namespace overwatch
