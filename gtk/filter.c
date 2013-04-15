/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2 (b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: filter.c 13625 2012-12-05 17:29:46Z jordan $
 */

#include <stdlib.h> /* qsort () */

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>

#include "favicon.h" /* gtr_get_favicon () */
#include "filter.h"
#include "hig.h" /* GUI_PAD */
#include "tr-core.h" /* MC_TORRENT */
#include "util.h" /* gtr_get_host_from_url () */

static GQuark DIRTY_KEY = 0;
static GQuark SESSION_KEY = 0;
static GQuark TEXT_KEY = 0;
static GQuark TORRENT_MODEL_KEY = 0;

/***
****
****  CATEGORIES
****
***/

enum
{
    CAT_FILTER_TYPE_ALL,
    CAT_FILTER_TYPE_PRIVATE,
    CAT_FILTER_TYPE_PUBLIC,
    CAT_FILTER_TYPE_HOST,
    CAT_FILTER_TYPE_PARENT,
    CAT_FILTER_TYPE_PRI_HIGH,
    CAT_FILTER_TYPE_PRI_NORMAL,
    CAT_FILTER_TYPE_PRI_LOW,
    CAT_FILTER_TYPE_TAG,
    CAT_FILTER_TYPE_SEPARATOR,
};

enum
{
    CAT_FILTER_COL_NAME, /* human-readable name; ie, Legaltorrents */
    CAT_FILTER_COL_COUNT, /* how many matches there are */
    CAT_FILTER_COL_TYPE,
    CAT_FILTER_COL_HOST, /* pattern-matching text; ie, legaltorrents.com */
    CAT_FILTER_COL_PIXBUF,
    CAT_FILTER_N_COLS
};

static int
pstrcmp (const void * a, const void * b)
{
    return strcmp (* (const char**)a, * (const char**)b);
}

/* human-readable name; ie, Legaltorrents */
static char*
get_name_from_host (const char * host)
{
    char * name;
    const char * dot = strrchr (host, '.');

    if (tr_addressIsIP (host))
        name = g_strdup (host);
    else if (dot)
        name = g_strndup (host, dot - host);
    else
        name = g_strdup (host);

    *name = g_ascii_toupper (*name);

    return name;
}

static void
category_model_update_count (GtkTreeStore * store, GtkTreeIter * iter, int n)
{
    int count;
    GtkTreeModel * model = GTK_TREE_MODEL (store);
    gtk_tree_model_get (model, iter, CAT_FILTER_COL_COUNT, &count, -1);
    if (n != count)
        gtk_tree_store_set (store, iter, CAT_FILTER_COL_COUNT, n, -1);
}

static void
favicon_ready_cb (gpointer pixbuf, gpointer vreference)
{
    GtkTreeIter iter;
    GtkTreeRowReference * reference = vreference;

    if (pixbuf != NULL)
    {
        GtkTreePath * path = gtk_tree_row_reference_get_path (reference);
        GtkTreeModel * model = gtk_tree_row_reference_get_model (reference);

        if (gtk_tree_model_get_iter (model, &iter, path))
            gtk_tree_store_set (GTK_TREE_STORE (model), &iter,
                                CAT_FILTER_COL_PIXBUF, pixbuf,
                                -1);

        gtk_tree_path_free (path);

        g_object_unref (pixbuf);
    }

    gtk_tree_row_reference_free (reference);
}

static gboolean
category_filter_model_update (GtkTreeStore * store)
{
    int i, n;
    int low = 0;
    int all = 0;
    int high = 0;
    int public = 0;
    int normal = 0;
    int private = 0;
    int store_pos;
    GtkTreeIter top;
    GtkTreeIter iter;
    GtkTreeModel * model = GTK_TREE_MODEL (store);
    GPtrArray * hosts = g_ptr_array_new ();
    GStringChunk * strings = g_string_chunk_new (4096);
    GHashTable * hosts_hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
    GObject * o = G_OBJECT (store);
    GtkTreeModel * tmodel = GTK_TREE_MODEL (g_object_get_qdata (o, TORRENT_MODEL_KEY));

    g_object_steal_qdata (o, DIRTY_KEY);

    /* Walk through all the torrents, tallying how many matches there are
     * for the various categories. Also make a sorted list of all tracker
     * hosts s.t. we can merge it with the existing list */
    if (gtk_tree_model_iter_nth_child (tmodel, &iter, NULL, 0)) do
    {
        tr_torrent * tor;
        const tr_info * inf;
        int keyCount;
        char ** keys;

        gtk_tree_model_get (tmodel, &iter, MC_TORRENT, &tor, -1);
        inf = tr_torrentInfo (tor);
        keyCount = 0;
        keys = g_new (char*, inf->trackerCount);

        for (i=0, n=inf->trackerCount; i<n; ++i)
        {
            int k;
            int * count;
            char buf[1024];
            char * key;

            gtr_get_host_from_url (buf, sizeof (buf), inf->trackers[i].announce);
            key = g_string_chunk_insert_const (strings, buf);

            count = g_hash_table_lookup (hosts_hash, key);
            if (count == NULL)
            {
                count = tr_new0 (int, 1);
                g_hash_table_insert (hosts_hash, key, count);
                g_ptr_array_add (hosts, key);
            }

            for (k=0; k<keyCount; ++k)
                if (!strcmp (keys[k], key))
                    break;
            if (k==keyCount)
                keys[keyCount++] = key;
        }

        for (i=0; i<keyCount; ++i)
        {
            int * incrementme = g_hash_table_lookup (hosts_hash, keys[i]);
            ++*incrementme;
        }
        g_free (keys);

        ++all;

        if (inf->isPrivate)
            ++private;
        else
            ++public;

        switch (tr_torrentGetPriority (tor))
        {
            case TR_PRI_HIGH: ++high; break;
            case TR_PRI_LOW: ++low; break;
            default: ++normal; break;
        }
    }
    while (gtk_tree_model_iter_next (tmodel, &iter));
    qsort (hosts->pdata, hosts->len, sizeof (char*), pstrcmp);

    /* update the "all" count */
    gtk_tree_model_iter_children (model, &top, NULL);
    category_model_update_count (store, &top, all);

    /* skip separator */
    gtk_tree_model_iter_next (model, &top);

    /* update the "hosts" subtree */
    gtk_tree_model_iter_next (model, &top);
    for (i=store_pos=0, n=hosts->len ; ;)
    {
        const gboolean new_hosts_done = i >= n;
        const gboolean old_hosts_done = !gtk_tree_model_iter_nth_child (model, &iter, &top, store_pos);
        gboolean remove_row = FALSE;
        gboolean insert_row = FALSE;

        /* are we done yet? */
        if (new_hosts_done && old_hosts_done)
            break;

        /* decide what to do */
        if (new_hosts_done)
            remove_row = TRUE;
        else if (old_hosts_done)
            insert_row = TRUE;
        else {
            int cmp;
            char * host;
            gtk_tree_model_get (model, &iter, CAT_FILTER_COL_HOST, &host,  -1);
            cmp = strcmp (host, hosts->pdata[i]);
            if (cmp < 0)
                remove_row = TRUE;
            else if (cmp > 0)
                insert_row = TRUE;
            g_free (host);
        }

        /* do something */
        if (remove_row) {
            /* g_message ("removing row and incrementing i"); */
            gtk_tree_store_remove (store, &iter);
        } else if (insert_row) {
            GtkTreeIter add;
            GtkTreePath * path;
            GtkTreeRowReference * reference;
            tr_session * session = g_object_get_qdata (G_OBJECT (store), SESSION_KEY);
            const char * host = hosts->pdata[i];
            char * name = get_name_from_host (host);
            const int count = * (int*)g_hash_table_lookup (hosts_hash, host);
            gtk_tree_store_insert_with_values (store, &add, &top, store_pos,
                CAT_FILTER_COL_HOST, host,
                CAT_FILTER_COL_NAME, name,
                CAT_FILTER_COL_COUNT, count,
                CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_HOST,
                -1);
            path = gtk_tree_model_get_path (model, &add);
            reference = gtk_tree_row_reference_new (model, path);
            gtr_get_favicon (session, host, favicon_ready_cb, reference);
            gtk_tree_path_free (path);
            g_free (name);
            ++store_pos;
            ++i;
        } else { /* update row */
            const char * host = hosts->pdata[i];
            const int count = * (int*)g_hash_table_lookup (hosts_hash, host);
            category_model_update_count (store, &iter, count);
            ++store_pos;
            ++i;
        }
    }

    /* update the "public" subtree */
    gtk_tree_model_iter_next (model, &top);
    gtk_tree_model_iter_children (model, &iter, &top);
    category_model_update_count (store, &iter, public);
    gtk_tree_model_iter_next (model, &iter);
    category_model_update_count (store, &iter, private);

    /* update the "priority" subtree */
    gtk_tree_model_iter_next (model, &top);
    gtk_tree_model_iter_children (model, &iter, &top);
    category_model_update_count (store, &iter, high);
    gtk_tree_model_iter_next (model, &iter);
    category_model_update_count (store, &iter, normal);
    gtk_tree_model_iter_next (model, &iter);
    category_model_update_count (store, &iter, low);

    /* cleanup */
    g_ptr_array_free (hosts, TRUE);
    g_hash_table_unref (hosts_hash);
    g_string_chunk_free (strings);
    return FALSE;
}

static GtkTreeModel *
category_filter_model_new (GtkTreeModel * tmodel)
{
    GtkTreeIter iter;
    const int invisible_number = -1; /* doesn't get rendered */
    GtkTreeStore * store = gtk_tree_store_new (CAT_FILTER_N_COLS,
                                               G_TYPE_STRING,
                                               G_TYPE_INT,
                                               G_TYPE_INT,
                                               G_TYPE_STRING,
                                               GDK_TYPE_PIXBUF);

    gtk_tree_store_insert_with_values (store, NULL, NULL, -1,
        CAT_FILTER_COL_NAME, _("All"),
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_ALL,
        -1);
    gtk_tree_store_insert_with_values (store, NULL, NULL, -1,
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_SEPARATOR,
        -1);

    gtk_tree_store_insert_with_values (store, &iter, NULL, -1,
        CAT_FILTER_COL_NAME, _("Trackers"),
        CAT_FILTER_COL_COUNT, invisible_number,
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PARENT,
        -1);

    gtk_tree_store_insert_with_values (store, &iter, NULL, -1,
        CAT_FILTER_COL_NAME, _("Privacy"),
        CAT_FILTER_COL_COUNT, invisible_number,
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PARENT,
        -1);
    gtk_tree_store_insert_with_values (store, NULL, &iter, -1,
        CAT_FILTER_COL_NAME, _("Public"),
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PUBLIC,
        -1);
    gtk_tree_store_insert_with_values (store, NULL, &iter, -1,
        CAT_FILTER_COL_NAME, _("Private"),
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PRIVATE,
        -1);

    gtk_tree_store_insert_with_values (store, &iter, NULL, -1,
        CAT_FILTER_COL_NAME, _("Priority"),
        CAT_FILTER_COL_COUNT, invisible_number,
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PARENT,
        -1);
    gtk_tree_store_insert_with_values (store, NULL, &iter, -1,
        CAT_FILTER_COL_NAME, _("High"),
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PRI_HIGH,
        -1);
    gtk_tree_store_insert_with_values (store, NULL, &iter, -1,
        CAT_FILTER_COL_NAME, _("Normal"),
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PRI_NORMAL,
        -1);
    gtk_tree_store_insert_with_values (store, NULL, &iter, -1,
        CAT_FILTER_COL_NAME, _("Low"),
        CAT_FILTER_COL_TYPE, CAT_FILTER_TYPE_PRI_LOW,
        -1);

    g_object_set_qdata (G_OBJECT (store), TORRENT_MODEL_KEY, tmodel);
    category_filter_model_update (store);
    return GTK_TREE_MODEL (store);
}

static gboolean
is_it_a_separator (GtkTreeModel * m, GtkTreeIter * iter, gpointer data UNUSED)
{
    int type;
    gtk_tree_model_get (m, iter, CAT_FILTER_COL_TYPE, &type, -1);
    return type == CAT_FILTER_TYPE_SEPARATOR;
}

static void
category_model_update_idle (gpointer category_model)
{
    GObject * o = G_OBJECT (category_model);
    const gboolean pending = g_object_get_qdata (o, DIRTY_KEY) != NULL;
    if (!pending)
    {
        GSourceFunc func = (GSourceFunc) category_filter_model_update;
        g_object_set_qdata (o, DIRTY_KEY, GINT_TO_POINTER (1));
        gdk_threads_add_idle (func, category_model);
    }
}

static void
torrent_model_row_changed (GtkTreeModel  * tmodel UNUSED,
                           GtkTreePath   * path UNUSED,
                           GtkTreeIter   * iter UNUSED,
                           gpointer        category_model)
{
    category_model_update_idle (category_model);
}

static void
torrent_model_row_deleted_cb (GtkTreeModel * tmodel UNUSED,
                              GtkTreePath  * path UNUSED,
                              gpointer       category_model)
{
    category_model_update_idle (category_model);
}

static void
render_pixbuf_func (GtkCellLayout    * cell_layout UNUSED,
                    GtkCellRenderer  * cell_renderer,
                    GtkTreeModel     * tree_model,
                    GtkTreeIter      * iter,
                    gpointer           data UNUSED)
{
    int type;
    int width = 0;
    const gboolean leaf = !gtk_tree_model_iter_has_child (tree_model, iter);

    gtk_tree_model_get (tree_model, iter, CAT_FILTER_COL_TYPE, &type, -1);
    if (type == CAT_FILTER_TYPE_HOST)
        width = 20;

    g_object_set (cell_renderer, "width", width,
                                 "sensitive", leaf,
                                 NULL);
}

static void
is_capital_sensitive (GtkCellLayout   * cell_layout UNUSED,
                      GtkCellRenderer * cell_renderer,
                      GtkTreeModel    * tree_model,
                      GtkTreeIter     * iter,
                      gpointer          data UNUSED)
{
    const gboolean leaf = !gtk_tree_model_iter_has_child (tree_model, iter);

    g_object_set (cell_renderer, "sensitive", leaf,
                                 NULL);
}

static void
render_number_func (GtkCellLayout    * cell_layout UNUSED,
                    GtkCellRenderer  * cell_renderer,
                    GtkTreeModel     * tree_model,
                    GtkTreeIter      * iter,
                    gpointer           data UNUSED)
{
    int count;
    char buf[32];
    const gboolean leaf = !gtk_tree_model_iter_has_child (tree_model, iter);

    gtk_tree_model_get (tree_model, iter, CAT_FILTER_COL_COUNT, &count, -1);

    if (count >= 0)
        g_snprintf (buf, sizeof (buf), "%'d", count);
    else
        *buf = '\0';

    g_object_set (cell_renderer, "text", buf,
                                 "sensitive", leaf,
                                 NULL);
}

static GtkCellRenderer *
number_renderer_new (void)
{
    GtkCellRenderer * r = gtk_cell_renderer_text_new ();

    g_object_set (G_OBJECT (r), "alignment", PANGO_ALIGN_RIGHT,
                                 "weight", PANGO_WEIGHT_ULTRALIGHT,
                                 "xalign", 1.0,
                                 "xpad", GUI_PAD,
                                 NULL);

    return r;
}

static void
disconnect_cat_model_callbacks (gpointer tmodel, GObject * cat_model)
{
    g_signal_handlers_disconnect_by_func (tmodel, torrent_model_row_changed, cat_model);
    g_signal_handlers_disconnect_by_func (tmodel, torrent_model_row_deleted_cb, cat_model);
}

static GtkWidget *
category_combo_box_new (GtkTreeModel * tmodel)
{
    GtkWidget * c;
    GtkCellRenderer * r;
    GtkTreeModel * cat_model;

    /* create the category combobox */
    cat_model = category_filter_model_new (tmodel);
    c = gtk_combo_box_new_with_model (cat_model);
    g_object_unref (cat_model);
    gtk_combo_box_set_row_separator_func (GTK_COMBO_BOX (c),
                                          is_it_a_separator, NULL, NULL);
    gtk_combo_box_set_active (GTK_COMBO_BOX (c), 0);

    r = gtk_cell_renderer_pixbuf_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (c), r, FALSE);
    gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (c), r,
                                        render_pixbuf_func, NULL, NULL);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (c), r,
                                    "pixbuf", CAT_FILTER_COL_PIXBUF,
                                    NULL);

    r = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (c), r, FALSE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (c), r,
                                    "text", CAT_FILTER_COL_NAME,
                                    NULL);
    gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (c), r,
                                        is_capital_sensitive,
                                        NULL, NULL);


    r = number_renderer_new ();
    gtk_cell_layout_pack_end (GTK_CELL_LAYOUT (c), r, TRUE);
    gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (c), r,
                                        render_number_func, NULL, NULL);

    g_object_weak_ref (G_OBJECT (cat_model), disconnect_cat_model_callbacks, tmodel);
    g_signal_connect (tmodel, "row-changed", G_CALLBACK (torrent_model_row_changed), cat_model);
    g_signal_connect (tmodel, "row-inserted", G_CALLBACK (torrent_model_row_changed), cat_model);
    g_signal_connect (tmodel, "row-deleted", G_CALLBACK (torrent_model_row_deleted_cb), cat_model);

    return c;
}

