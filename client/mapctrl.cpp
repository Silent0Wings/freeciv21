/**************************************************************************
 Copyright (c) 1996-2020 Freeciv21 and Freeciv contributors. This file is
 part of Freeciv21. Freeciv21 is free software: you can redistribute it
 and/or modify it under the terms of the GNU  General Public License  as
 published by the Free Software Foundation, either version 3 of the
 License,  or (at your option) any later version. You should have received
 a copy of the GNU General Public License along with Freeciv21. If not,
 see https://www.gnu.org/licenses/.
**************************************************************************/

// Qt
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
// common
#include "control.h"
#include "goto.h"
#include "map.h"
// client
#include "chatline_common.h"
#include "citydlg_common.h"
#include "client_main.h"
#include "mapctrl.h"
#include "mapctrl_common.h"
#include "mapview_common.h"
#include "themes_common.h"
#include "tile.h"
#include "tilespec.h"
#include "unit.h"
// gui-qt
#include "citydlg.h"
#include "fc_client.h"
#include "mapview.h"
#include "messagewin.h"
#include "page_game.h"
#include "shortcuts.h"
#include "unitselect.h"

extern void top_bar_disable_end_turn(bool do_restore);
extern void qload_lua_script();
extern void qreload_lua_script();

/**
   Popup a dialog to ask for the name of a new city.  The given string
   should be used as a suggestion.
 */
void popup_newcity_dialog(struct unit *punit, const char *suggestname)
{
  hud_input_box *ask = new hud_input_box(king()->central_wdg);
  int index = tile_index(unit_tile(punit));

  ask->set_text_title_definput(_("What should we call our new city?"),
                               _("Build New City"), suggestname);
  ask->setAttribute(Qt::WA_DeleteOnClose);
  QObject::connect(ask, &hud_input_box::finished, [=](int result) {
    if (result == QDialog::Accepted) {
      QByteArray ask_bytes;

      ask_bytes = ask->input_edit.text().toLocal8Bit();
      finish_city(index_to_tile(&(wld.map), index), ask_bytes.data());
    } else {
      cancel_city(index_to_tile(&(wld.map), index));
    }
  });
  ask->show();
}

/**
   A turn done button should be provided for the player.  This function
   is called to toggle it between active/inactive.
 */
void set_turn_done_button_state(bool state)
{
  top_bar_disable_end_turn(state);
}

/**
   Draw a goto or patrol line at the current mouse position.
 */
void create_line_at_mouse_pos(void)
{
  QPoint global_pos, local_pos;
  int x, y;

  global_pos = QCursor::pos();
  local_pos = queen()->mapview_wdg->mapFromGlobal(global_pos)
              / queen()->mapview_wdg->scale();
  x = local_pos.x();
  y = local_pos.y();

  if (x >= 0 && y >= 0 && x < mapview.width && y < mapview.width) {
    update_line(x, y);
  }
}

/**
   The Area Selection rectangle. Called by center_tile_mapcanvas() and
   when the mouse pointer moves.
 */
void update_rect_at_mouse_pos(void)
{ // PLS DONT PORT IT
}

/**
   Keyboard handler for map_view
 */
void map_view::keyPressEvent(QKeyEvent *event)
{
  Qt::KeyboardModifiers key_mod = QApplication::keyboardModifiers();
  bool is_shift = key_mod.testFlag(Qt::ShiftModifier);

  auto scale = queen()->mapview_wdg->scale();

  if (C_S_RUNNING == client_state()) {
    if (is_shift) {
      switch (event->key()) {
      case Qt::Key_Return:
      case Qt::Key_Enter:
        key_end_turn();
        return;
      default:
        break;
      }
    }

    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_8:
      if (is_shift) {
        recenter_button_pressed(queen()->mapview_wdg->width() / 2 / scale,
                                0);
      } else {
        key_unit_move(DIR8_NORTH);
      }
      return;
    case Qt::Key_Left:
    case Qt::Key_4:
      if (is_shift) {
        recenter_button_pressed(0,
                                queen()->mapview_wdg->height() / 2 / scale);
      } else {
        key_unit_move(DIR8_WEST);
      }
      return;
    case Qt::Key_Right:
    case Qt::Key_6:
      if (is_shift) {
        recenter_button_pressed(queen()->mapview_wdg->width() / scale,
                                queen()->mapview_wdg->height() / 2 / scale);
      } else {
        key_unit_move(DIR8_EAST);
      }
      return;
    case Qt::Key_Down:
    case Qt::Key_2:
      if (is_shift) {
        recenter_button_pressed(queen()->mapview_wdg->width() / 2 / scale,
                                queen()->mapview_wdg->height() / scale);
      } else {
        key_unit_move(DIR8_SOUTH);
      }
      return;
    case Qt::Key_PageUp:
    case Qt::Key_9:
      key_unit_move(DIR8_NORTHEAST);
      return;
    case Qt::Key_PageDown:
    case Qt::Key_3:
      key_unit_move(DIR8_SOUTHEAST);
      return;
    case Qt::Key_Home:
    case Qt::Key_7:
      key_unit_move(DIR8_NORTHWEST);
      return;
    case Qt::Key_End:
    case Qt::Key_1:
      key_unit_move(DIR8_SOUTHWEST);
      return;
    case Qt::Key_5:
    case Qt::Key_Clear:
      key_recall_previous_focus_unit();
      return;
    case Qt::Key_Escape:
      key_cancel_action();
      return;
    case Qt::Key_Enter:
    case Qt::Key_Return:
      // Ensure chat is visible
      queen()->chat->take_focus();
      return;
    default:
      break;
    }
    shortcut_pressed(event->key());
  }
}

