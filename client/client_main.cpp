/***********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#ifdef FREECIV_MSWINDOWS
#include <windows.h> /* LoadLibrary() */
#endif

#include <math.h>
#include <signal.h>

// Qt
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGlobalStatic>
#include <QTimer>

/* utility */
#include "bitvector.h"
#include "capstr.h"
#include "dataio.h"
#include "deprecations.h"
#include "fcbacktrace.h"
#include "fciconv.h"
#include "fcintl.h"
#include "log.h"
#include "mem.h"
#include "rand.h"
#include "registry.h"
#include "support.h"

/* common */
#include "ai.h"
#include "diptreaty.h"
#include "fc_interface.h"
#include "game.h"
#include "idex.h"
#include "map.h"
#include "mapimg.h"
#include "net_types.h"
#include "packets.h"
#include "player.h"
#include "research.h"
#include "server_settings.h"
#include "version.h"

/* client/include */
#include "chatline_g.h"
#include "citydlg_g.h"
#include "connectdlg_g.h"
#include "dialogs_g.h"
#include "diplodlg_g.h"
#include "editgui_g.h"
#include "graphics_g.h"
#include "gui_main_g.h"
#include "mapctrl_g.h"
#include "mapview_g.h"
#include "menu_g.h"
#include "messagewin_g.h"
#include "pages_g.h"
#include "plrdlg_g.h"
#include "repodlgs_g.h"
#include "voteinfo_bar_g.h"

/* client */
#include "attribute.h"
#include "audio.h"
#include "cityrepdata.h"
#include "climisc.h"
#include "clinet.h"
#include "connectdlg_common.h" /* client_kill_server() */
#include "control.h"
#include "editor.h"
#include "global_worklist.h"
#include "governor.h"
#include "helpdata.h" /* boot_help_texts() */
#include "mapview_common.h"
#include "music.h"
#include "options.h"
#include "overview_common.h"
#include "packhand.h"
#include "themes_common.h"
#include "tilespec.h"
#include "update_queue.h"
#include "voteinfo.h"

/* client/luascript */
#include "script_client.h"

#include "client_main.h"

#include "packhand_gen.h"

static enum known_type mapimg_client_tile_known(const struct tile *ptile,
                                                const struct player *pplayer,
                                                bool knowledge);
static struct terrain *
mapimg_client_tile_terrain(const struct tile *ptile,
                           const struct player *pplayer, bool knowledge);
static struct player *mapimg_client_tile_owner(const struct tile *ptile,
                                               const struct player *pplayer,
                                               bool knowledge);
static struct player *mapimg_client_tile_city(const struct tile *ptile,
                                              const struct player *pplayer,
                                              bool knowledge);
static struct player *mapimg_client_tile_unit(const struct tile *ptile,
                                              const struct player *pplayer,
                                              bool knowledge);
static int mapimg_client_plrcolor_count(void);
static struct rgbcolor *mapimg_client_plrcolor_get(int i);

static void fc_interface_init_client(void);

QString logfile;
QString scriptfile;
QString savefile;
QString forced_tileset_name;
QString sound_plugin_name;
QString sound_set_name;
QString music_set_name;
QString server_host;
QString user_name;
char password[MAX_LEN_PASSWORD] = "\0";
QString cmd_metaserver;
int server_port = -1;
bool auto_connect =
    FALSE;               /* TRUE = skip "Connect to Freeciv Server" dialog */
bool auto_spawn = FALSE; /* TRUE = skip main menu, start local server */
enum announce_type announce;

struct civclient client;

static enum client_states civclient_state = C_S_INITIAL;

/* TRUE if an end turn request is blocked by busy agents */
bool waiting_for_end_turn = FALSE;

/*
 * TRUE between receiving PACKET_END_TURN and PACKET_BEGIN_TURN
 */
static bool server_busy = FALSE;

#ifdef FREECIV_DEBUG
bool hackless = FALSE;
#endif

static bool client_quitting = FALSE;

/**********************************************************************/ /**
   Convert a text string from the internal to the data encoding, when it
   is written to the network.
 **************************************************************************/
static char *put_conv(const char *src, size_t *length)
{
  char *out = internal_to_data_string_malloc(src);

  if (out) {
    *length = strlen(out);
    return out;
  } else {
    *length = 0;
    return NULL;
  }
}

/**********************************************************************/ /**
   Convert a text string from the data to the internal encoding when it is
   first read from the network.  Returns FALSE if the destination isn't
   large enough or the source was bad.
 **************************************************************************/
static bool get_conv(char *dst, size_t ndst, const char *src, size_t nsrc)
{
  char *out = data_to_internal_string_malloc(src);
  bool ret = TRUE;
  size_t len;

  if (!out) {
    dst[0] = '\0';
    return FALSE;
  }

  len = strlen(out);
  if (ndst > 0 && len >= ndst) {
    ret = FALSE;
    len = ndst - 1;
  }

  memcpy(dst, out, len);
  dst[len] = '\0';
  delete[] out;

  return ret;
}

/**********************************************************************/ /**
   Set up charsets for the client.
 **************************************************************************/
static void charsets_init(void)
{
  dio_set_put_conv_callback(put_conv);
  dio_set_get_conv_callback(get_conv);
}

/**********************************************************************/ /**
   This is called at program exit in any emergency. This is registered
   as at_quick_exit() callback, so no destructor kind of actions here
 **************************************************************************/
static void emergency_exit(void) { client_kill_server(TRUE); }

