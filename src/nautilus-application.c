/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * nautilus-application: main Nautilus application class.
 *
 * Copyright (C) 1999, 2000 Red Hat, Inc.
 * Copyright (C) 2000, 2001 Eazel, Inc.
 * Copyright (C) 2010, Cosimo Cecchi <cosimoc@gnome.org>
 *
 * Nautilus is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nautilus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Elliot Lee <sopwith@redhat.com>,
 *          Darin Adler <darin@bentspoon.com>
 *          Cosimo Cecchi <cosimoc@gnome.org>
 *
 */

#include <config.h>

#include "nautilus-application.h"

#include "nautilus-application-actions.h"
#include "nautilus-bookmarks-window.h"
#include "nautilus-dbus-manager.h"
#include "nautilus-desktop-window.h"
#include "nautilus-freedesktop-dbus.h"
#include "nautilus-image-properties-page.h"
#include "nautilus-previewer.h"
#include "nautilus-progress-persistence-handler.h"
#include "nautilus-self-check-functions.h"
#include "nautilus-shell-search-provider.h"
#include "nautilus-window.h"
#include "nautilus-window-slot.h"

#include <libnautilus-private/nautilus-directory-private.h>
#include <libnautilus-private/nautilus-file-utilities.h>
#include <libnautilus-private/nautilus-file-operations.h>
#include <libnautilus-private/nautilus-global-preferences.h>
#include <libnautilus-private/nautilus-lib-self-check-functions.h>
#include <libnautilus-private/nautilus-module.h>
#include <libnautilus-private/nautilus-profile.h>
#include <libnautilus-private/nautilus-signaller.h>
#include <libnautilus-private/nautilus-ui-utilities.h>
#include <libnautilus-extension/nautilus-menu-provider.h>

#define DEBUG_FLAG NAUTILUS_DEBUG_APPLICATION
#include <libnautilus-private/nautilus-debug.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-stock-dialogs.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

G_DEFINE_TYPE (NautilusApplication, nautilus_application, GTK_TYPE_APPLICATION);

struct _NautilusApplicationPriv {
	NautilusProgressPersistenceHandler *progress_handler;
	NautilusDBusManager *dbus_manager;
	NautilusFreedesktopDBus *fdb_manager;

	gboolean desktop_override;

	NautilusBookmarkList *bookmark_list;

	NautilusShellSearchProvider *search_provider;

	GList *windows;
};

void
nautilus_application_add_accelerator (GApplication *app,
				      const gchar  *action_name,
				      const gchar  *accel)
{
	const gchar *vaccels[] = {
		accel,
		NULL
	};

	gtk_application_set_accels_for_action (GTK_APPLICATION (app), action_name, vaccels);
}

GList *
nautilus_application_get_windows (NautilusApplication *application)
{
	return application->priv->windows;
}

NautilusBookmarkList *
nautilus_application_get_bookmarks (NautilusApplication *application)
{
	if (!application->priv->bookmark_list) {
		application->priv->bookmark_list = nautilus_bookmark_list_new ();
	}

	return application->priv->bookmark_list;
}

void
nautilus_application_edit_bookmarks (NautilusApplication *application,
				     NautilusWindow      *window)
{
	GtkWindow *bookmarks_window;

	bookmarks_window = nautilus_bookmarks_window_new (window);
	gtk_window_present (bookmarks_window);
}

static gboolean
check_required_directories (NautilusApplication *application)
{
	char *user_directory;
	char *desktop_directory;
	GSList *directories;
	gboolean ret;

	g_assert (NAUTILUS_IS_APPLICATION (application));

	nautilus_profile_start (NULL);

	ret = TRUE;

	user_directory = nautilus_get_user_directory ();
	desktop_directory = nautilus_get_desktop_directory ();

	directories = NULL;

	if (!g_file_test (user_directory, G_FILE_TEST_IS_DIR)) {
		directories = g_slist_prepend (directories, user_directory);
	}

	if (!g_file_test (desktop_directory, G_FILE_TEST_IS_DIR)) {
		directories = g_slist_prepend (directories, desktop_directory);
	}

	if (directories != NULL) {
		int failed_count;
		GString *directories_as_string;
		GSList *l;
		char *error_string;
		const char *detail_string;
		GtkDialog *dialog;

		ret = FALSE;

		failed_count = g_slist_length (directories);

		directories_as_string = g_string_new ((const char *)directories->data);
		for (l = directories->next; l != NULL; l = l->next) {
			g_string_append_printf (directories_as_string, ", %s", (const char *)l->data);
		}

		error_string = _("Oops! Something went wrong.");
		if (failed_count == 1) {
			detail_string = g_strdup_printf (_("Unable to create a required folder. "
							   "Please create the following folder, or "
							   "set permissions such that it can be created:\n%s"),
							 directories_as_string->str);
		} else {
			detail_string = g_strdup_printf (_("Unable to create required folders. "
							   "Please create the following folders, or "
							   "set permissions such that they can be created:\n%s"),
							 directories_as_string->str);
		}

		dialog = eel_show_error_dialog (error_string, detail_string, NULL);
		/* We need the main event loop so the user has a chance to see the dialog. */
		gtk_application_add_window (GTK_APPLICATION (application),
					    GTK_WINDOW (dialog));

		g_string_free (directories_as_string, TRUE);
	}

	g_slist_free (directories);
	g_free (user_directory);
	g_free (desktop_directory);
	nautilus_profile_end (NULL);

	return ret;
}