/**
   Pressed mouse or keyboard
 */
void map_view::shortcut_pressed(int key)
{
  Qt::MouseButtons bt;
  QPoint pos;
  fc_shortcut *sc;
  fc_shortcut *sc_sec;
  Qt::KeyboardModifiers md;
  production_widget *pw;
  struct tile *ptile = nullptr;
  struct city *pcity = nullptr;

  md = QApplication::keyboardModifiers();
  bt = QApplication::mouseButtons();
  pos = mapFromGlobal(QCursor::pos()) / scale();

  ptile = canvas_pos_to_tile(pos.x(), pos.y());
  pcity = ptile ? tile_city(ptile) : nullptr;

  if (pcity && pcity->owner != client_player()) {
    pcity = nullptr;
  }

  // Trade Generator - skip
  sc = fc_shortcuts::sc()->get_shortcut(SC_SELECT_BUTTON);
  if (bt == sc->mouse && md == sc->mod && king()->trade_gen.hover_city) {
    king()->trade_gen.add_tile(ptile);
    queen()->mapview_wdg->repaint();
    return;
  }

  // Rally point - select city - skip
  if (bt == sc->mouse && md == sc->mod && king()->rallies.hover_city) {
    char text[1024];

    if (ptile && tile_city(ptile)) {
      king()->rallies.hover_tile = true;
      king()->rallies.rally_city = tile_city(ptile);

      fc_snprintf(text, sizeof(text),
                  _("Selected city %s. Now choose rally point."),
                  city_link(tile_city(ptile)));
      output_window_append(ftc_client, text);
    } else {
      output_window_append(ftc_client, _("No city selected. Aborted"));
    }
    return;
  }

  // Rally point - select tile  - skip
  if (bt == Qt::LeftButton && king()->rallies.hover_tile
      && ptile != nullptr) {
    char text[1024];

    struct city *pcity = king()->rallies.rally_city;
    fc_assert_ret(pcity != nullptr);

    if (send_rally_tile(pcity, ptile)) {
      fc_snprintf(text, sizeof(text),
                  _("Tile %s set as rally point from city %s."),
                  tile_link(ptile), city_link(pcity));
      output_window_append(ftc_client, text);
    } else {
      fc_snprintf(text, sizeof(text),
                  _("Could not set rally point for city %s."),
                  city_link(pcity));
      output_window_append(ftc_client, text);
    }

    king()->rallies.rally_city = nullptr;
    king()->rallies.hover_tile = false;
    return;
  }

  if (bt == Qt::LeftButton && king()->menu_bar->delayed_order && ptile) {
    king()->menu_bar->set_tile_for_order(ptile);
    clear_hover_state();
    exit_goto_state();
    king()->menu_bar->delayed_order = false;
    return;
  }

  if (bt == Qt::LeftButton && king()->menu_bar->quick_airlifting && ptile) {
    if (tile_city(ptile)) {
      multiairlift(tile_city(ptile), king()->menu_bar->airlift_type_id);
    } else {
      output_window_append(ftc_client, "No city selected for airlift");
    }
    king()->menu_bar->quick_airlifting = false;
    return;
  }
  // Check configured shortcuts
  if (!king()->menu_bar->delayed_order) {
    sc = fc_shortcuts::sc()->get_shortcut(SC_QUICK_SELECT);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && pcity != nullptr) {
      pw = new production_widget(this, pcity, false, 0, 0, true);
      pw->show();
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_SHOW_UNITS);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && ptile != nullptr && unit_list_size(ptile->units) > 0) {
      toggle_unit_sel_widget(ptile);
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_COPY_PROD);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && ptile != nullptr) {
      clipboard_copy_production(ptile);
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_POPUP_COMB_INF);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && queen()->battlelog_wdg != nullptr) {
      queen()->battlelog_wdg->show();
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_PASTE_PROD);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && pcity != nullptr) {
      clipboard_paste_production(pcity);
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_RELOAD_THEME);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      load_theme(gui_options.gui_qt_default_theme_name);
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_RELOAD_TILESET);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      QPixmapCache::clear();
      tilespec_reread(tileset_basename(tileset), true);
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_LOAD_LUA);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      qload_lua_script();
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_RELOAD_LUA);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      qreload_lua_script();
      return;
    }

    sc = fc_shortcuts::sc()->get_shortcut(SC_HIDE_WORKERS);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      key_city_overlay(pos.x(), pos.y());
      return;
    }
    sc = fc_shortcuts::sc()->get_shortcut(SC_MAKE_LINK);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && ptile != nullptr) {
      queen()->chat->make_link(ptile);
      return;
    }
    sc = fc_shortcuts::sc()->get_shortcut(SC_BUY_MAP);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && pcity != nullptr) {
      city_buy_production(pcity);
      return;
    }
    sc = fc_shortcuts::sc()->get_shortcut(SC_QUICK_BUY);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
        && pcity != nullptr) {
      pw = new production_widget(this, pcity, false, 0, 0, true, true);
      pw->show();
      return;
    }
    sc = fc_shortcuts::sc()->get_shortcut(SC_APPEND_FOCUS);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      action_button_pressed(pos.x(), pos.y(), SELECT_APPEND);
      return;
    }
    sc = fc_shortcuts::sc()->get_shortcut(SC_ADJUST_WORKERS);
    if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
      adjust_workers_button_pressed(pos.x(), pos.y());
      return;
    }
  }

  sc = fc_shortcuts::sc()->get_shortcut(SC_SCROLL_MAP);
  sc_sec = fc_shortcuts::sc()->get_shortcut(SC_SELECT_BUTTON);
  if ((((key && key == sc->key)
        || (bt == sc->mouse
            /* check if keyboardless goto active and its shortcut if pressed
             */
            || (goto_is_active() && (bt == (sc->mouse | sc_sec->mouse)))))
       && md == sc->mod)) {
    recenter_button_pressed(pos.x(), pos.y());
    return;
  }
  sc = fc_shortcuts::sc()->get_shortcut(SC_SELECT_BUTTON);
  if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
    if (!goto_is_active()) {
      stored_autocenter = gui_options.auto_center_on_unit;
      gui_options.auto_center_on_unit = false;
      action_button_pressed(pos.x(), pos.y(), SELECT_FOCUS);
    }
    return;
  }

  sc = fc_shortcuts::sc()->get_shortcut(SC_POPUP_INFO);
  if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod
      && ptile != nullptr) {
    popup_tile_info(ptile);
    return;
  }

  sc = fc_shortcuts::sc()->get_shortcut(SC_WAKEUP_SENTRIES);
  if (((key && key == sc->key) || bt == sc->mouse) && md == sc->mod) {
    wakeup_button_pressed(pos.x(), pos.y());
    return;
  }
}