/**********************************************************************/ /**
   This is called at program exit.
 **************************************************************************/
static void at_exit(void)
{
  emergency_exit();
  packets_deinit();
  update_queue_free();
  fc_destroy_ow_mutex();
}

/**********************************************************************/ /**
   Called only by set_client_state() below.
 **************************************************************************/
static void client_game_init(void)
{
  client.conn.playing = NULL;
  client.conn.observer = FALSE;

  game_init(FALSE);
  attribute_init();
  control_init();
  link_marks_init();
  voteinfo_queue_init();
  server_options_init();
  update_queue_init();
  mapimg_init(mapimg_client_tile_known, mapimg_client_tile_terrain,
              mapimg_client_tile_owner, mapimg_client_tile_city,
              mapimg_client_tile_unit, mapimg_client_plrcolor_count,
              mapimg_client_plrcolor_get);
  animations_init();
}

/**********************************************************************/ /**
   Called by set_client_state() and client_exit() below.
 **************************************************************************/
static void client_game_free(void)
{
  editgui_popdown_all();

  animations_free();
  mapimg_free();
  packhand_free();
  server_options_free();
  voteinfo_queue_free();
  link_marks_free();
  control_free();
  free_help_texts();
  attribute_free();
  governor::i()->drop();
  game.client.ruleset_init = FALSE;
  game.client.ruleset_ready = FALSE;
  game_free();
  /* update_queue_init() is correct at this point. The queue is reset to
     a clean state which is also needed if the client is not connected to
     the server! */
  update_queue_init();

  client.conn.playing = NULL;
  client.conn.observer = FALSE;
}

/**********************************************************************/ /**
   Called only by set_client_state() below.  Just free what is needed to
   change view (player target).
 **************************************************************************/
static void client_game_reset(void)
{
  editgui_popdown_all();

  packhand_free();
  link_marks_free();
  control_free();
  attribute_free();
  governor::i()->drop();

  game_reset();
  mapimg_reset();

  attribute_init();
  control_init();
  link_marks_init();
}

/**********************************************************************/ /**
   Entry point for common client code.
 **************************************************************************/