static gboolean
test_category (tr_torrent * tor, int active_category_type, const char * host)
{
    const tr_info * const inf = tr_torrentInfo (tor);

    switch (active_category_type)
    {
        case CAT_FILTER_TYPE_ALL:
            return TRUE;

        case CAT_FILTER_TYPE_PRIVATE:
            return inf->isPrivate;

        case CAT_FILTER_TYPE_PUBLIC:
            return !inf->isPrivate;

        case CAT_FILTER_TYPE_PRI_HIGH:
            return tr_torrentGetPriority (tor) == TR_PRI_HIGH;

        case CAT_FILTER_TYPE_PRI_NORMAL:
            return tr_torrentGetPriority (tor) == TR_PRI_NORMAL;

        case CAT_FILTER_TYPE_PRI_LOW:
            return tr_torrentGetPriority (tor) == TR_PRI_LOW;

        case CAT_FILTER_TYPE_HOST: {
            int i;
            char tmp[1024];
            for (i=0; i<inf->trackerCount; ++i) {
                gtr_get_host_from_url (tmp, sizeof (tmp), inf->trackers[i].announce);
                if (!strcmp (tmp, host))
                    break;
            }
            return i < inf->trackerCount;
        }

        case CAT_FILTER_TYPE_TAG:
            /* FIXME */
            return TRUE;

        default:
            return TRUE;
    }
}

