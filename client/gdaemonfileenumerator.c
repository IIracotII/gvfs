/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gdaemonfileenumerator.h>
#include <gio/gio.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>
#include "gdaemonfile.h"
#include "metatree.h"
#include <gvfsdbus.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/enumerator/"

/* atomic */
static volatile gint path_counter = 1;

G_LOCK_DEFINE_STATIC(infos);

struct _GDaemonFileEnumerator
{
  GFileEnumerator parent;

  gint id;
  GDBusConnection *sync_connection; /* NULL if async, i.e. we're listening on main dbus connection */

  /* protected by infos lock */
  GList *infos;
  gboolean done;

  /* For async ops, also protected by infos lock */
  int async_requested_files;
  gulong cancelled_tag;
  guint timeout_tag;
  GSimpleAsyncResult *async_res;
  GMainLoop *next_files_mainloop;
  GMainContext *next_files_context;
  guint next_files_sync_timeout_tag;
  GMutex next_files_mutex;

  GFileAttributeMatcher *matcher;
  MetaTree *metadata_tree;
};

G_DEFINE_TYPE (GDaemonFileEnumerator, g_daemon_file_enumerator, G_TYPE_FILE_ENUMERATOR)

static GFileInfo *       g_daemon_file_enumerator_next_file   (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static void              g_daemon_file_enumerator_next_files_async (GFileEnumerator     *enumerator,
								    int                  num_files,
								    int                  io_priority,
								    GCancellable        *cancellable,
								    GAsyncReadyCallback  callback,
								    gpointer             user_data);
static GList *           g_daemon_file_enumerator_next_files_finish (GFileEnumerator  *enumerator,
								     GAsyncResult     *result,
								     GError          **error);
static gboolean          g_daemon_file_enumerator_close       (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static void              g_daemon_file_enumerator_close_async (GFileEnumerator      *enumerator,
							       int                   io_priority,
							       GCancellable         *cancellable,
							       GAsyncReadyCallback   callback,
							       gpointer              user_data);
static gboolean          g_daemon_file_enumerator_close_finish (GFileEnumerator      *enumerator,
							       GAsyncResult         *result,
							       GError              **error);
static void              trigger_async_done (GDaemonFileEnumerator *daemon, gboolean ok);


static void
free_info_list (GList *infos)
{
  g_list_free_full (infos, g_object_unref);
}

static void
g_daemon_file_enumerator_finalize (GObject *object)
{
  GDaemonFileEnumerator *daemon;
  char *path;

  daemon = G_DAEMON_FILE_ENUMERATOR (object);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_unregister_vfs_filter (path);
  g_free (path);

  free_info_list (daemon->infos);

  g_file_attribute_matcher_unref (daemon->matcher);
  if (daemon->metadata_tree)
    meta_tree_unref (daemon->metadata_tree);

  g_clear_object (&daemon->sync_connection);

  if (daemon->next_files_context)
    g_main_context_unref (daemon->next_files_context);

  g_mutex_clear (&daemon->next_files_mutex);
  
  if (G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize) (object);
}


static void
g_daemon_file_enumerator_class_init (GDaemonFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_enumerator_finalize;

  enumerator_class->next_file = g_daemon_file_enumerator_next_file;
  enumerator_class->next_files_async = g_daemon_file_enumerator_next_files_async;
  enumerator_class->next_files_finish = g_daemon_file_enumerator_next_files_finish;
  enumerator_class->close_fn = g_daemon_file_enumerator_close;
  enumerator_class->close_async = g_daemon_file_enumerator_close_async;
  enumerator_class->close_finish = g_daemon_file_enumerator_close_finish;
}

static void
next_files_sync_check (GDaemonFileEnumerator *enumerator)
{
  g_mutex_lock (&enumerator->next_files_mutex);
  if ((enumerator->infos || enumerator->done) && 
      enumerator->next_files_mainloop != NULL)
    {
      g_main_loop_quit (enumerator->next_files_mainloop);
    }
  g_mutex_unlock (&enumerator->next_files_mutex);
}

static gboolean
handle_done (GVfsDBusEnumerator *object,
             GDBusMethodInvocation *invocation,
             gpointer user_data)
{
  GDaemonFileEnumerator *enumerator = G_DAEMON_FILE_ENUMERATOR (user_data);

  G_LOCK (infos);
  enumerator->done = TRUE;
  if (enumerator->async_requested_files > 0)
    trigger_async_done (enumerator, TRUE);
  next_files_sync_check (enumerator);
  G_UNLOCK (infos);

  gvfs_dbus_enumerator_complete_done (object, invocation);
  
  return TRUE;
}

static gboolean
handle_got_info (GVfsDBusEnumerator *object,
                 GDBusMethodInvocation *invocation,
                 GVariant *arg_infos,
                 gpointer user_data)
{
  GDaemonFileEnumerator *enumerator = G_DAEMON_FILE_ENUMERATOR (user_data);
  GList *infos;
  GFileInfo *info;
  GVariantIter iter;
  GVariant *child;

  infos = NULL;
    
  g_variant_iter_init (&iter, arg_infos);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      info = _g_dbus_get_file_info (child, NULL);
      if (info)
        g_assert (G_IS_FILE_INFO (info));

      if (info)
        infos = g_list_prepend (infos, info);

      g_variant_unref (child);
    }
  
  infos = g_list_reverse (infos);
  
  G_LOCK (infos);
  enumerator->infos = g_list_concat (enumerator->infos, infos);
  if (enumerator->async_requested_files > 0 &&
      g_list_length (enumerator->infos) >= enumerator->async_requested_files)
    trigger_async_done (enumerator, TRUE);
  next_files_sync_check (enumerator);
  G_UNLOCK (infos);

  gvfs_dbus_enumerator_complete_got_info (object, invocation);
  
  return TRUE;
}

static GDBusInterfaceSkeleton *
register_vfs_filter_cb (GDBusConnection *connection,
                        const char *obj_path,
                        gpointer callback_data)
{
  GError *error;
  GVfsDBusEnumerator *skeleton;
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (callback_data);

  if (daemon->next_files_context)
    g_main_context_push_thread_default (daemon->next_files_context);

  skeleton = gvfs_dbus_enumerator_skeleton_new ();
  g_signal_connect (skeleton, "handle-done", G_CALLBACK (handle_done), callback_data);
  g_signal_connect (skeleton, "handle-got-info", G_CALLBACK (handle_got_info), callback_data);

  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton),
                                         connection,
                                         obj_path,
                                         &error))
    {
      g_warning ("Error registering path: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  if (daemon->next_files_context)
    g_main_context_pop_thread_default (daemon->next_files_context);

  return G_DBUS_INTERFACE_SKELETON (skeleton);
}

static void
g_daemon_file_enumerator_init (GDaemonFileEnumerator *daemon)
{
  daemon->id = g_atomic_int_add (&path_counter, 1);

  g_mutex_init (&daemon->next_files_mutex);
}

GDaemonFileEnumerator *
g_daemon_file_enumerator_new (GFile *file,
			      const char *attributes,
			      gboolean sync)
{
  GDaemonFileEnumerator *daemon;
  char *treename;
  char *path;

  daemon = g_object_new (G_TYPE_DAEMON_FILE_ENUMERATOR,
                         "container", file,
                         NULL);

  if (sync)
    daemon->next_files_context = g_main_context_new ();

  path = g_daemon_file_enumerator_get_object_path (daemon);

  _g_dbus_register_vfs_filter (path,
                               register_vfs_filter_cb,
                               G_OBJECT (daemon));
  g_free (path);

  daemon->matcher = g_file_attribute_matcher_new (attributes);
  if (g_file_attribute_matcher_enumerate_namespace (daemon->matcher, "metadata") ||
      g_file_attribute_matcher_enumerate_next (daemon->matcher) != NULL)
    {
      treename = g_mount_spec_to_string (G_DAEMON_FILE (file)->mount_spec);
      daemon->metadata_tree = meta_tree_lookup_by_name (treename, FALSE);
      g_free (treename);
    }

  return daemon;
}

static gboolean
enumerate_keys_callback (const char *key,
			 MetaKeyType type,
			 gpointer value,
			 gpointer user_data)
{
  GFileInfo *info = G_FILE_INFO (user_data);
  char *attr;

  attr = g_strconcat ("metadata::", key, NULL);

  if (type == META_KEY_TYPE_STRING)
    g_file_info_set_attribute_string (info, attr, (char *)value);
  else
    g_file_info_set_attribute_stringv (info, attr, (char **)value);

  g_free (attr);

  return TRUE;
}

static void
add_metadata (GFileInfo *info,
	      GDaemonFileEnumerator *daemon)
{
  GFile *container;
  const char *name;
  char *path;

  if (!daemon->metadata_tree)
    return;

  name = g_file_info_get_name (info);
  container = g_file_enumerator_get_container (G_FILE_ENUMERATOR (daemon));
  path = g_build_filename (G_DAEMON_FILE (container)->path, name, NULL);

  g_file_info_set_attribute_mask (info, daemon->matcher);
  meta_tree_enumerate_keys (daemon->metadata_tree, path,
			    enumerate_keys_callback, info);
  g_file_info_unset_attribute_mask (info);

  g_free (path);
}

static GCancellable *
simple_async_result_get_cancellable (GSimpleAsyncResult *res)
{
  return g_object_get_data (G_OBJECT (res), "file-enumerator-cancellable");
}

static void
simple_async_result_set_cancellable (GSimpleAsyncResult *res,
                                     GCancellable       *cancellable)
{
  if (!cancellable)
    return;

  g_object_set_data_full (G_OBJECT (res),
                          "file-enumerator-cancellable",
                          g_object_ref (cancellable),
                          g_object_unref);
}

/* Called with infos lock held */
static void
trigger_async_done (GDaemonFileEnumerator *daemon, gboolean ok)
{
  GList *rest, *l;
  
  if (daemon->cancelled_tag != 0)
    {
      GCancellable *cancellable = simple_async_result_get_cancellable (daemon->async_res);

      /* If ok, we're a normal callback on the main thread,
	 ensure protection against a thread cancelling and
	 running the callback again.
	 If !ok then we're in a callback which may be
	 from another thread, but we're guaranteed that
	 cancellation will only happen once. However
	 we can't use g_cancellable_disconnect, as this
	 deadlocks if we call it from within the signal
	 handler.
      */
      if (ok)
	g_cancellable_disconnect (cancellable,
				  daemon->cancelled_tag);
      else
	g_signal_handler_disconnect (cancellable,
				     daemon->cancelled_tag);
    }

  /* cancelled signal handler won't execute after this */

  /* TODO: There is actually a small race here, where
     trigger_async_done could happen twice if the operation
     succeeds for some other reason and cancellation happens
     on some other thread right before the above call.
     However, this can only happen if cancellation happens
     outside the main thread which is kinda unusual for async
     ops. However, it would be nice to handle this too. */

  if (ok)
    {
      l = daemon->infos;
      rest = g_list_nth (l, daemon->async_requested_files);
      if (rest)
	{
	  /* Split list */
	  rest->prev->next = NULL;
	  rest->prev = NULL;
	}
      daemon->infos = rest;

      g_list_foreach (l, (GFunc)add_metadata, daemon);

      g_simple_async_result_set_op_res_gpointer (daemon->async_res,
						 l,
						 (GDestroyNotify)free_info_list);
    }

  g_simple_async_result_complete_in_idle (daemon->async_res);
  
  daemon->cancelled_tag = 0;

  if (daemon->timeout_tag != 0)
    g_source_remove (daemon->timeout_tag);
  daemon->timeout_tag = 0;
  
  daemon->async_requested_files = 0;
  
  g_object_unref (daemon->async_res);
  daemon->async_res = NULL;
}

char  *
g_daemon_file_enumerator_get_object_path (GDaemonFileEnumerator *enumerator)
{
  return g_strdup_printf (OBJ_PATH_PREFIX"%d", enumerator->id);
}

void
g_daemon_file_enumerator_set_sync_connection (GDaemonFileEnumerator *enumerator,
                                              GDBusConnection       *connection)
{
  enumerator->sync_connection = g_object_ref (connection);
}

static gboolean
sync_timeout (gpointer data)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (data);

  g_mutex_lock (&daemon->next_files_mutex);
  g_main_loop_quit (daemon->next_files_mainloop);
  g_mutex_unlock (&daemon->next_files_mutex);
  
  return FALSE;
}