int client_main(int argc, char *argv[])
{
  /* Load win32 post-crash debugger */
#ifdef FREECIV_MSWINDOWS
  if (LoadLibrary("exchndl.dll") == NULL) {
#ifdef FREECIV_DEBUG
    fprintf(stderr, "exchndl.dll could not be loaded, no crash debugger\n");
#endif /* FREECIV_DEBUG */
  }
#endif /* FREECIV_MSWINDOWS */

  QApplication app(argc, argv);
  QCoreApplication::setApplicationVersion(VERSION_STRING);

  i_am_client(); /* Tell to libfreeciv that we are client */

  fc_interface_init_client();

  game.client.ruleset_init = FALSE;

  /* Ensure that all AIs are initialized to unused state
   * Not using ai_type_iterate as it would stop at
   * current ai type count, ai_type_get_count(), i.e., 0 */
  for (int aii = 0; aii < FREECIV_AI_MOD_LAST; aii++) {
    struct ai_type *ai = get_ai_type(aii);

    init_ai(ai);
  }

  init_nls();
#ifdef ENABLE_NLS
  (void) bindtextdomain("freeciv-nations", get_locale_dir());
#endif

  audio_init();
  init_character_encodings(gui_character_encoding, gui_use_transliteration);
#ifdef ENABLE_NLS
  bind_textdomain_codeset("freeciv-nations", get_internal_encoding());
#endif

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();

  // List of all the supported options
  bool ok = parser.addOptions(
      {{{"A", _("Announce")},
        // TRANS: Do not translate IPv4, IPv6 and none
        _("Announce game in LAN using protocol PROTO (IPv4/IPv6/none)"),
        _("PROTO"),
        "IPv4"},
       {{"a", "autoconnect"}, _("Skip connect dialog")},
       {{"d", _("debug")},
        // TRANS: Do not translate "fatal", "critical", "warning", "info" or
        //        "debug". It's exactly what the user must type.
        _("Set debug log level (fatal/critical/warning/info/debug)"),
        _("LEVEL"),
        QStringLiteral("info")},
       {{"F", _("Fatal")}, _("Raise a signal on failed assertion")},
       {{"f", "file"}, _("Load saved game FILE"), "FILE"},
#ifdef FREECIV_DEBUG
       {{"H", "Hackless"},
        _("Do not request hack access to local, but not spawned, server")},
#endif /* FREECIV_DEBUG */
       {{"l", "log"},
        _("Use FILE as logfile (spawned server also uses this)"),
        "FILE"},
       {{"M", "Meta"}, _("Connect to the metaserver at HOST"), "HOST"},
       {{"n", "name"}, _("Use NAME as username on server"), "NAME"},
       {{"p", "port"},
        _("Connect to server port PORT (usually with -a)"),
        "PORT"},
       {{"P", _("Plugin")},
        QString::asprintf(_("Use PLUGIN for sound output %s"),
                          audio_get_all_plugin_names()),
        "PLUGIN"},
       {{"r", "read"},
        _("Read startup script FILE (for spawned server only)"),
        "FILE"},
       {{"s", "server"},
        _("Connect to the server at HOST (usually with -a)"),
        "HOST"},
       {{"S", "Sound"}, _("Read sound tags from FILE"), "FILE"},
       {{"m", "music"}, _("Read music tags from FILE"), "FILE"},
       {{"t", "tiles"}, _("Use data file FILE.tilespec for tiles"), "FILE"},
       {{"w", "warnings"}, _("Warn about deprecated modpack constructs")}});
  if (!ok) {
    qFatal("Adding command line arguments failed");
    exit(EXIT_FAILURE);
  }
  parser.process(app);

  // Process the parsed options
  if (!log_init(parser.value(QStringLiteral("debug")))) {
    exit(EXIT_FAILURE);
  }
  if (parser.isSet(QStringLiteral("version"))) {
    fc_fprintf(stderr, "%s %s\n", freeciv_name_version(), client_string);
    exit(EXIT_SUCCESS);
  }
#ifdef FREECIV_DEBUG
  if (parser.isSet(QStringLiteral("Hackless"))) {
    hackless = TRUE;
  }
#endif
  if (parser.isSet(QStringLiteral("log"))) {
    logfile = parser.value(QStringLiteral("log"));
  }
  fc_assert_set_fatal(parser.isSet(QStringLiteral("Fatal")));
  if (parser.isSet(QStringLiteral("read"))) {
    scriptfile = parser.value(QStringLiteral("read"));
  }
  if (parser.isSet(QStringLiteral("file"))) {
    savefile = parser.value(QStringLiteral("file"));
    auto_spawn = TRUE;
  }
  if (parser.isSet(QStringLiteral("name"))) {
    user_name = parser.value(QStringLiteral("name"));
  }
  if (parser.isSet(QStringLiteral("Meta"))) {
    cmd_metaserver = parser.value(QStringLiteral("Meta"));
  }
  if (parser.isSet(QStringLiteral("Sound"))) {
    sound_set_name = parser.value(QStringLiteral("Sound"));
  }
  if (parser.isSet(QStringLiteral("music"))) {
    music_set_name = parser.value(QStringLiteral("music"));
  }
  if (parser.isSet(QStringLiteral("Plugin"))) {
    sound_plugin_name = parser.value(QStringLiteral("Plugin"));
  }
  if (parser.isSet(QStringLiteral("port"))) {
    bool conversion_ok;
    server_port =
        parser.value(QStringLiteral("port")).toUInt(&conversion_ok);
    if (!conversion_ok) {
      qFatal(_("Invalid port number %s"), qPrintable(parser.value("port")));
      exit(EXIT_FAILURE);
    }
  }
  if (parser.isSet(QStringLiteral("autoconnect"))) {
    auto_connect = TRUE;
  }
  if (parser.isSet(QStringLiteral("tiles"))) {
    forced_tileset_name = parser.value(QStringLiteral("tiles"));
  }
  announce = ANNOUNCE_DEFAULT;
  if (parser.isSet(QStringLiteral("Announce"))) {
    auto value = parser.value(QStringLiteral("Announce")).toLower();
    if (value == QLatin1String("ipv4")) {
      announce = ANNOUNCE_IPV4;
    } else if (value == QLatin1String("ipv6")) {
      announce = ANNOUNCE_IPV6;
    } else if (value == QLatin1String("none")) {
      announce = ANNOUNCE_NONE;
    } else {
      qCritical(_("Illegal value \"%s\" for --Announce"),
                qPrintable(parser.value("Announce")));
    }
  }
  if (parser.isSet(QStringLiteral("warnings"))) {
    qCWarning(deprecations_category,
              // TRANS: Do not translate --warnings
              _("The --warnings option is deprecated."));
  }

  if (auto_spawn && auto_connect) {
    /* TRANS: don't translate option names */
    fc_fprintf(stderr, _("-f/--file and -a/--autoconnect options are "
                         "incompatible\n"));
    exit(EXIT_FAILURE);
  }

  /* disallow running as root -- too dangerous */
  dont_run_as_root(argv[0], "freeciv_client");

  log_set_file(logfile);
  backtrace_init();

  /* after log_init: */

  (void) user_username(gui_options.default_user_name, MAX_LEN_NAME);
  if (!is_valid_username(gui_options.default_user_name)) {
    char buf[sizeof(gui_options.default_user_name)];

    fc_snprintf(buf, sizeof(buf), "_%s", gui_options.default_user_name);
    if (is_valid_username(buf)) {
      sz_strlcpy(gui_options.default_user_name, buf);
    } else {
      fc_snprintf(gui_options.default_user_name,
                  sizeof(gui_options.default_user_name), "player%d",
                  fc_rand(10000));
    }
  }

  /* initialization */

  game.all_connections = conn_list_new();
  game.est_connections = conn_list_new();

  ui_init();
  charsets_init();
  update_queue_init();

  fc_init_ow_mutex();

  /* register exit handler */
  atexit(at_exit);
  fc_at_quick_exit(emergency_exit);

  init_our_capability();
  init_player_dlg_common();
  init_themes();

  options_init();
  options_load();

  script_client_init();

  if (sound_set_name.isEmpty()) {
    sound_set_name = gui_options.default_sound_set_name;
  }
  if (music_set_name.isEmpty()) {
    music_set_name = gui_options.default_music_set_name;
  }
  if (sound_plugin_name.isEmpty()) {
    sound_plugin_name = gui_options.default_sound_plugin_name;
  }
  if (server_host.isEmpty()) {
    server_host = gui_options.default_server_host;
  } else if (gui_options.use_prev_server) {
    sz_strlcpy(gui_options.default_server_host, qUtf8Printable(server_host));
  }
  if (user_name.isEmpty()) {
    user_name = gui_options.default_user_name;
  }
  if (cmd_metaserver.isEmpty()) {
    /* FIXME: Find a cleaner way to achieve this. */
    /* www.cazfi.net/freeciv/metaserver/ was default metaserver
     * over one release when meta.freeciv.org was unavailable. */
    const char *oldaddr = "http://www.cazfi.net/freeciv/metaserver/";

    if (0 == strcmp(gui_options.default_metaserver, oldaddr)) {
      qInfo(_("Updating old metaserver address \"%s\"."), oldaddr);
      sz_strlcpy(gui_options.default_metaserver, DEFAULT_METASERVER_OPTION);
      qInfo(_("Default metaserver has been set to value \"%s\"."),
            DEFAULT_METASERVER_OPTION);
    }
    if (0
        == strcmp(gui_options.default_metaserver,
                  DEFAULT_METASERVER_OPTION)) {
      cmd_metaserver = FREECIV_META_URL;
    } else {
      cmd_metaserver = gui_options.default_metaserver;
    }
  }
  if (server_port == -1) {
    server_port = gui_options.default_server_port;
  } else if (gui_options.use_prev_server) {
    gui_options.default_server_port = server_port;
  }

  /* This seed is not saved anywhere; randoms in the client should
     have cosmetic effects only (eg city name suggestions).  --dwp */
  fc_srand(time(NULL));
  boot_help_texts();

  fill_topo_ts_default();

  if (!forced_tileset_name.isEmpty()) {
    if (!tilespec_try_read(qUtf8Printable(forced_tileset_name), TRUE, -1,
                           TRUE)) {
      qCritical(_("Can't load requested tileset %s!"),
                qUtf8Printable(forced_tileset_name));
      client_exit();
      return EXIT_FAILURE;
    }
  } else {
    tilespec_try_read(gui_options.default_tileset_name, FALSE, -1, TRUE);
  }

  audio_real_init(sound_set_name, music_set_name, sound_plugin_name);
  start_menu_music("music_menu", NULL);

  editor_init();

  /* run gui-specific client */
  ui_main();

  /* termination */
  client_exit();

  /* not reached */
  return EXIT_SUCCESS;
}

