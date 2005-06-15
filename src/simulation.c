/*
 * simulation.c
 *
 *
 * Authors:
 *	Richard Hult <rhult@hem.passagen.se>
 *	Ricardo Markiewicz <rmarkie@fi.uba.ar>
 *	Andres de Barbara <adebarbara@fi.uba.ar>
 *
 * Web page: http://arrakis.lug.fi.uba.ar/
 *
 * Copyright (C) 1999-2001	Richard Hult
 * Copyright (C) 2003,2005	LUGFI
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
	 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
	 * Boston, MA 02111-1307, USA.
 */

#include <config.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <glade/glade.h>

#include "main.h"
#include "simulation.h"
#include "schematic.h"
#include "schematic-view.h"
#include "dialogs.h"
#include "oregano-utils.h"
#include "oregano-config.h"
#include "sim-engine.h"
#include "plot.h"

typedef struct {
	Schematic *sm;
	SchematicView *sv;
	GtkDialog *dialog;
	SimEngine *engine;
	GtkProgressBar *progress;
	int				progress_timeout_id;
} Simulation;

static int progress_bar_timeout_callback (Simulation *s);
static void cancel_callback (GtkWidget *widget, ging arg1, Simulation *s);
static void input_done_callback (SimEngine *engine, Simulation *s);
static void input_aborted_callback (SimEngine *engine, Simulation *s);
static gboolean simulate_cmd (Simulation *s);

static int
delete_event_callback (GtkWidget *widget, GdkEvent *event, gpointer data)
{
	return FALSE;
}

gpointer
simulation_new (Schematic *sm)
{
	Simulation *s;

	s = g_new0 (Simulation, 1);
	s->sm = sm;
	s->sv = NULL;

	return s;
}

void
simulation_show (GtkWidget *widget, SchematicView *sv)
{
	GtkWidget *w;
	GladeXML *gui;
	Simulation *s;
	Schematic *sm;
	gchar *msg;
	gchar *fullpath = NULL;

	g_return_if_fail (sv != NULL);

	sm = schematic_view_get_schematic (sv);
	s = schematic_get_simulation (sm);

/*
 * Only allow one instance of the dialog box per schematic.
 */
	if (s->dialog){
		gdk_window_raise (GTK_WIDGET (s->dialog)->window);
		return;
	}

	fullpath = g_find_program_in_path (oregano.simexec);
	if (oregano.simexec && !fullpath) {
		msg = g_strdup_printf (_("<span weight=\"bold\" size=\"x-large\">Could not find the simulation executable</span>\n\n"
			"This probably means that you have not configured Oregano properly."));
		oregano_error (msg);
		g_free (msg);
		return;
	} else if (!oregano.simexec) {
		oregano_error (_("<span weight=\"bold\" size=\"x-large\">You have not entered a simulation executable.</span>\n\n"
			"Please choose Settings and specify which"
			"program to use for simulations."));
		if (fullpath != NULL) g_free (fullpath);
		return;
	}

	if (fullpath != NULL) g_free (fullpath);
	if (!g_file_test (OREGANO_GLADEDIR "/simulation.glade2",
		G_FILE_TEST_EXISTS)) {
		oregano_error (_("Could not create simulation dialog."));
		return;
	}

	gui = glade_xml_new (OREGANO_GLADEDIR "/simulation.glade2", "toplevel", NULL);

	if (!gui) {
		oregano_error (_("<span weight=\"bold\" size=\"x-large\">Could not create simulation dialog.</span>"));
		return;
	}

	w = glade_xml_get_widget (gui, "toplevel");
	if (!w) {
		oregano_error (_("<span weight=\"bold\" size=\"x-large\">Could not create simulation dialog.</span>"));
		return;
	}

	s->dialog = GTK_DIALOG (w);
	g_signal_connect (G_OBJECT (w),
		"delete_event",
		G_CALLBACK (delete_event_callback),
		s);

	w = glade_xml_get_widget (gui, "progressbar");
	s->progress = GTK_PROGRESS_BAR (w);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (s->progress), 0.0);

	g_signal_connect (G_OBJECT (s->dialog),
		"response",
		G_CALLBACK (cancel_callback),
		s);

	gtk_widget_show_all (GTK_WIDGET (s->dialog));

	s->sv = sv;
	simulate_cmd (s);
}