static GFileInfo *
g_daemon_file_enumerator_next_file (GFileEnumerator *enumerator,
				    GCancellable     *cancellable,
				    GError **error)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator);
  GFileInfo *info;
  
  if (daemon->sync_connection == NULL)
    {
      /* The enumerator was initialized by an async call, so responses will
         come to the async dbus connection. We can't pump that as that would
         cause all sort of filters and stuff to run, possibly on the wrong
         thread. If you want to do async next_files you must create the
         enumerator asynchrounously.
      */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't do synchronous next_files() on a file enumerator created asynchronously");
      return NULL;
    }

  if (! daemon->infos && ! daemon->done)
    {
      /* Wait for incoming data */
      g_mutex_lock (&daemon->next_files_mutex);
      daemon->next_files_mainloop = g_main_loop_new (daemon->next_files_context, FALSE);

      g_mutex_unlock (&daemon->next_files_mutex);
      
      g_main_context_push_thread_default (daemon->next_files_context);
      daemon->next_files_sync_timeout_tag = g_timeout_add (G_VFS_DBUS_TIMEOUT_MSECS,
                                                           sync_timeout, daemon);
      g_main_loop_run (daemon->next_files_mainloop);
      g_main_context_pop_thread_default (daemon->next_files_context);

      g_mutex_lock (&daemon->next_files_mutex);
      g_source_remove (daemon->next_files_sync_timeout_tag);

      g_main_loop_unref (daemon->next_files_mainloop);
      daemon->next_files_mainloop = NULL;
      g_mutex_unlock (&daemon->next_files_mutex);
    }
  
  info = NULL;

  G_LOCK (infos);
  if (daemon->infos)
    {
      info = daemon->infos->data;
      if (info)
        {
          g_assert (G_IS_FILE_INFO (info));
          add_metadata (G_FILE_INFO (info), daemon);
        }
      daemon->infos = g_list_delete_link (daemon->infos, daemon->infos);
    }
  G_UNLOCK (infos);

  if (info)
    g_assert (G_IS_FILE_INFO (info));
      
  return info;
}