/**********************************************************************/ /**
   Write messages from option saving to the log.
 **************************************************************************/
static void log_option_save_msg(QtMsgType lvl, const QString &msg)
{
  switch (lvl) {
  case QtDebugMsg:
    qDebug("%s", qPrintable(msg));
    break;
  case QtInfoMsg:
    qInfo("%s", qPrintable(msg));
    break;
  case QtWarningMsg:
    qWarning("%s", qPrintable(msg));
    break;
  case QtCriticalMsg:
    qCritical("%s", qPrintable(msg));
    break;
  case QtFatalMsg:
    qFatal("%s", qPrintable(msg));
    break;
  }
}

/**********************************************************************/ /**
   Main client execution stop function. This calls ui_exit() and not the
   other way around.
 **************************************************************************/
void client_exit(void)
{
  if (client_state() >= C_S_PREPARING) {
    attribute_flush();
    client_remove_all_cli_conn();
  }

  if (gui_options.save_options_on_exit) {
    options_save(log_option_save_msg);
  }

  overview_free();
  if (unscaled_tileset != NULL) {
    tileset_free(unscaled_tileset);
  }
  tileset_free(tileset);

  ui_exit();

  script_client_free();

  editor_free();
  options_free();
  if (client_state() >= C_S_PREPARING) {
    client_game_free();
  }

  conn_list_destroy(game.all_connections);
  conn_list_destroy(game.est_connections);

  free_libfreeciv();
  free_nls();

  backtrace_deinit();
  log_close();

  exit(EXIT_SUCCESS);
}

/**********************************************************************/ /**
   Handle packet received from server.
 **************************************************************************/
void client_packet_input(void *packet, int type)
{
  if (!client.conn.established && PACKET_CONN_PING != type
      && PACKET_PROCESSING_STARTED != type
      && PACKET_PROCESSING_FINISHED != type
      && PACKET_SERVER_JOIN_REPLY != type
      && PACKET_AUTHENTICATION_REQ != type && PACKET_SERVER_SHUTDOWN != type
      && PACKET_CONNECT_MSG != type && PACKET_EARLY_CHAT_MSG != type
      && PACKET_SERVER_INFO != type) {
    qCritical("Received packet %s (%d) before establishing connection!",
              packet_name(static_cast<packet_type>(type)), type);
    disconnect_from_server();
  } else if (!client_handle_packet(static_cast<packet_type>(type), packet)) {
    qCritical("Received unknown packet (type %d) from server!", type);
    disconnect_from_server();
  }
}

/**********************************************************************/ /**
   Handle user ending his/her turn.
 **************************************************************************/
void user_ended_turn(void) { send_turn_done(); }

/**********************************************************************/ /**
   Send information about player having finished his/her turn to server.
 **************************************************************************/
void send_turn_done(void)
{
  log_debug("send_turn_done() can_end_turn=%d", can_end_turn());

  if (!can_end_turn()) {
    /*
     * The turn done button is disabled but the user may have pressed
     * the return key.
     */

    if (!governor::i()->hot()) {
      waiting_for_end_turn = TRUE;
    }

    return;
  }

  waiting_for_end_turn = FALSE;

  attribute_flush();

  dsend_packet_player_phase_done(&client.conn, game.info.turn);

  update_turn_done_button_state();
}

/**********************************************************************/ /**
   Send request for some report to server
 **************************************************************************/
void send_report_request(enum report_type type)
{
  dsend_packet_report_req(&client.conn, type);
}