/**
   Released mouse buttons
 */
void map_view::shortcut_released(Qt::MouseButton bt)
{
  QPoint pos;
  fc_shortcut *sc;
  Qt::KeyboardModifiers md;
  md = QApplication::keyboardModifiers();
  pos = mapFromGlobal(QCursor::pos()) / scale();

  sc = fc_shortcuts::sc()->get_shortcut(SC_POPUP_INFO);
  if (bt == sc->mouse && md == sc->mod) {
    popdown_tile_info();
    return;
  }

  sc = fc_shortcuts::sc()->get_shortcut(SC_SELECT_BUTTON);
  if (bt == sc->mouse && md == sc->mod) {
    if (king()->trade_gen.hover_city || king()->rallies.hover_city) {
      king()->trade_gen.hover_city = false;
      king()->rallies.hover_city = false;
      return;
    }
    if (menu_click) {
      menu_click = false;
      return;
    }
    if (!keyboardless_goto_active || goto_is_active()) {
      action_button_pressed(pos.x(), pos.y(), SELECT_POPUP);
      gui_options.auto_center_on_unit = stored_autocenter;
    }
    release_goto_button(pos.x(), pos.y());
    return;
  }
}

/**
   Mouse buttons handler for map_view
 */
void map_view::mousePressEvent(QMouseEvent *event) { shortcut_pressed(0); }

/**
   Mouse release event for map_view
 */
void map_view::mouseReleaseEvent(QMouseEvent *event)
{
  shortcut_released(event->button());
}

/**
   Mouse movement handler for map_view
 */
void map_view::mouseMoveEvent(QMouseEvent *event)
{
  update_line(event->pos().x() / scale(), event->pos().y() / scale());
  if (keyboardless_goto_button_down && hover_state == HOVER_NONE) {
    maybe_activate_keyboardless_goto(event->pos().x() / scale(),
                                     event->pos().y() / scale());
  }
  control_mouse_cursor(canvas_pos_to_tile(event->pos().x() / scale(),
                                          event->pos().y() / scale()));
}