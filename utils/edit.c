/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 *
 * $Id: edit.c 13625 2012-12-05 17:29:46Z jordan $
 */

#include <stdio.h> /* fprintf () */
#include <string.h> /* strlen (), strstr (), strcmp () */
#include <stdlib.h> /* EXIT_FAILURE */

#include <event2/buffer.h>

#include <libtransmission/transmission.h>
#include <libtransmission/bencode.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/version.h>

#define MY_NAME "transmission-edit"

static int fileCount = 0;
static bool showVersion = false;
static const char ** files = NULL;
static const char * add = NULL;
static const char * deleteme = NULL;
static const char * replace[2] = { NULL, NULL };

static tr_option options[] =
{
  { 'a', "add", "Add a tracker's announce URL", "a", 1, "<url>" },
  { 'd', "delete", "Delete a tracker's announce URL", "d", 1, "<url>" },
  { 'r', "replace", "Search and replace a substring in the announce URLs", "r", 1, "<old> <new>" },
  { 'V', "version", "Show version number and exit", "V", 0, NULL },
  { 0, NULL, NULL, NULL, 0, NULL }
};

static const char *
getUsage (void)
{
  return "Usage: " MY_NAME " [options] torrent-file (s)";
}

static int
parseCommandLine (int argc, const char ** argv)
{
  int c;
  const char * optarg;

  while ((c = tr_getopt (getUsage (), argc, argv, options, &optarg)))
    {
      switch (c)
        {
          case 'a':
            add = optarg;
            break;

          case 'd':
            deleteme = optarg;
            break;

          case 'r':
            replace[0] = optarg;
            c = tr_getopt (getUsage (), argc, argv, options, &optarg);
            if (c != TR_OPT_UNK)
              return 1;
            replace[1] = optarg;
            break;

          case 'V':
            showVersion = true;
            break;

          case TR_OPT_UNK:
            files[fileCount++] = optarg;
            break;

          default:
            return 1;
        }
    }

  return 0;
}

static bool
removeURL (tr_benc * metainfo, const char * url)
{
  const char * str;
  tr_benc * announce_list;
  bool changed = false;

  if (tr_bencDictFindStr (metainfo, "announce", &str) && !strcmp (str, url))
    {
      printf ("\tRemoved \"%s\" from \"announce\"\n", str);
      tr_bencDictRemove (metainfo, "announce");
      changed = true;
    }

  if (tr_bencDictFindList (metainfo, "announce-list", &announce_list))
    {
      tr_benc * tier;
      int tierIndex = 0;
      while ((tier = tr_bencListChild (announce_list, tierIndex)))
        {
          tr_benc * node;
          int nodeIndex = 0;
          while ((node = tr_bencListChild (tier, nodeIndex)))
            {
              if (tr_bencGetStr (node, &str) && !strcmp (str, url))
                {
                  printf ("\tRemoved \"%s\" from \"announce-list\" tier #%d\n", str, (tierIndex+1));
                  tr_bencListRemove (tier, nodeIndex);
                  changed = true;
                }
              else ++nodeIndex;
            }

          if (tr_bencListSize (tier) == 0)
            {
              printf ("\tNo URLs left in tier #%d... removing tier\n", (tierIndex+1));
              tr_bencListRemove (announce_list, tierIndex);
            }
          else
            {
              ++tierIndex;
            }
        }

      if (tr_bencListSize (announce_list) == 0)
        {
          printf ("\tNo tiers left... removing announce-list\n");
          tr_bencDictRemove (metainfo, "announce-list");
        }
    }

  /* if we removed the "announce" field and there's still another track left,
   * use it as the "announce" field */
  if (changed && !tr_bencDictFindStr (metainfo, "announce", &str))
    {
      tr_benc * tier;
      tr_benc * node;

      if ((tier = tr_bencListChild (announce_list, 0)))
        {
          if ((node = tr_bencListChild (tier, 0)))
            {
              if (tr_bencGetStr (node, &str))
                {
                  tr_bencDictAddStr (metainfo, "announce", str);
                  printf ("\tAdded \"%s\" to announce\n", str);
                }
            }
        }
    }

  return changed;
}

static char*
replaceSubstr (const char * str, const char * in, const char * out)
{
  char * walk;
  struct evbuffer * buf = evbuffer_new ();
  const size_t inlen = strlen (in);
  const size_t outlen = strlen (out);

  while ((walk = strstr (str, in)))
    {
      evbuffer_add (buf, str, walk-str);
      evbuffer_add (buf, out, outlen);
      str = walk + inlen;
    }

  evbuffer_add (buf, str, strlen (str));

  return evbuffer_free_to_str (buf);
}