/**********************************************************************/ /**
   Change client state.
 **************************************************************************/
void set_client_state(enum client_states newstate)
{
  enum client_states oldstate = civclient_state;
  struct player *pplayer = client_player();

  if (auto_spawn) {
    fc_assert(!auto_connect);
    auto_spawn = FALSE;
    if (!client_start_server()) {
      qFatal(_("Failed to start local server; aborting."));
      exit(EXIT_FAILURE);
    }
  }

  if (auto_connect && newstate == C_S_DISCONNECTED) {
    if (oldstate == C_S_DISCONNECTED) {
      qFatal(_("There was an error while auto connecting; aborting."));
      exit(EXIT_FAILURE);
    } else {
      start_autoconnecting_to_server();
      auto_connect = FALSE; /* Don't try this again. */
    }
  }

  if (C_S_PREPARING == newstate
      && (client_has_player() || client_is_observer())) {
    /* Reset the delta-state. */
    conn_reset_delta_state(&client.conn);
  }

  if (oldstate == newstate) {
    return;
  }

  if (oldstate == C_S_RUNNING && newstate != C_S_PREPARING) {
    stop_style_music();

    if (!is_client_quitting()) {
      /* Back to menu */
      start_menu_music("music_menu", NULL);
    }
  }

  civclient_state = newstate;

  switch (newstate) {
  case C_S_INITIAL:
    qCritical("%d is not a valid client state to set.", C_S_INITIAL);
    break;

  case C_S_DISCONNECTED:
    popdown_all_city_dialogs();
    close_all_diplomacy_dialogs();
    popdown_all_game_dialogs();
    meswin_clear_older(MESWIN_CLEAR_ALL, 0);

    if (oldstate > C_S_DISCONNECTED) {
      unit_focus_set(NULL);
      editor_clear();
      global_worklists_unbuild();
      client_remove_all_cli_conn();
      client_game_free();
      if (oldstate > C_S_PREPARING) {
        options_dialogs_update();
      }
    }

    set_client_page(PAGE_MAIN);
    break;

  case C_S_PREPARING:
    popdown_all_city_dialogs();
    close_all_diplomacy_dialogs();
    popdown_all_game_dialogs();
    meswin_clear_older(MESWIN_CLEAR_ALL, 0);

    if (oldstate < C_S_PREPARING) {
      client_game_init();
    } else {
      /* From an upper state means that we didn't quit the server,
       * so a lot of informations are still in effect. */
      client_game_reset();
      options_dialogs_update();
    }

    unit_focus_set(NULL);

    if (get_client_page() != PAGE_SCENARIO
        && get_client_page() != PAGE_LOAD) {
      set_client_page(PAGE_START);
    }
    break;

  case C_S_RUNNING:
    if (oldstate == C_S_PREPARING) {
      popdown_races_dialog();
      stop_menu_music(); /* stop intro sound loop. */
    }

    init_city_report_game_data();
    options_dialogs_set();
    create_event(NULL, E_GAME_START, ftc_client, _("Game started."));
    if (pplayer) {
      research_update(research_get(pplayer));
    }
    role_unit_precalcs();
    boot_help_texts(); /* reboot with player */
    global_worklists_build();
    can_slide = FALSE;
    unit_focus_update();
    can_slide = TRUE;
    set_client_page(PAGE_GAME);
    /* Find something sensible to display instead of the intro gfx. */
    center_on_something();
    free_intro_radar_sprites();
    editgui_tileset_changed();
    voteinfo_gui_update();

    refresh_overview_canvas();

    update_info_label(); /* get initial population right */
    unit_focus_update();
    update_unit_info_label(get_units_in_focus());

    if (gui_options.auto_center_each_turn) {
      center_on_something();
    }
    start_style_music();
    break;

  case C_S_OVER:
    if (C_S_RUNNING == oldstate) {
      /* Extra kludge for end-game handling of the CMA. */
      if (pplayer && pplayer->cities) {
        city_list_iterate(pplayer->cities, pcity)
        {
          if (cma_is_city_under_agent(pcity, NULL)) {
            cma_release_city(pcity);
          }
        }
        city_list_iterate_end;
      }
      popdown_all_city_dialogs();
      close_all_diplomacy_dialogs();
      popdown_all_game_dialogs();
      unit_focus_set(NULL);
    } else {
      /* From C_S_PREPARING. */
      init_city_report_game_data();
      options_dialogs_set();
      if (pplayer) {
        research_update(research_get(pplayer));
      }
      role_unit_precalcs();
      boot_help_texts(); /* reboot */
      global_worklists_build();
      unit_focus_set(NULL);
      set_client_page(PAGE_GAME);
      center_on_something();
    }
    refresh_overview_canvas();

    update_info_label();
    unit_focus_update();
    update_unit_info_label(NULL);

    break;
  }

  menus_init();
  update_turn_done_button_state();
  conn_list_dialog_update();
  if (can_client_change_view()) {
    update_map_canvas_visible();
  }

  /* If turn was going to change, that is now aborted. */
  set_server_busy(FALSE);
}

/**********************************************************************/ /**
   Return current client state.
 **************************************************************************/
enum client_states client_state(void) { return civclient_state; }

/**********************************************************************/ /**
   Remove pconn from all connection lists in client, then free it.
 **************************************************************************/