static void
menu_provider_items_updated_handler (NautilusMenuProvider *provider, GtkWidget* parent_window, gpointer data)
{

	g_signal_emit_by_name (nautilus_signaller_get_current (),
			       "popup-menu-changed");
}

static void
menu_provider_init_callback (void)
{
        GList *providers;
        GList *l;

        providers = nautilus_module_get_extensions_for_type (NAUTILUS_TYPE_MENU_PROVIDER);

        for (l = providers; l != NULL; l = l->next) {
                NautilusMenuProvider *provider = NAUTILUS_MENU_PROVIDER (l->data);

		g_signal_connect_after (G_OBJECT (provider), "items-updated",
                           (GCallback)menu_provider_items_updated_handler,
                           NULL);
        }

        nautilus_module_extension_list_free (providers);
}

static void
mark_desktop_files_trusted (void)
{
	char *do_once_file;
	GFile *f, *c;
	GFileEnumerator *e;
	GFileInfo *info;
	const char *name;
	int fd;
	
	do_once_file = g_build_filename (g_get_user_data_dir (),
					 ".converted-launchers", NULL);

	if (g_file_test (do_once_file, G_FILE_TEST_EXISTS)) {
		goto out;
	}

	f = nautilus_get_desktop_location ();
	e = g_file_enumerate_children (f,
				       G_FILE_ATTRIBUTE_STANDARD_TYPE ","
				       G_FILE_ATTRIBUTE_STANDARD_NAME ","
				       G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
				       ,
				       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				       NULL, NULL);
	if (e == NULL) {
		goto out2;
	}
	
	while ((info = g_file_enumerator_next_file (e, NULL, NULL)) != NULL) {
		name = g_file_info_get_name (info);
		
		if (g_str_has_suffix (name, ".desktop") &&
		    !g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)) {
			c = g_file_get_child (f, name);
			nautilus_file_mark_desktop_file_trusted (c,
								 NULL, FALSE,
								 NULL, NULL);
			g_object_unref (c);
		}
		g_object_unref (info);
	}
	
	g_object_unref (e);
 out2:
	fd = g_creat (do_once_file, 0666);
	close (fd);
	
	g_object_unref (f);
 out:	
	g_free (do_once_file);
}

static void
do_upgrades_once (NautilusApplication *self)
{
	char *metafile_dir, *updated, *nautilus_dir, *xdg_dir;
	const gchar *message;
	int fd, res;

	mark_desktop_files_trusted ();

	metafile_dir = g_build_filename (g_get_home_dir (),
					 ".nautilus/metafiles", NULL);
	if (g_file_test (metafile_dir, G_FILE_TEST_IS_DIR)) {
		updated = g_build_filename (metafile_dir, "migrated-to-gvfs", NULL);
		if (!g_file_test (updated, G_FILE_TEST_EXISTS)) {
			g_spawn_command_line_async (LIBEXECDIR"/nautilus-convert-metadata --quiet", NULL);
			fd = g_creat (updated, 0600);
			if (fd != -1) {
				close (fd);
			}
		}
		g_free (updated);
	}
	g_free (metafile_dir);

	nautilus_dir = g_build_filename (g_get_home_dir (),
					 ".nautilus", NULL);
	xdg_dir = nautilus_get_user_directory ();
	if (g_file_test (nautilus_dir, G_FILE_TEST_IS_DIR)) {
		/* test if we already attempted to migrate first */
		updated = g_build_filename (nautilus_dir, "DEPRECATED-DIRECTORY", NULL);
		message = _("Nautilus 3.0 deprecated this directory and tried migrating "
			    "this configuration to ~/.config/nautilus");
		if (!g_file_test (updated, G_FILE_TEST_EXISTS)) {
			/* rename() works fine if the destination directory is
			 * empty.
			 */
			res = g_rename (nautilus_dir, xdg_dir);

			if (res == -1) {
				fd = g_creat (updated, 0600);
				if (fd != -1) {
					res = write (fd, message, strlen (message));
					close (fd);
				}
			}
		}

		g_free (updated);
	}

	g_free (nautilus_dir);
	g_free (xdg_dir);
}

