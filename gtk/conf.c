/******************************************************************************
 * $Id: conf.c 13638 2012-12-09 21:28:20Z jordan $
 *
 * Copyright (c) Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h> /* strtol () */
#include <string.h>

#include <unistd.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <libtransmission/transmission.h>
#include <libtransmission/bencode.h>

#include "conf.h"
#include "tr-prefs.h"
#include "util.h"

#define MY_CONFIG_NAME "transmission"
#define MY_READABLE_NAME "transmission-gtk"

static char * gl_confdir = NULL;

void
gtr_pref_init (const char * config_dir)
{
  gl_confdir = g_strdup (config_dir);
}

/***
****
****  Preferences
****
***/

static void cf_check_older_configs (void);

/**
 * This is where we initialize the preferences file with the default values.
 * If you add a new preferences key, you /must/ add a default value here.
 */
static void
tr_prefs_init_defaults (tr_benc * d)
{
  const char * str;
  const char * special_dl_dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);

  cf_check_older_configs ();

  str = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
  if (!str)
    str = g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP);
  if (!str)
    str = tr_getDefaultDownloadDir ();

  tr_bencDictReserve (d, 29);

  tr_bencDictAddStr (d, PREF_KEY_DIR_WATCH, str);
  tr_bencDictAddBool (d, PREF_KEY_DIR_WATCH_ENABLED, FALSE);

  tr_bencDictAddBool (d, PREF_KEY_USER_HAS_GIVEN_INFORMED_CONSENT, FALSE);
  tr_bencDictAddBool (d, PREF_KEY_INHIBIT_HIBERNATION, FALSE);
  tr_bencDictAddBool (d, PREF_KEY_BLOCKLIST_UPDATES_ENABLED, TRUE);

  tr_bencDictAddStr (d, PREF_KEY_OPEN_DIALOG_FOLDER, g_get_home_dir ());

  tr_bencDictAddBool (d, PREF_KEY_TOOLBAR, TRUE);
  tr_bencDictAddBool (d, PREF_KEY_FILTERBAR, TRUE);
  tr_bencDictAddBool (d, PREF_KEY_STATUSBAR, TRUE);
  tr_bencDictAddBool (d, PREF_KEY_TRASH_CAN_ENABLED, TRUE);
  tr_bencDictAddBool (d, PREF_KEY_SHOW_TRAY_ICON, FALSE);
  tr_bencDictAddBool (d, PREF_KEY_SHOW_MORE_TRACKER_INFO, FALSE);
  tr_bencDictAddBool (d, PREF_KEY_SHOW_MORE_PEER_INFO, FALSE);
  tr_bencDictAddBool (d, PREF_KEY_SHOW_BACKUP_TRACKERS, FALSE);
  tr_bencDictAddStr (d, PREF_KEY_STATUSBAR_STATS, "total-ratio");

  tr_bencDictAddBool (d, PREF_KEY_TORRENT_ADDED_NOTIFICATION_ENABLED, true);
  tr_bencDictAddBool (d, PREF_KEY_TORRENT_COMPLETE_NOTIFICATION_ENABLED, true);
  tr_bencDictAddStr (d, PREF_KEY_TORRENT_COMPLETE_SOUND_COMMAND, "canberra-gtk-play -i complete-download -d 'transmission torrent downloaded'");
  tr_bencDictAddBool (d, PREF_KEY_TORRENT_COMPLETE_SOUND_ENABLED, true);

  tr_bencDictAddBool (d, PREF_KEY_OPTIONS_PROMPT, TRUE);

  tr_bencDictAddBool (d, PREF_KEY_MAIN_WINDOW_IS_MAXIMIZED, FALSE);
  tr_bencDictAddInt (d, PREF_KEY_MAIN_WINDOW_HEIGHT, 500);
  tr_bencDictAddInt (d, PREF_KEY_MAIN_WINDOW_WIDTH, 300);
  tr_bencDictAddInt (d, PREF_KEY_MAIN_WINDOW_X, 50);
  tr_bencDictAddInt (d, PREF_KEY_MAIN_WINDOW_Y, 50);

  tr_bencDictAddStr (d, TR_PREFS_KEY_DOWNLOAD_DIR, special_dl_dir ? special_dl_dir : str);

  tr_bencDictAddStr (d, PREF_KEY_SORT_MODE, "sort-by-name");
  tr_bencDictAddBool (d, PREF_KEY_SORT_REVERSED, FALSE);
  tr_bencDictAddBool (d, PREF_KEY_COMPACT_VIEW, FALSE);
}

static char*
getPrefsFilename (void)
{
  g_assert (gl_confdir != NULL);
  return g_build_filename (gl_confdir, "settings.json", NULL);
}