/***
****
****  ACTIVITY
****
***/

enum
{
    ACTIVITY_FILTER_ALL,
    ACTIVITY_FILTER_DOWNLOADING,
    ACTIVITY_FILTER_SEEDING,
    ACTIVITY_FILTER_ACTIVE,
    ACTIVITY_FILTER_PAUSED,
    ACTIVITY_FILTER_FINISHED,
    ACTIVITY_FILTER_VERIFYING,
    ACTIVITY_FILTER_ERROR,
    ACTIVITY_FILTER_SEPARATOR
};

enum
{
    ACTIVITY_FILTER_COL_NAME,
    ACTIVITY_FILTER_COL_COUNT,
    ACTIVITY_FILTER_COL_TYPE,
    ACTIVITY_FILTER_COL_STOCK_ID,
    ACTIVITY_FILTER_N_COLS
};

static gboolean
activity_is_it_a_separator (GtkTreeModel * m, GtkTreeIter * i, gpointer d UNUSED)
{
    int type;
    gtk_tree_model_get (m, i, ACTIVITY_FILTER_COL_TYPE, &type, -1);
    return type == ACTIVITY_FILTER_SEPARATOR;
}

static gboolean
test_torrent_activity (tr_torrent * tor, int type)
{
    const tr_stat * st = tr_torrentStatCached (tor);

    switch (type)
    {
        case ACTIVITY_FILTER_DOWNLOADING:
            return (st->activity == TR_STATUS_DOWNLOAD)
                || (st->activity == TR_STATUS_DOWNLOAD_WAIT);

        case ACTIVITY_FILTER_SEEDING:
            return (st->activity == TR_STATUS_SEED)
                || (st->activity == TR_STATUS_SEED_WAIT);

        case ACTIVITY_FILTER_ACTIVE:
            return (st->peersSendingToUs > 0)
                || (st->peersGettingFromUs > 0)
                || (st->webseedsSendingToUs > 0)
                || (st->activity == TR_STATUS_CHECK);

        case ACTIVITY_FILTER_PAUSED:
            return st->activity == TR_STATUS_STOPPED;

        case ACTIVITY_FILTER_FINISHED:
            return st->finished == TRUE;

        case ACTIVITY_FILTER_VERIFYING:
            return (st->activity == TR_STATUS_CHECK)
                || (st->activity == TR_STATUS_CHECK_WAIT);

        case ACTIVITY_FILTER_ERROR:
            return st->error != 0;

        default: /* ACTIVITY_FILTER_ALL */
            return TRUE;
    }
}

