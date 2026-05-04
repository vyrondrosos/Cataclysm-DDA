#pragma once
#ifndef CATA_SRC_MILITARY_SITE_SPECIALIZATION_H
#define CATA_SRC_MILITARY_SITE_SPECIALIZATION_H

#include <string>
#include <string_view>
#include <vector>

#include "coordinates.h"
#include "item_group.h"
#include "omdata.h"
#include "translation.h"
#include "type_id.h"

class JsonObject;

class military_site_specialization;

using military_site_specialization_id = string_id<military_site_specialization>;

struct military_site_specialization_entry {
    translation label;
    item_group_id loot_group;
    int weight = 100;
};

class military_site_specialization
{
    public:
        military_site_specialization_id id;
        bool was_loaded = false;

        void load( const JsonObject &jo, std::string_view src );
        void check() const;

        const translation &name() const;
        const std::string &overmap_terrain() const;
        ot_match_type match_type() const;
        bool matches_terrain( const oter_id &oter ) const;
        const military_site_specialization_entry *resolve( const tripoint_abs_omt &pos,
                const oter_id &oter ) const;

    private:
        translation name_;
        std::string overmap_terrain_;
        ot_match_type match_type_ = ot_match_type::exact;
        std::vector<military_site_specialization_entry> entries_;
};

namespace military_site_specializations
{
void load( const JsonObject &jo, const std::string &src );
void reset();
void check_consistency();
const std::vector<military_site_specialization> &get_all();
const military_site_specialization_entry *resolve( const military_site_specialization_id &id,
        const tripoint_abs_omt &pos, const oter_id &oter );
} // namespace military_site_specializations

#endif // CATA_SRC_MILITARY_SITE_SPECIALIZATION_H