NautilusWindow *
nautilus_application_create_window (NautilusApplication *application,
				    GdkScreen           *screen)
{
	NautilusWindow *window;
	char *geometry_string;
	gboolean maximized;
	gint n_windows;

	g_return_val_if_fail (NAUTILUS_IS_APPLICATION (application), NULL);
	nautilus_profile_start (NULL);

	n_windows = g_list_length (application->priv->windows);
	window = nautilus_window_new (screen);

	maximized = g_settings_get_boolean
		(nautilus_window_state, NAUTILUS_WINDOW_STATE_MAXIMIZED);
	if (maximized) {
		gtk_window_maximize (GTK_WINDOW (window));
	} else {
		gtk_window_unmaximize (GTK_WINDOW (window));
	}

	geometry_string = g_settings_get_string
		(nautilus_window_state, NAUTILUS_WINDOW_STATE_GEOMETRY);
	if (geometry_string != NULL &&
	    geometry_string[0] != 0) {
		/* Ignore saved window position if another window is already showing.
		 * That way the two windows wont appear at the exact same
		 * location on the screen.
		 */
		eel_gtk_window_set_initial_geometry_from_string 
			(GTK_WINDOW (window), 
			 geometry_string,
			 NAUTILUS_WINDOW_MIN_WIDTH,
			 NAUTILUS_WINDOW_MIN_HEIGHT,
			 n_windows > 0);
	}
	g_free (geometry_string);

	DEBUG ("Creating a new navigation window");
	nautilus_profile_end (NULL);

	return window;
}

static NautilusWindowSlot *
get_window_slot_for_location (NautilusApplication *application, GFile *location)
{
	NautilusWindowSlot *slot;
	NautilusWindow *window;
        NautilusFile *file;
	GList *l, *sl;

	slot = NULL;
        file = nautilus_file_get (location);

        if (!nautilus_file_is_directory (file) && !nautilus_file_is_other_locations (file) &&
            g_file_has_parent (location, NULL)) {
		location = g_file_get_parent (location);
	} else {
		g_object_ref (location);
	}

	for (l = application->priv->windows; l != NULL; l = l->next) {
		window = l->data;

		for (sl = nautilus_window_get_slots (window); sl; sl = sl->next) {
			NautilusWindowSlot *current = sl->data;
			GFile *slot_location = nautilus_window_slot_get_location (current);

			if (slot_location && g_file_equal (slot_location, location)) {
				slot = current;
				break;
			}
		}

		if (slot) {
			break;
		}
	}

        nautilus_file_unref (file);
	g_object_unref (location);

	return slot;
}

static void
new_window_show_callback (GtkWidget *widget,
                          gpointer   user_data)
{
	NautilusWindow *window;

	window = NAUTILUS_WINDOW (user_data);
	nautilus_window_close (window);

	g_signal_handlers_disconnect_by_func (widget,
					      G_CALLBACK (new_window_show_callback),
					      user_data);
}