static void
status_model_update_count (GtkListStore * store, GtkTreeIter * iter, int n)
{
    int count;
    GtkTreeModel * model = GTK_TREE_MODEL (store);
    gtk_tree_model_get (model, iter, ACTIVITY_FILTER_COL_COUNT, &count, -1);
    if (n != count)
        gtk_list_store_set (store, iter, ACTIVITY_FILTER_COL_COUNT, n, -1);
}

static void
activity_filter_model_update (GtkListStore * store)
{
    GtkTreeIter iter;
    GtkTreeModel * model = GTK_TREE_MODEL (store);
    GObject * o = G_OBJECT (store);
    GtkTreeModel * tmodel = GTK_TREE_MODEL (g_object_get_qdata (o, TORRENT_MODEL_KEY));

    g_object_steal_qdata (o, DIRTY_KEY);

    if (gtk_tree_model_iter_nth_child (model, &iter, NULL, 0)) do
    {
        int hits;
        int type;
        GtkTreeIter torrent_iter;

        gtk_tree_model_get (model, &iter, ACTIVITY_FILTER_COL_TYPE, &type, -1);

        hits = 0;
        if (gtk_tree_model_iter_nth_child (tmodel, &torrent_iter, NULL, 0)) do {
            tr_torrent * tor;
            gtk_tree_model_get (tmodel, &torrent_iter, MC_TORRENT, &tor, -1);
            if (test_torrent_activity (tor, type))
                ++hits;
        } while (gtk_tree_model_iter_next (tmodel, &torrent_iter));

        status_model_update_count (store, &iter, hits);

    } while (gtk_tree_model_iter_next (model, &iter));
}