static bool
replaceURL (tr_benc * metainfo, const char * in, const char * out)
{
  const char * str;
  tr_benc * announce_list;
  bool changed = false;

  if (tr_bencDictFindStr (metainfo, "announce", &str) && strstr (str, in))
    {
      char * newstr = replaceSubstr (str, in, out);
      printf ("\tReplaced in \"announce\": \"%s\" --> \"%s\"\n", str, newstr);
      tr_bencDictAddStr (metainfo, "announce", newstr);
      tr_free (newstr);
      changed = true;
    }

  if (tr_bencDictFindList (metainfo, "announce-list", &announce_list))
    {
      tr_benc * tier;
      int tierCount = 0;
      while ((tier = tr_bencListChild (announce_list, tierCount++)))
        {
          tr_benc * node;
          int nodeCount = 0;
          while ((node = tr_bencListChild (tier, nodeCount++)))
            {
              if (tr_bencGetStr (node, &str) && strstr (str, in))
                {
                  char * newstr = replaceSubstr (str, in, out);
                  printf ("\tReplaced in \"announce-list\" tier %d: \"%s\" --> \"%s\"\n", tierCount, str, newstr);
                  tr_bencFree (node);
                  tr_bencInitStr (node, newstr, -1);
                  tr_free (newstr);
                  changed = true;
                }
            }
        }
    }

  return changed;
}

static bool
announce_list_has_url (tr_benc * announce_list, const char * url)
{
  tr_benc * tier;
  int tierCount = 0;
  while ((tier = tr_bencListChild (announce_list, tierCount++)))
    {
      tr_benc * node;
      const char * str;
      int nodeCount = 0;
      while ((node = tr_bencListChild (tier, nodeCount++)))
        if (tr_bencGetStr (node, &str) && !strcmp (str, url))
          return true;
    }

  return false;
}

static bool
addURL (tr_benc * metainfo, const char * url)
{
  const char * announce = NULL;
  tr_benc * announce_list = NULL;
  bool changed = false;
  const bool had_announce = tr_bencDictFindStr (metainfo, "announce", &announce);
  const bool had_announce_list = tr_bencDictFindList (metainfo, "announce-list", &announce_list);

  if (!had_announce && !had_announce_list)
    {
      /* this new tracker is the only one, so add it to "announce"... */
      printf ("\tAdded \"%s\" in \"announce\"\n", url);
      tr_bencDictAddStr (metainfo, "announce", url);
      changed = true;
    }
  else
    {
      if (!had_announce_list)
        {
          announce_list = tr_bencDictAddList (metainfo, "announce-list", 2);

          if (had_announce)
            {
              /* we're moving from an 'announce' to an 'announce-list',
               * so copy the old announce URL to the list */
              tr_benc * tier = tr_bencListAddList (announce_list, 1);
              tr_bencListAddStr (tier, announce);
              changed = true;
            }
        }

      /* If the user-specified URL isn't in the announce list yet, add it */
      if (!announce_list_has_url (announce_list, url))
        {
          tr_benc * tier = tr_bencListAddList (announce_list, 1);
          tr_bencListAddStr (tier, url);
          printf ("\tAdded \"%s\" to \"announce-list\" tier %zu\n", url, tr_bencListSize (announce_list));
          changed = true;
        }
    }

  return changed;
}

int
main (int argc, char * argv[])
{
  int i;
  int changedCount = 0;

  files = tr_new0 (const char*, argc);

  tr_setMessageLevel (TR_MSG_ERR);

  if (parseCommandLine (argc, (const char**)argv))
    return EXIT_FAILURE;

  if (showVersion)
    {
      fprintf (stderr, MY_NAME" "LONG_VERSION_STRING"\n");
      return EXIT_SUCCESS;
    }

  if (fileCount < 1)
    {
      fprintf (stderr, "ERROR: No torrent files specified.\n");
      tr_getopt_usage (MY_NAME, getUsage (), options);
      fprintf (stderr, "\n");
      return EXIT_FAILURE;
    }

  if (!add && !deleteme && !replace[0])
    {
      fprintf (stderr, "ERROR: Must specify -a, -d or -r\n");
      tr_getopt_usage (MY_NAME, getUsage (), options);
      fprintf (stderr, "\n");
      return EXIT_FAILURE;
    }

  for (i=0; i<fileCount; ++i)
    {
      tr_benc top;
      bool changed = false;
      const char * filename = files[i];

      printf ("%s\n", filename);

      if (tr_bencLoadFile (&top, TR_FMT_BENC, filename))
        {
          printf ("\tError reading file\n");
          continue;
        }

      if (deleteme != NULL)
        changed |= removeURL (&top, deleteme);

      if (add != NULL)
        changed = addURL (&top, add);

      if (replace[0] && replace[1])
        changed |= replaceURL (&top, replace[0], replace[1]);

      if (changed)
        {
          ++changedCount;
          tr_bencToFile (&top, TR_FMT_BENC, filename);
        }

      tr_bencFree (&top);
    }

  printf ("Changed %d files\n", changedCount);

  tr_free (files);
  return EXIT_SUCCESS;
}