void client_remove_cli_conn(struct connection *pconn)
{
  fc_assert_msg(pconn != NULL, "Trying to remove a non existing connection");

  if (NULL != pconn->playing) {
    conn_list_remove(pconn->playing->connections, pconn);
  }
  conn_list_remove(game.all_connections, pconn);
  conn_list_remove(game.est_connections, pconn);
  fc_assert_ret(pconn != &client.conn);
  delete pconn;
}

/**********************************************************************/ /**
   Remove (and free) all connections from all connection lists in client.
   Assumes game.all_connections is properly maintained with all connections.
 **************************************************************************/
void client_remove_all_cli_conn(void)
{
  fc_assert_msg(game.all_connections != NULL, "Connection list missing");

  while (conn_list_size(game.all_connections) > 0) {
    struct connection *pconn = conn_list_get(game.all_connections, 0);
    client_remove_cli_conn(pconn);
  }
}

/**********************************************************************/ /**
   Send attribute block.
 **************************************************************************/
void send_attribute_block_request(void)
{
  send_packet_player_attribute_block(&client.conn);
}

/**********************************************************************/ /**
   Returns whether client is observer.
 **************************************************************************/
bool client_is_observer(void)
{
  return client.conn.established && client.conn.observer;
}

/* Seconds_to_turndone is the number of seconds the server has told us
 * are left.  The timer tells exactly how much time has passed since the
 * server gave us that data. */
static int miliseconds_to_turndone = 0;
Q_GLOBAL_STATIC(QTimer, turndone_timer)

/* The timer tells how long since server informed us about starting
 * turn-change activities. */
Q_GLOBAL_STATIC(QElapsedTimer, between_turns)
static bool waiting_turn_change = FALSE;

/* This value shows what value the timeout label is currently showing for
 * the seconds-to-turndone. */
static int seconds_shown_to_turndone;
static int seconds_shown_to_new_turn;

/**********************************************************************/ /**
   Reset the number of seconds to turndone from an "authentic" source.

   The seconds are taken as a double even though most callers will just
   know an integer value.
 **************************************************************************/
void set_miliseconds_to_turndone(int miliseconds)
{
  if (current_turn_timeout() > 0) {
    miliseconds_to_turndone = miliseconds;
    turndone_timer->setSingleShot(true);
    turndone_timer->start(miliseconds);

    /* Maybe we should do an update_timeout_label here, but it doesn't
     * seem to be necessary. */
    seconds_shown_to_turndone = miliseconds / 1000;
  }
}

/**********************************************************************/ /**
   Are we in turn-change wait state?
 **************************************************************************/
bool is_waiting_turn_change(void) { return waiting_turn_change; }

/**********************************************************************/ /**
   Start waiting of the server turn change activities.
 **************************************************************************/
void start_turn_change_wait(void)
{
  seconds_shown_to_new_turn = ceil(game.tinfo.last_turn_change_time) + 0.1;
  between_turns->start();

  waiting_turn_change = TRUE;
}

/**********************************************************************/ /**
   Server is responsive again
 **************************************************************************/
void stop_turn_change_wait(void)
{
  waiting_turn_change = FALSE;
  update_timeout_label();
}

/**********************************************************************/ /**
   Return the number of seconds until turn-done. Don't call this unless
   current_turn_timeout() != 0.
 **************************************************************************/
int get_seconds_to_turndone(void)
{
  if (current_turn_timeout() > 0) {
    return seconds_shown_to_turndone;
  } else {
    /* This shouldn't happen. */
    return FC_INFINITY;
  }
}

/**********************************************************************/ /**
   Return the number of seconds until turn-done.  Don't call this unless
   current_turn_timeout() != 0.
 **************************************************************************/
int get_seconds_to_new_turn(void) { return seconds_shown_to_new_turn; }

/**********************************************************************/ /**
   This function should be called at least once per second.  It does various
   updates (idle animations and timeout updates).  It returns the number of
   seconds until it should be called again.
 **************************************************************************/
double real_timer_callback(void)
{
  double time_until_next_call = 1.0;

  voteinfo_queue_check_removed();

  {
    double autoconnect_time = try_to_autoconnect();
    time_until_next_call = MIN(time_until_next_call, autoconnect_time);
  }

  if (C_S_RUNNING != client_state()) {
    return time_until_next_call;
  }

  {
    double blink_time = blink_turn_done_button();

    time_until_next_call = MIN(time_until_next_call, blink_time) / 1000;
  }

  if (get_num_units_in_focus() > 0) {
    double blink_time = blink_active_unit();

    time_until_next_call = MIN(time_until_next_call, blink_time) / 1000;
  }

  /* It is possible to have current_turn_timeout() > 0 but !turndone_timer,
   * in the first moments after the timeout is set. */
  if (current_turn_timeout() > 0 && turndone_timer) {

    if (turndone_timer->remainingTime() / 1000 < seconds_shown_to_turndone) {
      seconds_shown_to_turndone = turndone_timer->remainingTime() / 1000;
      update_timeout_label();
    }

    time_until_next_call = MIN(time_until_next_call, 1.0);
  }
  if (waiting_turn_change) {
    double seconds =
        game.tinfo.last_turn_change_time - between_turns->elapsed() / 1000;
    int iseconds = ceil(seconds) + 0.1; /* Turn should end right on 0. */

    if (iseconds < game.tinfo.last_turn_change_time) {
      seconds_shown_to_new_turn = iseconds;
      update_timeout_label();
    }
  }

  {
    static long counter = 0;

    counter++;

    if (gui_options.heartbeat_enabled && (counter % (20 * 10) == 0)) {
      send_packet_client_heartbeat(&client.conn);
    }
  }

  /* Make sure we wait at least 50 ms, otherwise we may not give any other
   * code time to run. */
  return MAX(time_until_next_call, 0.05);
}