static GtkTreeModel *
activity_filter_model_new (GtkTreeModel * tmodel)
{
    int i, n;
    struct {
        int type;
        const char * context;
        const char * name;
        const char * stock_id;
    } types[] = {
        { ACTIVITY_FILTER_ALL, NULL, N_("All"), NULL },
        { ACTIVITY_FILTER_SEPARATOR, NULL, NULL, NULL },
        { ACTIVITY_FILTER_ACTIVE, NULL, N_("Active"), GTK_STOCK_EXECUTE },
        { ACTIVITY_FILTER_DOWNLOADING, "Verb", NC_("Verb", "Downloading"), GTK_STOCK_GO_DOWN },
        { ACTIVITY_FILTER_SEEDING, "Verb", NC_("Verb", "Seeding"), GTK_STOCK_GO_UP },
        { ACTIVITY_FILTER_PAUSED, NULL, N_("Paused"), GTK_STOCK_MEDIA_PAUSE },
        { ACTIVITY_FILTER_FINISHED, NULL, N_("Finished"), NULL },
        { ACTIVITY_FILTER_VERIFYING, "Verb", NC_("Verb", "Verifying"), GTK_STOCK_REFRESH },
        { ACTIVITY_FILTER_ERROR, NULL, N_("Error"), GTK_STOCK_DIALOG_ERROR }
    };
    GtkListStore * store = gtk_list_store_new (ACTIVITY_FILTER_N_COLS,
                                               G_TYPE_STRING,
                                               G_TYPE_INT,
                                               G_TYPE_INT,
                                               G_TYPE_STRING);
    for (i=0, n=G_N_ELEMENTS (types); i<n; ++i) {
        const char * name = types[i].context ? g_dpgettext2 (NULL, types[i].context, types[i].name)
                                             : _ (types[i].name);
        gtk_list_store_insert_with_values (store, NULL, -1,
            ACTIVITY_FILTER_COL_NAME, name,
            ACTIVITY_FILTER_COL_TYPE, types[i].type,
            ACTIVITY_FILTER_COL_STOCK_ID, types[i].stock_id,
            -1);
    }

    g_object_set_qdata (G_OBJECT (store), TORRENT_MODEL_KEY, tmodel);
    activity_filter_model_update (store);
    return GTK_TREE_MODEL (store);
}