static void
async_cancelled (GCancellable *cancellable,
		 GDaemonFileEnumerator *daemon)
{
  g_simple_async_result_set_error (daemon->async_res,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
				   _("Operation was cancelled"));
  G_LOCK (infos);
  trigger_async_done (daemon, FALSE);
  G_UNLOCK (infos);
}

static gboolean
async_timeout (gpointer data)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (data);

  G_LOCK (infos);
  trigger_async_done (daemon, TRUE);
  G_UNLOCK (infos);
  return FALSE;
}

static void
g_daemon_file_enumerator_next_files_async (GFileEnumerator     *enumerator,
					   int                  num_files,
					   int                  io_priority,
					   GCancellable        *cancellable,
					   GAsyncReadyCallback  callback,
					   gpointer             user_data)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator);

  if (daemon->sync_connection != NULL)
    {
      /* If the enumerator was created synchronously then the connection used
       * for return messages will be the private connection for that thread.
       * We can't rely on it being pumped, so we don't support this.
       * We could possibly pump it ourselves in this case, but i'm not sure
       * how much sense this makes, so we don't for now.
       */
      g_simple_async_report_error_in_idle  (G_OBJECT (enumerator),
					    callback,
					    user_data,
					    G_IO_ERROR, G_IO_ERROR_FAILED,
					    "Can't do asynchronous next_files() on a file enumerator created synchronously");
      return;
    }
  
  G_LOCK (infos);
  daemon->cancelled_tag = 0;
  daemon->timeout_tag = 0;
  daemon->async_requested_files = num_files;
  daemon->async_res = g_simple_async_result_new (G_OBJECT (enumerator), callback, user_data,
						 g_daemon_file_enumerator_next_files_async);
  simple_async_result_set_cancellable (daemon->async_res, cancellable);

  /* Maybe we already have enough info to fulfill the requeust already */
  if (daemon->done ||
      g_list_length (daemon->infos) >= daemon->async_requested_files)
    trigger_async_done (daemon, TRUE);
  else
    {
      daemon->timeout_tag = g_timeout_add (G_VFS_DBUS_TIMEOUT_MSECS,
					   async_timeout, daemon);
      if (cancellable)
	daemon->cancelled_tag =
	  g_cancellable_connect (cancellable, (GCallback)async_cancelled,
				 daemon, NULL);
    }
  
  G_UNLOCK (infos);
}