/**********************************************************************/ /**
   Returns TRUE iff the client can control player.
 **************************************************************************/
bool can_client_control(void)
{
  return (NULL != client.conn.playing && !client_is_observer());
}

/**********************************************************************/ /**
   Returns TRUE iff the client can issue orders (such as giving unit
   commands).  This function should be called each time before allowing the
   user to give an order.
 **************************************************************************/
bool can_client_issue_orders(void)
{
  return (can_client_control() && C_S_RUNNING == client_state());
}

/**********************************************************************/ /**
   Returns TRUE iff the client can do diplomatic meetings with another
   given player.
 **************************************************************************/
bool can_meet_with_player(const struct player *pplayer)
{
  return (can_client_issue_orders()
          /* && NULL != client.conn.playing (above) */
          && could_meet_with_player(client.conn.playing, pplayer));
}

/**********************************************************************/ /**
   Returns TRUE iff the client can get intelligence from another
   given player.
 **************************************************************************/
bool can_intel_with_player(const struct player *pplayer)
{
  return (client_is_observer()
          || (NULL != client.conn.playing
              && could_intel_with_player(client.conn.playing, pplayer)));
}

/**********************************************************************/ /**
   Return TRUE if the client can change the view; i.e. if the mapview is
   active.  This function should be called each time before allowing the
   user to do mapview actions.
 **************************************************************************/
bool can_client_change_view(void)
{
  return ((NULL != client.conn.playing || client_is_observer())
          && (C_S_RUNNING == client_state() || C_S_OVER == client_state()));
}

/**********************************************************************/ /**
   Sets if server is considered busy. Currently it is considered busy
   between turns.
 **************************************************************************/
void set_server_busy(bool busy)
{
  if (busy != server_busy) {
    /* server_busy value will change */
    server_busy = busy;

    /* This may mean that we have to change from or to wait cursor */
    control_mouse_cursor(NULL);
  }
}

/**********************************************************************/ /**
   Returns if server is considered busy at the moment
 **************************************************************************/
bool is_server_busy(void) { return server_busy; }

/**********************************************************************/ /**
   Returns whether client is global observer
 **************************************************************************/
bool client_is_global_observer(void)
{
  return client.conn.playing == NULL && client.conn.observer == TRUE;
}

/**********************************************************************/ /**
   Returns number of player attached to client.
 **************************************************************************/
int client_player_number(void)
{
  if (client.conn.playing == NULL) {
    return -1;
  }
  return player_number(client.conn.playing);
}

/**********************************************************************/ /**
   Either controlling or observing.
 **************************************************************************/
bool client_has_player(void) { return client.conn.playing != NULL; }

/**********************************************************************/ /**
   Either controlling or observing.
 **************************************************************************/
struct player *client_player(void) { return client.conn.playing; }

/**********************************************************************/ /**
   Return the vision of the player on a tile. Client version of
   ./server/maphand/map_is_known_and_seen().
 **************************************************************************/
bool client_map_is_known_and_seen(const struct tile *ptile,
                                  const struct player *pplayer,
                                  enum vision_layer vlayer)
{
  return pplayer->client.tile_vision[vlayer]->at(tile_index(ptile));
}

/**********************************************************************/ /**
   Returns the id of the city the player believes exists at 'ptile'.
 **************************************************************************/
static int client_plr_tile_city_id_get(const struct tile *ptile,
                                       const struct player *pplayer)
{
  struct city *pcity = tile_city(ptile);

  /* Can't look up what other players think. */
  fc_assert(pplayer == client_player());

  return pcity ? pcity->id : IDENTITY_NUMBER_ZERO;
}

/**********************************************************************/ /**
   Returns the id of the server setting with the specified name.
 **************************************************************************/
static server_setting_id client_ss_by_name(const char *name)
{
  struct option *pset = optset_option_by_name(server_optset, name);

  if (pset) {
    return option_number(pset);
  } else {
    qCritical("No server setting named %s exists.", name);
    return SERVER_SETTING_NONE;
  }
}

/**********************************************************************/ /**
   Returns the name of the server setting with the specified id.
 **************************************************************************/
static const char *client_ss_name_get(server_setting_id id)
{
  struct option *pset = optset_option_by_number(server_optset, id);

  if (pset) {
    return option_name(pset);
  } else {
    qCritical("No server setting with the id %d exists.", id);
    return NULL;
  }
}

/**********************************************************************/ /**
   Returns the type of the server setting with the specified id.
 **************************************************************************/
static enum sset_type client_ss_type_get(server_setting_id id)
{
  enum option_type opt_type;
  struct option *pset = optset_option_by_number(server_optset, id);

  if (!pset) {
    qCritical("No server setting with the id %d exists.", id);
    return sset_type_invalid();
  }

  opt_type = option_type(pset);

  /* The option type isn't client only. */
  fc_assert_ret_val_msg((opt_type != OT_FONT && opt_type != OT_COLOR),
                        sset_type_invalid(),
                        "%s is a client option type but not a server "
                        "setting type",
                        option_type_name(option_type(pset)));

  /* The option type is valid. */
  fc_assert_ret_val(sset_type_is_valid((enum sset_type) opt_type),
                    sset_type_invalid());