static void
render_activity_pixbuf_func (GtkCellLayout    * cell_layout UNUSED,
                             GtkCellRenderer  * cell_renderer,
                             GtkTreeModel     * tree_model,
                             GtkTreeIter      * iter,
                             gpointer           data UNUSED)
{
    int type;
    int width;
    int ypad;
    const gboolean leaf = !gtk_tree_model_iter_has_child (tree_model, iter);

    gtk_tree_model_get (tree_model, iter, ACTIVITY_FILTER_COL_TYPE, &type, -1);
    width = type == ACTIVITY_FILTER_ALL ? 0 : 20;
    ypad = type == ACTIVITY_FILTER_ALL ? 0 : 2;

    g_object_set (cell_renderer, "width", width,
                                 "sensitive", leaf,
                                 "ypad", ypad,
                                 NULL);
}

static void
activity_model_update_idle (gpointer activity_model)
{
    GObject * o = G_OBJECT (activity_model);
    const gboolean pending = g_object_get_qdata (o, DIRTY_KEY) != NULL;
    if (!pending)
    {
        GSourceFunc func = (GSourceFunc) activity_filter_model_update;
        g_object_set_qdata (o, DIRTY_KEY, GINT_TO_POINTER (1));
        gdk_threads_add_idle (func, activity_model);
    }
}

static void
activity_torrent_model_row_changed (GtkTreeModel  * tmodel UNUSED,
                                    GtkTreePath   * path UNUSED,
                                    GtkTreeIter   * iter UNUSED,
                                    gpointer        activity_model)
{
    activity_model_update_idle (activity_model);
}