void
nautilus_application_open_location_full (NautilusApplication     *application,
                                         GFile                   *location,
                                         NautilusWindowOpenFlags  flags,
                                         GList                   *selection,
                                         NautilusWindow          *target_window,
                                         NautilusWindowSlot      *target_slot)
{
        NautilusWindowSlot *active_slot;
        NautilusWindow *active_window;
        GFile *old_location;
	char *old_uri, *new_uri;
	gboolean use_same;

	use_same = TRUE;
        active_window = NAUTILUS_WINDOW (gtk_application_get_active_window (GTK_APPLICATION (application)));
        active_slot = nautilus_window_get_active_slot (active_window);

	/* Just for debug.*/

	old_location = nautilus_window_slot_get_location (active_slot);
        /* this happens at startup */
        if (old_location == NULL)
		old_uri = g_strdup ("(none)");
        else
                old_uri = g_file_get_uri (old_location);

	new_uri = g_file_get_uri (location);

	DEBUG ("Application opening location, old: %s, new: %s", old_uri, new_uri);
	nautilus_profile_start ("Application opening location, old: %s, new: %s", old_uri, new_uri);

	g_free (old_uri);
	g_free (new_uri);
        /* end debug */

        /* In case a target slot is provided, we can use it's associated window.
         * In case a target window were given as well, we give preference to the
         * slot we target at */
        if (target_slot != NULL)
                target_window = nautilus_window_slot_get_window (target_slot);

        if ((target_window && NAUTILUS_IS_DESKTOP_WINDOW (target_window)) ||
            (!target_window && NAUTILUS_IS_DESKTOP_WINDOW (active_window))) {
                NautilusWindow *desktop_target_window;

                desktop_target_window = target_window ? target_window : active_window;
		use_same = !nautilus_desktop_window_loaded (NAUTILUS_DESKTOP_WINDOW (desktop_target_window));

		/* if we're requested to open a new tab on the desktop, open a window
		 * instead.
		 */
		if (flags & NAUTILUS_WINDOW_OPEN_FLAG_NEW_TAB) {
			flags ^= NAUTILUS_WINDOW_OPEN_FLAG_NEW_TAB;
			flags |= NAUTILUS_WINDOW_OPEN_FLAG_NEW_WINDOW;
		}
	}

	g_assert (!((flags & NAUTILUS_WINDOW_OPEN_FLAG_NEW_WINDOW) != 0 &&
		    (flags & NAUTILUS_WINDOW_OPEN_FLAG_NEW_TAB) != 0));

	/* and if the flags specify so, this is overridden */
	if ((flags & NAUTILUS_WINDOW_OPEN_FLAG_NEW_WINDOW) != 0) {
		use_same = FALSE;
	}

	/* now get/create the window */
	if (use_same) {
                if (!target_window) {
                        if (!target_slot) {
                                target_window = active_window;
                        } else {
                                target_window = nautilus_window_slot_get_window (target_slot);
                        }
                }
	} else {
		target_window = nautilus_application_create_window (application,
                                                                    gtk_window_get_screen (GTK_WINDOW (active_window)));
	}

        g_assert (target_window != NULL);

	/* close the current window if the flags say so */
	if ((flags & NAUTILUS_WINDOW_OPEN_FLAG_CLOSE_BEHIND) != 0) {
		if (NAUTILUS_IS_DESKTOP_WINDOW (active_window)) {
			if (gtk_widget_get_visible (GTK_WIDGET (target_window))) {
				nautilus_window_close (active_window);
			} else {
				g_signal_connect_object (target_window,
							 "show",
							 G_CALLBACK (new_window_show_callback),
							 active_window,
							 G_CONNECT_AFTER);
			}
		}
	}

        /* Application is the one that manages windows, so this flag shouldn't use
         * it anymore by any client */
        flags &= ~NAUTILUS_WINDOW_OPEN_FLAG_NEW_WINDOW;
        nautilus_window_open_location_full (target_window, location, flags, selection, target_slot);
}

static void
open_window (NautilusApplication *application,
             GFile               *location)
{
	NautilusWindow *window;

	nautilus_profile_start (NULL);
	window = nautilus_application_create_window (application, gdk_screen_get_default ());

	if (location != NULL) {
                nautilus_application_open_location_full (application, location, 0, NULL, window, NULL);
	} else {
                GFile *home;
	        home = g_file_new_for_path (g_get_home_dir ());
                nautilus_application_open_location_full (application, home, 0, NULL, window, NULL);

                g_object_unref (home);
	}

	nautilus_profile_end (NULL);
}

void
nautilus_application_open_location (NautilusApplication *application,
                                    GFile               *location,
                                    GFile               *selection,
                                    const char          *startup_id)
{
	NautilusWindow *window;
	NautilusWindowSlot *slot;
	GList *sel_list = NULL;

	nautilus_profile_start (NULL);

	if (selection != NULL) {
		sel_list = g_list_prepend (sel_list, nautilus_file_get (selection));
	}

	slot = get_window_slot_for_location (application, location);

	if (!slot) {
		window = nautilus_application_create_window (application, gdk_screen_get_default ());
	} else {
		window = nautilus_window_slot_get_window (slot);
	}

	nautilus_application_open_location_full (application, location, 0, sel_list, window, slot);

	if (sel_list != NULL) {
		nautilus_file_list_free (sel_list);
	}

	nautilus_profile_end (NULL);
}

/* Note: when launched from command line we do not reach this method
 * since we manually handle the command line parameters in order to
 * parse --no-default-window, etc.
 * However this method is called when open() is called via dbus, for
 * instance when gtk_uri_open () is called from outside.
 */
static void
nautilus_application_open (GApplication *app,
			   GFile **files,
			   gint n_files,
			   const gchar *hint)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (app);
	gboolean force_new = (g_strcmp0 (hint, "new-window") == 0);
	NautilusWindowSlot *slot = NULL;
	GFile *file;
	gint idx;

	DEBUG ("Open called on the GApplication instance; %d files", n_files);

	/* Open windows at each requested location. */
	for (idx = 0; idx < n_files; idx++) {
		file = files[idx];

		if (!force_new) {
			slot = get_window_slot_for_location (self, file);
		}

		if (!slot) {
			open_window (self, file);
		} else {
			/* We open the location again to update any possible selection */
			nautilus_application_open_location_full (NAUTILUS_APPLICATION (app), file, 0, NULL, NULL, slot);
		}
	}
}

