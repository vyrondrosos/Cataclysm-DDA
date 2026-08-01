#include "map_view_ui.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "cached_options.h"
#include "cata_scope_helpers.h"
#include "cursesdef.h"
#include "game.h"
#include "input_context.h"
#include "map.h"
#include "map_viewpoint.h"
#include "options.h"
#include "output.h"
#include "point.h"
#include "string_formatter.h"
#include "translations.h"
#include "ui_manager.h"

std::optional<tripoint_abs_ms> query_map_view( map &viewed_map,
        const map_viewpoint &viewpoint, const map_view_ui_params &params )
{
    const tripoint_bub_ms origin = viewed_map.get_bub( viewpoint.origin() );
    if( !viewed_map.inbounds( origin ) ) {
        return std::nullopt;
    }

    tripoint_bub_ms center = origin;
    tripoint_bub_ms cursor = origin;
    std::string feedback;
    catacurses::window w_view;
    catacurses::window w_info;

    input_context ctxt( "LOOK" );
    ctxt.set_iso( true );
    ctxt.register_directions();
    ctxt.register_action( "COORDINATE" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "TOGGLE_FAST_SCROLL" );
    ctxt.register_action( "CENTER" );
    ctxt.register_action( "LEVEL_UP" );
    ctxt.register_action( "LEVEL_DOWN" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    ui_adaptor ui;
    ui.on_screen_resize( [&w_view, &w_info]( ui_adaptor & ui ) {
        const int height = getmaxy( g->w_terrain );
        const int width = getmaxx( g->w_terrain );
        const point position( getbegx( g->w_terrain ), getbegy( g->w_terrain ) );
        const int info_height = std::min( 3, height );
        w_view = catacurses::newwin( height, width, position );
        w_info = catacurses::newwin( info_height, width,
                                     position + point( 0, height - info_height ) );
        ui.position_from_window( w_view );
    } );
    ui.mark_resize();

    ui.on_redraw( [&viewed_map, &viewpoint, &params, &center, &cursor, &feedback,
                   &w_view, &w_info, &ctxt, &ui]( const ui_adaptor & ) {
        werase( w_view );
        viewed_map.draw_view( w_view, center, viewpoint, cursor );
        const point screen_center( getmaxx( w_view ) / 2, getmaxy( w_view ) / 2 );
        const map_view_ui_overlay *cursor_overlay = nullptr;
        for( const map_view_ui_overlay &overlay : params.overlays ) {
            const tripoint_bub_ms p = viewed_map.get_bub( overlay.pos );
            if( p.z() != center.z() || !viewpoint.sees( viewed_map, p ) ) {
                continue;
            }
            const point screen = screen_center + p.xy().raw() - center.xy().raw();
            if( screen.x < 0 || screen.y < 0 || screen.x >= getmaxx( w_view ) ||
                screen.y >= getmaxy( w_view ) ) {
                continue;
            }
            if( p == cursor ) {
                mvwputch_inv( w_view, screen, overlay.color, overlay.symbol );
                cursor_overlay = &overlay;
            } else {
                mvwputch( w_view, screen, overlay.color, overlay.symbol );
            }
        }
        wnoutrefresh( w_view );

        werase( w_info );
        if( getmaxy( w_info ) > 0 ) {
            center_print( w_info, 0, c_white,
                          string_format( "< %s >", params.title ) );
        }
        if( getmaxy( w_info ) > 1 ) {
            const bool visible = viewpoint.sees( viewed_map, cursor );
            std::string description = _( "No signal" );
            if( visible ) {
                description = cursor_overlay != nullptr && !cursor_overlay->description.empty() ?
                              cursor_overlay->description : viewed_map.name( cursor );
            }
            trim_and_print( w_info, point( 0, 1 ), getmaxx( w_info ),
                            visible ? c_light_gray : c_light_red,
                            "%s  %s", viewed_map.get_abs( cursor ).to_string(), description );
        }
        if( getmaxy( w_info ) > 2 ) {
            const std::string controls = params.select ?
                                         string_format( _( "%1$s select  %2$s center  %3$s exit" ),
                                                        ctxt.get_desc( "CONFIRM" ),
                                                        ctxt.get_desc( "CENTER" ),
                                                        ctxt.get_desc( "QUIT" ) ) :
                                         string_format( _( "%1$s close  %2$s center  %3$s exit" ),
                                                        ctxt.get_desc( "CONFIRM" ),
                                                        ctxt.get_desc( "CENTER" ),
                                                        ctxt.get_desc( "QUIT" ) );
            trim_and_print( w_info, point( 0, 2 ), getmaxx( w_info ),
                            feedback.empty() ? c_light_gray : c_light_red,
                            "%s", feedback.empty() ? controls : feedback );
        }
        wnoutrefresh( w_info );

        const point screen_cursor = screen_center + cursor.xy().raw() - center.xy().raw();
        if( screen_cursor.x >= 0 && screen_cursor.y >= 0 &&
            screen_cursor.x < getmaxx( w_view ) && screen_cursor.y < getmaxy( w_view ) ) {
            ui.set_cursor( w_view, screen_cursor );
        }
    } );

    [[maybe_unused]] const on_out_of_scope redraw_game( []() {
        g->invalidate_main_ui_adaptor();
    } );
    bool fast_scroll = false;
    while( true ) {
        ui_manager::redraw();
        const std::string action = ctxt.handle_input();
        feedback.clear();

        if( action == "QUIT" ) {
            return std::nullopt;
        }
        if( action == "SELECT" || action == "MOUSE_MOVE" || action == "COORDINATE" ) {
            const std::optional<point> mouse = ctxt.get_coordinates_text( w_view );
            if( mouse ) {
                const tripoint_rel_ms offset( mouse->x - getmaxx( w_view ) / 2,
                                              mouse->y - getmaxy( w_view ) / 2, 0 );
                const tripoint_bub_ms candidate = center + offset;
                if( viewed_map.inbounds( candidate ) ) {
                    cursor = candidate;
                }
            }
            if( action != "SELECT" ) {
                continue;
            }
        }
        if( action == "CONFIRM" || action == "SELECT" ) {
            if( !params.select ) {
                return std::nullopt;
            }
            if( viewpoint.sees( viewed_map, cursor ) ) {
                return viewed_map.get_abs( cursor );
            }
            feedback = _( "That point is not visible from this viewpoint." );
            continue;
        }
        if( action == "CENTER" ) {
            center = origin;
            cursor = origin;
            continue;
        }
        if( action == "TOGGLE_FAST_SCROLL" ) {
            fast_scroll = !fast_scroll;
            continue;
        }
        if( action == "LEVEL_UP" || action == "LEVEL_DOWN" ) {
            const int dz = action == "LEVEL_UP" ? 1 : -1;
            const tripoint_bub_ms candidate = cursor + tripoint_rel_ms( 0, 0, dz );
            if( viewed_map.inbounds( candidate ) &&
                std::abs( candidate.z() - origin.z() ) <= fov_3d_z_range ) {
                cursor = candidate;
                center.z() = candidate.z();
            }
            continue;
        }
        if( std::optional<tripoint_rel_ms> direction = ctxt.get_direction_rel_ms( action ) ) {
            if( fast_scroll ) {
                direction->x() *= get_option<int>( "FAST_SCROLL_OFFSET" );
                direction->y() *= get_option<int>( "FAST_SCROLL_OFFSET" );
            }
            const tripoint_bub_ms candidate = cursor + *direction;
            if( viewed_map.inbounds( candidate ) ) {
                cursor = candidate;
                center += *direction;
            }
        }
    }
}