static tr_benc*
getPrefs (void)
{
  static tr_benc settings;
  static gboolean loaded = FALSE;

  if (!loaded)
    {
      tr_bencInitDict (&settings, 0);
      tr_prefs_init_defaults (&settings);
      tr_sessionLoadSettings (&settings, gl_confdir, MY_CONFIG_NAME);
      loaded = TRUE;
    }

  return &settings;
}

/***
****
***/

tr_benc*
gtr_pref_get_all (void)
{
  return getPrefs ();
}

int64_t
gtr_pref_int_get (const char * key)
{
  int64_t i = 0;

  tr_bencDictFindInt (getPrefs (), key, &i);

  return i;
}

void
gtr_pref_int_set (const char * key, int64_t value)
{
  tr_bencDictAddInt (getPrefs (), key, value);
}

double
gtr_pref_double_get (const char * key)
{
  double d = 0.0;

  tr_bencDictFindReal (getPrefs (), key, &d);

  return d;
}

void
gtr_pref_double_set (const char * key, double value)
{
  tr_bencDictAddReal (getPrefs (), key, value);
}

/***
****
***/

gboolean
gtr_pref_flag_get (const char * key)
{
  bool boolVal;

  tr_bencDictFindBool (getPrefs (), key, &boolVal);

  return boolVal != 0;
}

void
gtr_pref_flag_set (const char * key, gboolean value)
{
  tr_bencDictAddBool (getPrefs (), key, value);
}

/***
****
***/

const char*
gtr_pref_string_get (const char * key)
{
  const char * str = NULL;

  tr_bencDictFindStr (getPrefs (), key, &str);

  return str;
}

void
gtr_pref_string_set (const char * key, const char * value)
{
  tr_bencDictAddStr (getPrefs (), key, value);
}

/***
****
***/

void
gtr_pref_save (tr_session * session)
{
  tr_sessionSaveSettings (session, gl_confdir, getPrefs ());
}

/***
****
***/

static char*
getCompat090PrefsFilename (void)
{
  g_assert (gl_confdir != NULL);

  return g_build_filename (g_get_home_dir (), ".transmission", "gtk", "prefs.ini", NULL);
}

static char*
getCompat121PrefsFilename (void)
{
  return g_build_filename (g_get_user_config_dir (), "transmission", "gtk", "prefs.ini", NULL);
}

static void
translate_keyfile_to_json (const char * old_file, const char * new_file)
{
  tr_benc    dict;
  GKeyFile * keyfile;
  gchar **   keys;
  gsize      i;
  gsize      length;

  static struct pref_entry {
    const char*   oldkey;
    const char*   newkey;
  } renamed[] = {
    { "default-download-directory", "download-dir"             },
    { "encrypted-connections-only", "encryption"               },
    { "listening-port",             "peer-port"                },
    { "nat-traversal-enabled",      "port-forwarding-enabled"  },
    { "open-dialog-folder",         "open-dialog-dir"          },
    { "watch-folder",               "watch-dir"                },
    { "watch-folder-enabled",       "watch-dir-enabled"        }
  };

  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, old_file, 0, NULL);
  length = 0;
  keys = g_key_file_get_keys (keyfile, "general", &length, NULL);

  tr_bencInitDict (&dict, length);
  for (i=0; i<length; i++)
    {
      guint j;
      const char * key = keys[i];
      gchar * val = g_key_file_get_value (keyfile, "general", key, NULL);

      for (j=0; j<G_N_ELEMENTS (renamed); j++)
        if (!strcmp (renamed[j].oldkey, key))
          key = renamed[j].newkey;

      if (!strcmp (val, "true") || !strcmp (val, "false"))
        {
          tr_bencDictAddInt (&dict, key, !strcmp (val, "true"));
        }
      else
        {
          char * end;
          long l;

          errno = 0;

          l = strtol (val, &end, 10);
          if (!errno && end && !*end)
            tr_bencDictAddInt (&dict, key, l);
          else
            tr_bencDictAddStr (&dict, key, val);
        }

      g_free (val);
    }

  g_key_file_free (keyfile);
  tr_bencToFile (&dict, TR_FMT_JSON, new_file);
  tr_bencFree (&dict);
}

static void
cf_check_older_configs (void)
{
  char * filename = getPrefsFilename ();

  if (!g_file_test (filename, G_FILE_TEST_IS_REGULAR))
    {
      char * key1 = getCompat121PrefsFilename ();
      char * key2 = getCompat090PrefsFilename ();

      if (g_file_test (key1, G_FILE_TEST_IS_REGULAR))
        {
          g_message (_("Importing \"%s\""), key1);
          translate_keyfile_to_json (key1, filename);
        }
      else if (g_file_test (key2, G_FILE_TEST_IS_REGULAR))
        {
          g_message (_("Importing \"%s\""), key2);
          translate_keyfile_to_json (key2, filename);
        }

      g_free (key2);
      g_free (key1);
    }

  g_free (filename);
}