  /* Each server setting type value equals the client option type value with
   * the same meaning. */
  FC_STATIC_ASSERT((enum sset_type) OT_BOOLEAN == SST_BOOL
                       && (enum sset_type) OT_INTEGER == SST_INT
                       && (enum sset_type) OT_STRING == SST_STRING
                       && (enum sset_type) OT_ENUM == SST_ENUM
                       && (enum sset_type) OT_BITWISE == SST_BITWISE
                       && SST_COUNT == (enum sset_type) 5,
                   server_setting_type_not_equal_to_client_option_type);

  /* Exploit the fact that each server setting type value corresponds to the
   * client option type value with the same meaning. */
  return (enum sset_type) opt_type;
}

/**********************************************************************/ /**
   Returns the value of the boolean server setting with the specified id.
 **************************************************************************/
static bool client_ss_val_bool_get(server_setting_id id)
{
  struct option *pset = optset_option_by_number(server_optset, id);

  if (pset) {
    return option_bool_get(pset);
  } else {
    qCritical("No server setting with the id %d exists.", id);
    return FALSE;
  }
}

/**********************************************************************/ /**
   Returns the value of the integer server setting with the specified id.
 **************************************************************************/
static int client_ss_val_int_get(server_setting_id id)
{
  struct option *pset = optset_option_by_number(server_optset, id);

  if (pset) {
    return option_int_get(pset);
  } else {
    qCritical("No server setting with the id %d exists.", id);
    return 0;
  }
}

/**********************************************************************/ /**
   Returns the value of the bitwise server setting with the specified id.
 **************************************************************************/
static unsigned int client_ss_val_bitwise_get(server_setting_id id)
{
  struct option *pset = optset_option_by_number(server_optset, id);

  if (pset) {
    return option_bitwise_get(pset);
  } else {
    qCritical("No server setting with the id %d exists.", id);
    return FALSE;
  }
}

/**********************************************************************/ /**
   Initialize client specific functions.
 **************************************************************************/
static void fc_interface_init_client(void)
{
  struct functions *funcs = fc_interface_funcs();

  funcs->server_setting_by_name = client_ss_by_name;
  funcs->server_setting_name_get = client_ss_name_get;
  funcs->server_setting_type_get = client_ss_type_get;
  funcs->server_setting_val_bool_get = client_ss_val_bool_get;
  funcs->server_setting_val_int_get = client_ss_val_int_get;
  funcs->server_setting_val_bitwise_get = client_ss_val_bitwise_get;
  funcs->create_extra = NULL;
  funcs->destroy_extra = NULL;
  funcs->player_tile_vision_get = client_map_is_known_and_seen;
  funcs->player_tile_city_id_get = client_plr_tile_city_id_get;
  funcs->gui_color_free = color_free;

  /* Keep this function call at the end. It checks if all required functions
     are defined. */
  fc_interface_init();
}

/**********************************************************************/ /**
   Helper function for the mapimg module - tile knowledge.
 **************************************************************************/
static enum known_type mapimg_client_tile_known(const struct tile *ptile,
                                                const struct player *pplayer,
                                                bool knowledge)
{
  if (client_is_global_observer()) {
    return TILE_KNOWN_SEEN;
  }

  return tile_get_known(ptile, pplayer);
}

/**********************************************************************/ /**
   Helper function for the mapimg module - tile terrain.
 **************************************************************************/
static struct terrain *
mapimg_client_tile_terrain(const struct tile *ptile,
                           const struct player *pplayer, bool knowledge)
{
  return tile_terrain(ptile);
}

/**********************************************************************/ /**
   Helper function for the mapimg module - tile owner.
 **************************************************************************/
static struct player *mapimg_client_tile_owner(const struct tile *ptile,
                                               const struct player *pplayer,
                                               bool knowledge)
{
  return tile_owner(ptile);
}

/**********************************************************************/ /**
   Helper function for the mapimg module - city owner.
 **************************************************************************/
static struct player *mapimg_client_tile_city(const struct tile *ptile,
                                              const struct player *pplayer,
                                              bool knowledge)
{
  struct city *pcity = tile_city(ptile);

  if (!pcity) {
    return NULL;
  }

  return city_owner(tile_city(ptile));
}

/**********************************************************************/ /**
   Helper function for the mapimg module - unit owner.
 **************************************************************************/
static struct player *mapimg_client_tile_unit(const struct tile *ptile,
                                              const struct player *pplayer,
                                              bool knowledge)
{
  int unit_count = unit_list_size(ptile->units);

  if (unit_count == 0) {
    return NULL;
  }

  return unit_owner(unit_list_get(ptile->units, 0));
}

/**********************************************************************/ /**
   Helper function for the mapimg module - number of player colors.
 **************************************************************************/
static int mapimg_client_plrcolor_count(void) { return player_count(); }

/**********************************************************************/ /**
   Helper function for the mapimg module - one player color. For the client
   only the colors of the defined players are shown.
 **************************************************************************/
static struct rgbcolor *mapimg_client_plrcolor_get(int i)
{
  int count = 0;

  if (0 > i || i > player_count()) {
    return NULL;
  }

  players_iterate(pplayer)
  {
    if (count == i) {
      return pplayer->rgb;
    }
    count++;
  }
  players_iterate_end;

  return NULL;
}

/**********************************************************************/ /**
   Is the client marked as one going down?
 **************************************************************************/
bool is_client_quitting(void) { return client_quitting; }

/**********************************************************************/ /**
   Mark client as one going to quit as soon as possible,
 **************************************************************************/
void start_quitting(void) { client_quitting = TRUE; }