static void
nautilus_application_finalize (GObject *object)
{
	NautilusApplication *application;

	application = NAUTILUS_APPLICATION (object);

	g_clear_object (&application->priv->progress_handler);
	g_clear_object (&application->priv->bookmark_list);

	g_clear_object (&application->priv->dbus_manager);
	g_clear_object (&application->priv->fdb_manager);
	g_clear_object (&application->priv->search_provider);

	g_list_free (application->priv->windows);

        G_OBJECT_CLASS (nautilus_application_parent_class)->finalize (object);
}

static gboolean
do_cmdline_sanity_checks (NautilusApplication *self,
			  GVariantDict        *options)
{
	gboolean retval = FALSE;

	if (g_variant_dict_contains (options, "check") &&
	    (g_variant_dict_contains (options, G_OPTION_REMAINING) ||
	     g_variant_dict_contains (options, "quit"))) {
		g_printerr ("%s\n",
			    _("--check cannot be used with other options."));
		goto out;
	}

	if (g_variant_dict_contains (options, "quit") &&
	    g_variant_dict_contains (options, G_OPTION_REMAINING)) {
		g_printerr ("%s\n",
			    _("--quit cannot be used with URIs."));
		goto out;
	}


	if (g_variant_dict_contains (options, "select") &&
	    !g_variant_dict_contains (options, G_OPTION_REMAINING)) {
		g_printerr ("%s\n",
			    _("--select must be used with at least an URI."));
		goto out;
	}

	if (g_variant_dict_contains (options, "force-desktop") &&
	    g_variant_dict_contains (options, "no-desktop")) {
		g_printerr ("%s\n",
			    _("--no-desktop and --force-desktop cannot be used together."));
		goto out;
	}

	retval = TRUE;

 out:
	return retval;
}

static int
do_perform_self_checks (void)
{
#ifndef NAUTILUS_OMIT_SELF_CHECK
	gtk_init (NULL, NULL);

	nautilus_profile_start (NULL);
	/* Run the checks (each twice) for nautilus and libnautilus-private. */

	nautilus_run_self_checks ();
	nautilus_run_lib_self_checks ();
	eel_exit_if_self_checks_failed ();

	nautilus_run_self_checks ();
	nautilus_run_lib_self_checks ();
	eel_exit_if_self_checks_failed ();
	nautilus_profile_end (NULL);
#endif

	return EXIT_SUCCESS;
}

static void
nautilus_application_select (NautilusApplication *self,
			     GFile **files,
			     gint len)
{
  int i;
  GFile *file;
  GFile *parent;

  for (i = 0; i < len; i++)
    {
      file = files[i];
      parent = g_file_get_parent (file);
      if (parent != NULL)
        {
          nautilus_application_open_location (self, parent, file, NULL);
          g_object_unref (parent);
        }
      else
        {
          nautilus_application_open_location (self, file, NULL, NULL);
        }
    }
}