static int
progress_bar_timeout_callback (Simulation *s)
{
	g_return_val_if_fail (s != NULL, FALSE);

	if (s->engine->progress >= 1)
		s->engine->progress = 0;

	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (s->progress),
		s->engine->progress);

	s->engine->progress += 0.1;

	return TRUE;
}

static void
input_done_callback (SimEngine *engine, Simulation *s)
{
	if (s->progress_timeout_id != 0) {
		gtk_timeout_remove (s->progress_timeout_id); // No existe la version de g_* Uhmmm.
		s->progress_timeout_id = 0;

/* Make sure that the progress bar is completed, just for good looks. */
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (s->progress), 1.0);
//	gtk_widget_draw (GTK_WIDGET (s->progress), NULL); // Como arriba
	}

	gtk_widget_destroy (GTK_WIDGET (s->dialog));
	s->dialog = NULL;

	plot_show (s->engine);

	if (s->engine->has_warnings) {
		schematic_view_log_show (s->sv, FALSE);
	}

	schematic_view_clear_op_values (s->sv);
	schematic_view_show_op_values (s->sv, s->engine);
}

static void
input_aborted_callback (SimEngine *engine, Simulation *s)
{
	GtkWidget *dialog;
	int answer;

	if (s->progress_timeout_id != 0) {
		gtk_timeout_remove (s->progress_timeout_id);
		s->progress_timeout_id = 0;
	}

	gtk_widget_destroy (GTK_WIDGET (s->dialog));
	s->dialog = NULL;

	if (!schematic_view_log_window_exists (s->sv)) {

		dialog = gtk_message_dialog_new_with_markup (
			GTK_WINDOW (s->sv->toplevel),
			GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_ERROR,
			GTK_BUTTONS_YES_NO,
			_("<span weight=\"bold\" size=\"x-large\">The simulation was aborted due to an error.</span>\n\n"
				"Would you like to view the error log?"));


		answer = gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);

		if (answer == GTK_RESPONSE_YES) {
			/*
			 * Show logs.
			 */
			schematic_view_log_show (s->sv, TRUE);
		}
	} else {
		oregano_error (_("<span weight=\"bold\" size=\"x-large\"> The simulation was aborted due to an error.</span>\n"));

		schematic_view_log_show (s->sv, FALSE);
	}
}

static void
cancel_callback (GtkWidget *widget, gint arg1, Simulation *s)
{
	g_return_if_fail (s != NULL);

	if (s->progress_timeout_id != 0) {
	/* Watchout, the timer is destroyed when returns FALSE - Remove! */
		gtk_timeout_remove (s->progress_timeout_id);
		s->progress_timeout_id = 0;
	}

	if (s->engine)
		sim_engine_stop (s->engine);

	gtk_widget_destroy (GTK_WIDGET (s->dialog));
	s->dialog = NULL;
	s->sv = NULL;
}

static gboolean
simulate_cmd (Simulation *s)
{
	SimEngine *engine;
	char *netlist_filename;

	if (s->engine != NULL) {
		g_object_unref(G_OBJECT(s->engine));
		s->engine = NULL;
	}

	netlist_filename = schematic_get_netlist_filename (s->sm);
	if (netlist_filename == NULL) {
		return FALSE;
	}

	engine = sim_engine_new (s->sm);
	s->engine = engine;

	s->progress_timeout_id = g_timeout_add (
		100,
		(gpointer) progress_bar_timeout_callback,
		(gpointer) s);

	g_signal_connect (G_OBJECT (engine),
		"done",
		G_CALLBACK (input_done_callback),
		s);
	g_signal_connect (G_OBJECT (engine),
		"aborted",
		G_CALLBACK (input_aborted_callback),
		s);

	sim_engine_start_with_file (engine, netlist_filename);

	return TRUE;
}