static void
activity_torrent_model_row_deleted_cb (GtkTreeModel  * tmodel UNUSED,
                                       GtkTreePath   * path UNUSED,
                                       gpointer        activity_model)
{
    activity_model_update_idle (activity_model);
}

static void
disconnect_activity_model_callbacks (gpointer tmodel, GObject * cat_model)
{
    g_signal_handlers_disconnect_by_func (tmodel, activity_torrent_model_row_changed, cat_model);
    g_signal_handlers_disconnect_by_func (tmodel, activity_torrent_model_row_deleted_cb, cat_model);
}

static GtkWidget *
activity_combo_box_new (GtkTreeModel * tmodel)
{
    GtkWidget * c;
    GtkCellRenderer * r;
    GtkTreeModel * activity_model;

    activity_model = activity_filter_model_new (tmodel);
    c = gtk_combo_box_new_with_model (activity_model);
    g_object_unref (activity_model);
    gtk_combo_box_set_row_separator_func (GTK_COMBO_BOX (c),
                                       activity_is_it_a_separator, NULL, NULL);
    gtk_combo_box_set_active (GTK_COMBO_BOX (c), 0);

    r = gtk_cell_renderer_pixbuf_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (c), r, FALSE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (c), r,
                                    "stock-id", ACTIVITY_FILTER_COL_STOCK_ID,
                                    NULL);
    gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (c), r,
                                        render_activity_pixbuf_func, NULL, NULL);

    r = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (c), r, TRUE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (c), r,
                                    "text", ACTIVITY_FILTER_COL_NAME,
                                    NULL);

    r = number_renderer_new ();
    gtk_cell_layout_pack_end (GTK_CELL_LAYOUT (c), r, TRUE);
    gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (c), r,
                                        render_number_func, NULL, NULL);

    g_object_weak_ref (G_OBJECT (activity_model), disconnect_activity_model_callbacks, tmodel);
    g_signal_connect (tmodel, "row-changed", G_CALLBACK (activity_torrent_model_row_changed), activity_model);
    g_signal_connect (tmodel, "row-inserted", G_CALLBACK (activity_torrent_model_row_changed), activity_model);
    g_signal_connect (tmodel, "row-deleted", G_CALLBACK (activity_torrent_model_row_deleted_cb), activity_model);

    return c;
}

/****
*****
*****  ENTRY FIELD
*****
****/

static gboolean
testText (const tr_torrent * tor, const char * key)
{
    gboolean ret = FALSE;

    if (!key || !*key)
    {
        ret = TRUE;
    }
    else
    {
        tr_file_index_t i;
        const tr_info * inf = tr_torrentInfo (tor);

        /* test the torrent name... */
        {
            char * pch = g_utf8_casefold (tr_torrentName (tor), -1);
            ret = !key || strstr (pch, key) != NULL;
            g_free (pch);
        }

        /* test the files... */
        for (i=0; i<inf->fileCount && !ret; ++i)
        {
            char * pch = g_utf8_casefold (inf->files[i].name, -1);
            ret = !key || strstr (pch, key) != NULL;
            g_free (pch);
        }
    }

    return ret;
}

static void
entry_clear (GtkEntry * e)
{
    gtk_entry_set_text (e, "");
}

static void
filter_entry_changed (GtkEditable * e, gpointer filter_model)
{
    char * pch;
    char * folded;

    pch = gtk_editable_get_chars (e, 0, -1);
    folded = g_utf8_casefold (pch, -1);
    g_strstrip (folded);
    g_object_set_qdata_full (filter_model, TEXT_KEY, folded, g_free);
    g_free (pch);

    gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (filter_model));
}

/*****
******
******
******
*****/

struct filter_data
{
    GtkWidget * activity;
    GtkWidget * category;
    GtkWidget * entry;
    GtkTreeModel * filter_model;
    int active_activity_type;
    int active_category_type;
    char * active_category_host;
};

static gboolean
is_row_visible (GtkTreeModel * model, GtkTreeIter * iter, gpointer vdata)
{
    const char * text;
    tr_torrent * tor;
    struct filter_data * data = vdata;
    GObject * o = G_OBJECT (data->filter_model);

    gtk_tree_model_get (model, iter, MC_TORRENT, &tor, -1);

    text = (const char*) g_object_get_qdata (o, TEXT_KEY);

    return (tor != NULL) && test_category (tor, data->active_category_type, data->active_category_host)
                           && test_torrent_activity (tor, data->active_activity_type)
                           && testText (tor, text);
}