static GList *
g_daemon_file_enumerator_next_files_finish (GFileEnumerator  *enumerator,
					    GAsyncResult     *res,
					    GError          **error)
{
  GSimpleAsyncResult *result = G_SIMPLE_ASYNC_RESULT (res);
  GCancellable *cancellable;
  GList *l;

  cancellable = simple_async_result_get_cancellable (result);
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_CANCELLED,
                   "%s", _("Operation was cancelled"));
      return NULL;
    }

  l = g_simple_async_result_get_op_res_gpointer (result);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);
  return g_list_copy (l);
}

static gboolean
g_daemon_file_enumerator_close (GFileEnumerator *enumerator,
				GCancellable     *cancellable,
				GError          **error)
{
  /*GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator); */

  return TRUE;
}


/* We want an explicitly async version of close (doing nothing) to avoid
   the default thread-using version. */
static void
g_daemon_file_enumerator_close_async (GFileEnumerator      *enumerator,
				      int                   io_priority,
				      GCancellable         *cancellable,
				      GAsyncReadyCallback   callback,
				      gpointer              user_data)
{
  GSimpleAsyncResult *res;

  res = g_simple_async_result_new (G_OBJECT (enumerator), callback, user_data,
				   g_daemon_file_enumerator_close_async);
  simple_async_result_set_cancellable (res, cancellable);
  g_simple_async_result_complete_in_idle (res);
  g_object_unref (res);
}

static gboolean
g_daemon_file_enumerator_close_finish (GFileEnumerator      *enumerator,
				       GAsyncResult         *result,
				       GError              **error)
{
  GCancellable *cancellable;
  
  cancellable = simple_async_result_get_cancellable (G_SIMPLE_ASYNC_RESULT (result));
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_CANCELLED,
                   "%s", _("Operation was cancelled"));
      return FALSE;
    }

  return TRUE;
}