const GOptionEntry options[] = {
#ifndef NAUTILUS_OMIT_SELF_CHECK
	{ "check", 'c', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Perform a quick set of self-check tests."), NULL },
#endif
	/* dummy, only for compatibility reasons */
	{ "browser", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, NULL,
	  NULL, NULL },
	/* ditto */
	{ "geometry", 'g', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, NULL,
	  N_("Create the initial window with the given geometry."), N_("GEOMETRY") },
	{ "version", '\0', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Show the version of the program."), NULL },
	{ "new-window", 'w', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Always open a new window for browsing specified URIs"), NULL },
	{ "no-default-window", 'n', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Only create windows for explicitly specified URIs."), NULL },
	{ "no-desktop", '\0', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Never manage the desktop (ignore the GSettings preference)."), NULL },
	{ "force-desktop", '\0', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Always manage the desktop (ignore the GSettings preference)."), NULL },
	{ "quit", 'q', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Quit Nautilus."), NULL },
	{ "select", 's', 0, G_OPTION_ARG_NONE, NULL,
	  N_("Select specified URI in parent folder."), NULL },
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, NULL, NULL,  N_("[URI...]") },

	{ NULL }
};

static void
nautilus_application_activate (GApplication *app)
{
	GFile **files;

	DEBUG ("Calling activate");

	files = g_malloc0 (2 * sizeof (GFile *));
	files[0] = g_file_new_for_path (g_get_home_dir ());
	nautilus_application_open (app, files, 1, NULL);

	g_object_unref (files[0]);
	g_free (files);
}

static gint
nautilus_application_handle_file_args (NautilusApplication *self,
				       GVariantDict        *options)
{
	GFile **files;
	GFile *file;
	gint idx, len;
	const gchar * const *remaining = NULL;
	GPtrArray *file_array;

	g_variant_dict_lookup (options, G_OPTION_REMAINING, "^a&s", &remaining);

	/* Convert args to GFiles */
	file_array = g_ptr_array_new_full (0, g_object_unref);

	if (remaining) {
		for (idx = 0; remaining[idx] != NULL; idx++) {
			file = g_file_new_for_commandline_arg (remaining[idx]);
			g_ptr_array_add (file_array, file);
		}
	} else if (g_variant_dict_contains (options, "new-window")) {
		file = g_file_new_for_path (g_get_home_dir ());
		g_ptr_array_add (file_array, file);
	} else {
		g_ptr_array_unref (file_array);

		/* No command line options or files, just activate the application */
		nautilus_application_activate (G_APPLICATION (self));
		return EXIT_SUCCESS;
	}

	len = file_array->len;
	files = (GFile **) file_array->pdata;

	if (g_variant_dict_contains (options, "select")) {
		nautilus_application_select (self, files, len);
	} else {
		/* Create new windows */
		nautilus_application_open (G_APPLICATION (self), files, len,
                                           g_variant_dict_contains (options, "new-window") ? "new-window" : "");
	}

	g_ptr_array_unref (file_array);

	return EXIT_SUCCESS;
}

static gint
nautilus_application_command_line (GApplication            *application,
                                   GApplicationCommandLine *command_line)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (application);
	gint retval = -1;
	GVariantDict *options;

	nautilus_profile_start (NULL);

	options = g_application_command_line_get_options_dict (command_line);

	if (g_variant_dict_contains (options, "version")) {
		g_application_command_line_print (command_line,
                                                  "GNOME nautilus " PACKAGE_VERSION "\n");
		retval = EXIT_SUCCESS;
		goto out;
	}

	if (!do_cmdline_sanity_checks (self, options)) {
		retval = EXIT_FAILURE;
		goto out;
	}

	if (g_variant_dict_contains (options, "check")) {
		retval = do_perform_self_checks ();
		goto out;
	}

	if (g_variant_dict_contains (options, "quit")) {
		DEBUG ("Killing application, as requested");
		g_action_group_activate_action (G_ACTION_GROUP (application),
						"kill", NULL);
		goto out;
	}

	if (g_variant_dict_contains (options, "force-desktop")) {
		DEBUG ("Forcing desktop, as requested");
		self->priv->desktop_override = TRUE;
		g_action_group_activate_action (G_ACTION_GROUP (application),
						"open-desktop", NULL);
	} else if (g_variant_dict_contains (options, "no-desktop")) {
		if (g_application_get_is_remote (application)) {
			DEBUG ("Not primary instance. Ignoring --no-desktop.");
		} else {
			DEBUG ("Forcing desktop off, as requested");
			self->priv->desktop_override = TRUE;
			g_action_group_activate_action (G_ACTION_GROUP (application),
							"close-desktop", NULL);
		}
	}

	if (g_variant_dict_contains (options, "no-default-window")) {
		/* Do nothing. If icons on desktop are enabled, it will create
		 * the desktop window which will hold the application. If not,
		 * it will just exit. */
		retval = EXIT_SUCCESS;
		goto out;
	}

	retval = nautilus_application_handle_file_args (self, options);

 out:
	nautilus_profile_end (NULL);

	return retval;
}

static void
nautilus_application_init (NautilusApplication *application)
{
	application->priv =
		G_TYPE_INSTANCE_GET_PRIVATE (application, NAUTILUS_TYPE_APPLICATION,
					     NautilusApplicationPriv);

	g_application_add_main_option_entries (G_APPLICATION (application), options);
}

static void
nautilus_application_set_desktop_visible (NautilusApplication *self,
					  gboolean             visible)
{
	const gchar *action_name;

	action_name = visible ? "open-desktop" : "close-desktop";
	g_action_group_activate_action (G_ACTION_GROUP (self),
					action_name, NULL);
}

static void
update_desktop_from_gsettings (NautilusApplication *self)
{
	GdkDisplay *display;
	gboolean visible;

	/* desktop GSetting was overridden - don't do anything */
	if (self->priv->desktop_override) {
		return;
	}

#ifdef GDK_WINDOWING_X11
	display = gdk_display_get_default ();
	visible = g_settings_get_boolean (gnome_background_preferences,
                                          NAUTILUS_PREFERENCES_SHOW_DESKTOP);
	if (!GDK_IS_X11_DISPLAY (display)) {
		if (visible)
			g_warning ("Desktop icons only supported on X11. Desktop not created");

		return;
	}

	nautilus_application_set_desktop_visible (self, visible);

	return;
#endif

	g_warning ("Desktop icons only supported on X11. Desktop not created");
}

static void
init_desktop (NautilusApplication *self)
{
	g_signal_connect_swapped (gnome_background_preferences, "changed::" NAUTILUS_PREFERENCES_SHOW_DESKTOP,
				  G_CALLBACK (update_desktop_from_gsettings),
				  self);
	update_desktop_from_gsettings (self);
}

static void
theme_changed (GtkSettings *settings)
{
	static GtkCssProvider *provider = NULL;
	gchar *theme;
	GdkScreen *screen;

	g_object_get (settings, "gtk-theme-name", &theme, NULL);
	screen = gdk_screen_get_default ();

	if (g_str_equal (theme, "Adwaita"))
	{
		if (provider == NULL)
		{
			GFile *file;

			provider = gtk_css_provider_new ();
			file = g_file_new_for_uri ("resource:///org/gnome/nautilus/Adwaita.css");
			gtk_css_provider_load_from_file (provider, file, NULL);
			g_object_unref (file);
		}

		gtk_style_context_add_provider_for_screen (screen,
							   GTK_STYLE_PROVIDER (provider),
							   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}
	else if (provider != NULL)
	{
		gtk_style_context_remove_provider_for_screen (screen,
							      GTK_STYLE_PROVIDER (provider));
		g_clear_object (&provider);
	}

	g_free (theme);
}

static void
setup_theme_extensions (void)
{
	GtkSettings *settings;

	/* Set up a handler to load our custom css for Adwaita.
	 * See https://bugzilla.gnome.org/show_bug.cgi?id=732959
	 * for a more automatic solution that is still under discussion.
	 */
	settings = gtk_settings_get_default ();
	g_signal_connect (settings, "notify::gtk-theme-name", G_CALLBACK (theme_changed), NULL);
	theme_changed (settings);
}

static void
nautilus_application_startup (GApplication *app)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (app);

	nautilus_profile_start (NULL);

	/* chain up to the GTK+ implementation early, so gtk_init()
	 * is called for us.
	 */
	G_APPLICATION_CLASS (nautilus_application_parent_class)->startup (app);

	gtk_window_set_default_icon_name ("system-file-manager");

	setup_theme_extensions ();

	/* create DBus manager */
	self->priv->fdb_manager = nautilus_freedesktop_dbus_new ();

	/* initialize preferences and create the global GSettings objects */
	nautilus_global_preferences_init ();

	/* register property pages */
	nautilus_image_properties_page_register ();

	/* initialize nautilus modules */
	nautilus_profile_start ("Modules");
	nautilus_module_setup ();
	nautilus_profile_end ("Modules");

	/* attach menu-provider module callback */
	menu_provider_init_callback ();
	
	/* Initialize the UI handler singleton for file operations */
	self->priv->progress_handler = nautilus_progress_persistence_handler_new (G_OBJECT (self));

	/* Check the user's .nautilus directories and post warnings
	 * if there are problems.
	 */
	check_required_directories (self);

	do_upgrades_once (self);

	nautilus_init_application_actions (self);
	init_desktop (self);

	nautilus_profile_end (NULL);
}

static gboolean
nautilus_application_dbus_register (GApplication	 *app,
				    GDBusConnection      *connection,
				    const gchar		 *object_path,
				    GError		**error)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (app);

	self->priv->dbus_manager = nautilus_dbus_manager_new ();
	if (!nautilus_dbus_manager_register (self->priv->dbus_manager, connection, error)) {
		return FALSE;
	}

	self->priv->search_provider = nautilus_shell_search_provider_new ();
	if (!nautilus_shell_search_provider_register (self->priv->search_provider, connection, error)) {
		return FALSE;
	}

	return TRUE;
}

static void
nautilus_application_dbus_unregister (GApplication	*app,
				      GDBusConnection   *connection,
				      const gchar	*object_path)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (app);

	if (self->priv->dbus_manager) {
		nautilus_dbus_manager_unregister (self->priv->dbus_manager);
	}

	if (self->priv->search_provider) {
		nautilus_shell_search_provider_unregister (self->priv->search_provider);
	}
}

static void
nautilus_application_quit_mainloop (GApplication *app)
{
	DEBUG ("Quitting mainloop");

	nautilus_icon_info_clear_caches ();

	G_APPLICATION_CLASS (nautilus_application_parent_class)->quit_mainloop (app);
}

static void
update_dbus_opened_locations (NautilusApplication *app)
{
	gint i;
	GList *l, *sl;
	GList *locations = NULL;
	gsize locations_size = 0;
	gchar **locations_array;
	NautilusWindow *window;
        GFile *location;

	g_return_if_fail (NAUTILUS_IS_APPLICATION (app));

	for (l = app->priv->windows; l != NULL; l = l->next) {
		window = l->data;

		for (sl = nautilus_window_get_slots (window); sl; sl = sl->next) {
			NautilusWindowSlot *slot = sl->data;
                        location = nautilus_window_slot_get_location (slot);

			if (location != NULL) {
			        gchar *uri = g_file_get_uri (location);
				GList *found = g_list_find_custom (locations, uri, (GCompareFunc) g_strcmp0);

				if (!found) {
					locations = g_list_prepend (locations, uri);
					++locations_size;
				} else {
					g_free (uri);
				}
			}
		}
	}

	locations_array = g_new (gchar*, locations_size + 1);

	for (i = 0, l = locations; l; l = l->next, ++i) {
		/* We reuse the locations string locations saved on list */
		locations_array[i] = l->data;
	}

	locations_array[locations_size] = NULL;

	nautilus_freedesktop_dbus_set_open_locations (app->priv->fdb_manager,
		                                      (const gchar**) locations_array);

	g_free (locations_array);
	g_list_free_full (locations, g_free);
}

static void
on_slot_location_changed (NautilusWindowSlot  *slot,
                          GParamSpec          *pspec,
                          NautilusApplication *application)
{
	update_dbus_opened_locations (application);
}

static void
on_slot_added (NautilusWindow      *window,
	       NautilusWindowSlot  *slot,
	       NautilusApplication *application)
{
	if (nautilus_window_slot_get_location (slot)) {
		update_dbus_opened_locations (application);
	}

	g_signal_connect (slot, "notify::location", G_CALLBACK (on_slot_location_changed), application);
}

static void
on_slot_removed (NautilusWindow      *window,
		 NautilusWindowSlot  *slot,
		 NautilusApplication *application)
{
	update_dbus_opened_locations (application);

	g_signal_handlers_disconnect_by_func (slot, on_slot_location_changed, application);
}

static void
nautilus_application_window_added (GtkApplication *app,
				   GtkWindow *window)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (app);

	GTK_APPLICATION_CLASS (nautilus_application_parent_class)->window_added (app, window);

	if (NAUTILUS_IS_WINDOW (window)) {
		self->priv->windows = g_list_prepend (self->priv->windows, window);
		g_signal_connect (window, "slot-added", G_CALLBACK (on_slot_added), app);
		g_signal_connect (window, "slot-removed", G_CALLBACK (on_slot_removed), app);
	}
}

static void
nautilus_application_window_removed (GtkApplication *app,
				     GtkWindow *window)
{
	NautilusApplication *self = NAUTILUS_APPLICATION (app);

	GTK_APPLICATION_CLASS (nautilus_application_parent_class)->window_removed (app, window);

	if (NAUTILUS_IS_WINDOW (window)) {
		self->priv->windows = g_list_remove_all (self->priv->windows, window);
		g_signal_handlers_disconnect_by_func (window, on_slot_added, app);
		g_signal_handlers_disconnect_by_func (window, on_slot_removed, app);
	}

	/* if this was the last window, close the previewer */
	if (g_list_length (self->priv->windows) == 0) {
		nautilus_previewer_call_close ();
                nautilus_progress_persistence_handler_make_persistent (self->priv->progress_handler);
	}
}

static void
nautilus_application_class_init (NautilusApplicationClass *class)
{
        GObjectClass *object_class;
	GApplicationClass *application_class;
	GtkApplicationClass *gtkapp_class;

        object_class = G_OBJECT_CLASS (class);
        object_class->finalize = nautilus_application_finalize;

	application_class = G_APPLICATION_CLASS (class);
	application_class->startup = nautilus_application_startup;
	application_class->activate = nautilus_application_activate;
	application_class->quit_mainloop = nautilus_application_quit_mainloop;
	application_class->dbus_register = nautilus_application_dbus_register;
	application_class->dbus_unregister = nautilus_application_dbus_unregister;
	application_class->open = nautilus_application_open;
	application_class->command_line = nautilus_application_command_line;

	gtkapp_class = GTK_APPLICATION_CLASS (class);
	gtkapp_class->window_added = nautilus_application_window_added;
	gtkapp_class->window_removed = nautilus_application_window_removed;

	g_type_class_add_private (class, sizeof (NautilusApplicationPriv));
}

NautilusApplication *
nautilus_application_new (void)
{
	return g_object_new (NAUTILUS_TYPE_APPLICATION,
			     "application-id", "org.gnome.Nautilus",
			     "flags", G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_HANDLES_OPEN,
			     "inactivity-timeout", 12000,
			     "register-session", TRUE,
			     NULL);
}