static void
selection_changed_cb (GtkComboBox * combo, gpointer vdata)
{
    int type;
    char * host;
    GtkTreeIter iter;
    GtkTreeModel * model;
    struct filter_data * data = vdata;

    /* set data->active_activity_type from the activity combobox */
    combo = GTK_COMBO_BOX (data->activity);
    model = gtk_combo_box_get_model (combo);
    if (gtk_combo_box_get_active_iter (combo, &iter))
        gtk_tree_model_get (model, &iter, ACTIVITY_FILTER_COL_TYPE, &type, -1);
    else
        type = ACTIVITY_FILTER_ALL;
    data->active_activity_type = type;

    /* set the active category type & host from the category combobox */
    combo = GTK_COMBO_BOX (data->category);
    model = gtk_combo_box_get_model (combo);
    if (gtk_combo_box_get_active_iter (combo, &iter)) {
        gtk_tree_model_get (model, &iter, CAT_FILTER_COL_TYPE, &type,
                                          CAT_FILTER_COL_HOST, &host,
                                          -1);
    } else {
        type = CAT_FILTER_TYPE_ALL;
        host = NULL;
    }
    g_free (data->active_category_host);
    data->active_category_host = host;
    data->active_category_type = type;

    /* refilter */
    gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (data->filter_model));
}

GtkWidget *
gtr_filter_bar_new (tr_session * session, GtkTreeModel * tmodel, GtkTreeModel ** filter_model)
{
    GtkWidget * l;
    GtkWidget * w;
    GtkWidget * h;
    GtkWidget * s;
    GtkWidget * activity;
    GtkWidget * category;
    const char * str;
    struct filter_data * data;

    g_assert (DIRTY_KEY == 0);
    TEXT_KEY = g_quark_from_static_string ("tr-filter-text-key");
    DIRTY_KEY = g_quark_from_static_string ("tr-filter-dirty-key");
    SESSION_KEY = g_quark_from_static_string ("tr-session-key");
    TORRENT_MODEL_KEY = g_quark_from_static_string ("tr-filter-torrent-model-key");

    data = g_new0 (struct filter_data, 1);
    data->activity = activity = activity_combo_box_new (tmodel);
    data->category = category = category_combo_box_new (tmodel);
    data->filter_model = gtk_tree_model_filter_new (tmodel, NULL);

    g_object_set (G_OBJECT (data->category), "width-request", 170, NULL);
    g_object_set_qdata (G_OBJECT (gtk_combo_box_get_model (GTK_COMBO_BOX (data->category))), SESSION_KEY, session);

    gtk_tree_model_filter_set_visible_func (
        GTK_TREE_MODEL_FILTER (data->filter_model),
        is_row_visible, data, g_free);

    g_signal_connect (data->category, "changed", G_CALLBACK (selection_changed_cb), data);
    g_signal_connect (data->activity, "changed", G_CALLBACK (selection_changed_cb), data);


    h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GUI_PAD_SMALL);

    /* add the activity combobox */
    str = _("_Show:");
    w = activity;
    l = gtk_label_new (NULL);
    gtk_label_set_markup_with_mnemonic (GTK_LABEL (l), str);
    gtk_label_set_mnemonic_widget (GTK_LABEL (l), w);
    gtk_box_pack_start (GTK_BOX (h), l, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (h), w, TRUE, TRUE, 0);

    /* add a spacer */
    w = gtk_alignment_new (0.0f, 0.0f, 0.0f, 0.0f);
    gtk_widget_set_size_request (w, 0u, GUI_PAD_BIG);
    gtk_box_pack_start (GTK_BOX (h), w, FALSE, FALSE, 0);

    /* add the category combobox */
    w = category;
    gtk_box_pack_start (GTK_BOX (h), w, TRUE, TRUE, 0);

    /* add a spacer */
    w = gtk_alignment_new (0.0f, 0.0f, 0.0f, 0.0f);
    gtk_widget_set_size_request (w, 0u, GUI_PAD_BIG);
    gtk_box_pack_start (GTK_BOX (h), w, FALSE, FALSE, 0);

    /* add the entry field */
    s = gtk_entry_new ();
    gtk_entry_set_icon_from_stock (GTK_ENTRY (s), GTK_ENTRY_ICON_SECONDARY, GTK_STOCK_CLEAR);
    g_signal_connect (s, "icon-release", G_CALLBACK (entry_clear), NULL);
    gtk_box_pack_start (GTK_BOX (h), s, TRUE, TRUE, 0);

    g_signal_connect (s, "changed", G_CALLBACK (filter_entry_changed), data->filter_model);
    selection_changed_cb (NULL, data);

    *filter_model = data->filter_model;
    return h;
}
