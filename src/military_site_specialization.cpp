#include "military_site_specialization.h"

#include <cctype>
#include <cstdint>
#include <numeric>

#include "debug.h"
#include "generic_factory.h"
#include "json.h"
#include "overmap.h"

namespace
{

generic_factory<military_site_specialization> military_site_specialization_factory(
    "military_site_specialization" );

uint32_t stable_mix( uint32_t hash, uint32_t value )
{
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

uint32_t stable_hash( const military_site_specialization_id &id, const tripoint_abs_omt &pos )
{
    uint32_t hash = 2166136261u;
    hash = stable_mix( hash, static_cast<uint32_t>( pos.x() ) );
    hash = stable_mix( hash, static_cast<uint32_t>( pos.y() ) );

    for( const char ch : id.str() ) {
        hash = stable_mix( hash, static_cast<unsigned char>( ch ) );
    }

    return hash;
}

tripoint_abs_omt site_anchor_pos( const tripoint_abs_omt &pos, const oter_id &oter )
{
    const std::string type_id = oter->get_type_id().str();
    static const std::string prefix = "mil_base_";

    if( type_id.compare( 0, prefix.size(), prefix ) == 0 &&
        type_id.size() >= prefix.size() + 2 ) {
        const char col_ch = type_id[prefix.size()];
        const char row_ch = type_id[prefix.size() + 1];

        if( col_ch >= '1' && col_ch <= '8' &&
            std::isalpha( static_cast<unsigned char>( row_ch ) ) ) {
            const int col_offset = col_ch - '1';
            const int row_offset = std::tolower( static_cast<unsigned char>( row_ch ) ) - 'a';

            if( row_offset >= 0 ) {
                return tripoint_abs_omt( pos.x() - col_offset, pos.y() - row_offset, pos.z() );
            }
        }
    }

    return pos;
}

} // namespace

template<>
const military_site_specialization &string_id<military_site_specialization>::obj() const
{
    return military_site_specialization_factory.obj( *this );
}

template<>
bool string_id<military_site_specialization>::is_valid() const
{
    return military_site_specialization_factory.is_valid( *this );
}

void military_site_specialization::load( const JsonObject &jo, std::string_view )
{
    optional( jo, was_loaded, "name", name_, translation() );
    mandatory( jo, was_loaded, "overmap_terrain", overmap_terrain_ );
    optional( jo, was_loaded, "overmap_match_type", match_type_, ot_match_type::exact );

    for( JsonObject entry_jo : jo.get_array( "entries" ) ) {
        military_site_specialization_entry entry;
        mandatory( entry_jo, false, "label", entry.label );
        mandatory( entry_jo, false, "loot_group", entry.loot_group );
        optional( entry_jo, false, "weight", entry.weight, 100 );
        entries_.push_back( entry );
    }
}

void military_site_specialization::check() const
{
    if( overmap_terrain_.empty() ) {
        debugmsg( "Military site specialization %s has no overmap_terrain", id.str() );
    }
    if( entries_.empty() ) {
        debugmsg( "Military site specialization %s has no entries", id.str() );
    }

    for( const military_site_specialization_entry &entry : entries_ ) {
        if( entry.weight <= 0 ) {
            debugmsg( "Military site specialization %s has non-positive entry weight", id.str() );
        }
        if( !entry.loot_group.is_valid() ) {
            debugmsg( "Military site specialization %s uses invalid loot group %s", id.str(),
                      entry.loot_group.str() );
        }
    }
}

const translation &military_site_specialization::name() const
{
    return name_;
}

const std::string &military_site_specialization::overmap_terrain() const
{
    return overmap_terrain_;
}

ot_match_type military_site_specialization::match_type() const
{
    return match_type_;
}

bool military_site_specialization::matches_terrain( const oter_id &oter ) const
{
    return is_ot_match( overmap_terrain_, oter, match_type_ );
}

const military_site_specialization_entry *military_site_specialization::resolve(
    const tripoint_abs_omt &pos, const oter_id &oter ) const
{
    const int total_weight = std::accumulate( entries_.begin(), entries_.end(), 0,
    []( const int acc, const military_site_specialization_entry & entry ) {
        return entry.weight > 0 ? acc + entry.weight : acc;
    } );

    if( total_weight <= 0 ) {
        return nullptr;
    }

    int roll = stable_hash( id, site_anchor_pos( pos, oter ) ) % total_weight;
    for( const military_site_specialization_entry &entry : entries_ ) {
        if( entry.weight <= 0 ) {
            continue;
        }
        if( roll < entry.weight ) {
            return &entry;
        }
        roll -= entry.weight;
    }

    return nullptr;
}

namespace military_site_specializations
{

void load( const JsonObject &jo, const std::string &src )
{
    military_site_specialization_factory.load( jo, src );
}

void reset()
{
    military_site_specialization_factory.reset();
}

void check_consistency()
{
    for( const military_site_specialization &specialization :
         military_site_specialization_factory.get_all() ) {
        specialization.check();
    }
}

const std::vector<military_site_specialization> &get_all()
{
    return military_site_specialization_factory.get_all();
}

const military_site_specialization_entry *resolve( const military_site_specialization_id &id,
        const tripoint_abs_omt &pos, const oter_id &oter )
{
    if( !id.is_valid() ) {
        debugmsg( "Invalid military site specialization id %s", id.str() );
        return nullptr;
    }

    return id->resolve( pos, oter );
}

} // namespace military_site_specializations
