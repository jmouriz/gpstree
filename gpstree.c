/* gcc -o gpstree gpstree.c $(pkg-config --cflags --libs gtk+-2.0) */
#include <gtk/gtk.h>
#include <stdlib.h>

/*
 * State of a process:
 *
 *   D    Uninterruptible sleep (usually IO)
 *   R    Running or runnable (on run queue)
 *   S    Interruptible sleep (waiting for an event to complete)
 *   T    Stopped, either by a job control signal or because it is being traced.
 *   W    paging (not valid since the 2.6.xx kernel)
 *   X    dead (should never be seen)
 *   Z    Defunct ("zombie") process, terminated but not reaped by its parent.
 *
 * For BSD formats and when the stat keyword is used, additional characters may be displayed:
 *
 *   <    high-priority (not nice to other users)
 *   N    low-priority (nice to other users)
 *   L    has pages locked into memory (for real-time and custom IO)
 *   s    is a session leader
 *   l    is multi-threaded (using CLONE_THREAD, like NPTL pthreads do)
 *   +    is in the foreground process group
 */

#define PROCESS_DATA(process_data) ((ProcessData *) process_data)

typedef struct _ProecssData ProcessData;

struct _ProecssData
{
	gchar *process_id;
	gchar *parent_process_id;
	gchar *owner;
	gchar *time_elapsed;
	gchar *memory_ratio;
	gchar *cpu_usage;
	gchar *state;
	gchar *command;

	GtkTreeIter iter;
};

enum
{
  PROCESS_ID_COLUMN,
  PARENT_PROCESS_ID_COLUMN,
  OWNER_COLUMN,
  TIME_ELAPSED_COLUMN,
  MEMORY_RATIO_COLUMN,
  CPU_USAGE_COLUMN,
  STATE_COLUMN,
  COMMAND_COLUMN,

  N_COLUMNS
};

static GSList *process_list = NULL;

static void parse_process_data (gchar *string);
static gint find_parent (gconstpointer process_data, gconstpointer parent_process_id);
static void free_process_list (gpointer element, gpointer data);
static void set_store (GtkTreeStore *store);
static void child_watch (GPid pid, gint status, gpointer data);
static gboolean output_watch (GIOChannel *channel, GIOCondition cond, GtkTreeView *tree);
static gboolean error_watch (GIOChannel *channel, GIOCondition cond, gpointer data);
static void execute (GtkTreeView *tree);

int
main (int argc, char **argv)
{
	GtkWidget *window;
	GtkWidget *scrolled;
	GtkWidget *tree;
	GtkCellRenderer *cell;
	GtkTreeViewColumn *column;
	GtkTreeModel *model;
	GtkTreeStore *store;
	GdkScreen *screen;
	gint width;
	gint height;

	gtk_init (&argc, &argv);

	/* tree store */
	store = gtk_tree_store_new (N_COLUMNS,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING);

	/* main window */
	window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

	gtk_window_set_title (GTK_WINDOW (window), "Explorador de procesos");

	/* screen/window ratio */
	screen = gdk_screen_get_default ();

	width = gdk_screen_get_width (screen);
	height = gdk_screen_get_height (screen);

	width *= 0.75;
	height /= 2;

	gtk_widget_set_size_request (window, width, height);
	gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_CENTER);

	/* scrolled window */
	scrolled = gtk_scrolled_window_new (NULL, NULL);

	/* tree model */
	model = GTK_TREE_MODEL (store);

	/* tree view */
	tree = gtk_tree_view_new_with_model (model);

	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (tree), TRUE);
	gtk_tree_view_set_enable_tree_lines (GTK_TREE_VIEW (tree), TRUE);
	gtk_tree_view_set_rubber_banding (GTK_TREE_VIEW (tree), TRUE);
	gtk_tree_view_set_grid_lines (GTK_TREE_VIEW (tree), GTK_TREE_VIEW_GRID_LINES_NONE);

	/* process id column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"width-chars", 10,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("pid", cell, "text", PROCESS_ID_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* owner column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"width-chars", 10,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("owner", cell, "text", OWNER_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* elapsed time column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"width-chars", 15,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("time", cell, "text", TIME_ELAPSED_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* memory ratio column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"width-chars", 5,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("mem (%)", cell, "text", MEMORY_RATIO_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* cpu usage column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"width-chars", 5,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("cpu (%)", cell, "text", CPU_USAGE_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* state column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		"width-chars", 5,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("state", cell, "text", STATE_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* command column */
	cell = gtk_cell_renderer_text_new ();

	g_object_set (cell,
		"editable", FALSE,
		"ellipsize", PANGO_ELLIPSIZE_END,
		NULL);

	column = gtk_tree_view_column_new_with_attributes ("command", cell, "text", COMMAND_COLUMN, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);
	gtk_tree_view_set_expander_column (GTK_TREE_VIEW (tree), column);

	gtk_tree_view_set_search_column (GTK_TREE_VIEW (tree), COMMAND_COLUMN);

  execute (GTK_TREE_VIEW (tree));

	/* put all toegher */
	gtk_container_add (GTK_CONTAINER (scrolled), tree);
	gtk_container_add (GTK_CONTAINER (window), scrolled);

	g_signal_connect (G_OBJECT (window), "delete-event", G_CALLBACK (gtk_main_quit), NULL);

	gtk_widget_show_all (window);

	gtk_main ();

	return (EXIT_SUCCESS);
}

static void
parse_process_data (gchar *string)
{
	GError *error = NULL;
	GRegex *regex;
	GMatchInfo *matches;

	//regex = g_regex_new ("^ *([0-9]+) +([0-9]+) +([^ ]+) +([0-9]+-[0-9]{2}:[0-9]{2}:[0-9]{2}) +"
	//					"([0-9]+\\.[0-9]+) +([0-9]+\\.[0-9]+) +([DRSTWXZ][NLsl\\+<]*) +(.+)$", 0, 0, &error);

	regex = g_regex_new ("^ *([0-9]+) +([0-9]+) +([^ ]+) +([^ ]+) +([0-9]+\\.[0-9]+) +"
						"([0-9]+\\.[0-9]+) +([DRSTWXZ][NLsl\\+<]*) +(.+)$", 0, 0, &error);

	if (error)
	{
		g_error ("%s", error->message);
		g_error_free (error);
	}
	else
	{
		gboolean result;

		result = g_regex_match (regex, string, 0, &matches); /* XXX */

		if (result)
		{
			ProcessData *process_data = g_new0 (ProcessData, 1);

			process_data->process_id = g_match_info_fetch (matches, PROCESS_ID_COLUMN+1);
			process_data->parent_process_id = g_match_info_fetch (matches, PARENT_PROCESS_ID_COLUMN+1);
			process_data->owner = g_match_info_fetch (matches, OWNER_COLUMN+1);
			process_data->time_elapsed = g_match_info_fetch (matches, TIME_ELAPSED_COLUMN+1);
			process_data->memory_ratio = g_match_info_fetch (matches, MEMORY_RATIO_COLUMN+1);
			process_data->cpu_usage = g_match_info_fetch (matches, CPU_USAGE_COLUMN+1);
			process_data->state = g_match_info_fetch (matches, STATE_COLUMN+1);
			process_data->command = g_match_info_fetch (matches, COMMAND_COLUMN+1);

			process_list = g_slist_append (process_list, process_data);
		}

		g_match_info_matches (matches);
	}
 
	g_regex_unref (regex);
}

static gint
find_parent (gconstpointer process_data, gconstpointer parent_process_id)
{
	if (g_str_equal (PROCESS_DATA (process_data)->process_id, (gchar *) parent_process_id))
		return (0);

	return (1);
}

static void
free_process_list (gpointer element, gpointer data)
{
	g_free (PROCESS_DATA (element)->process_id);
	g_free (PROCESS_DATA (element)->parent_process_id);
	g_free (PROCESS_DATA (element)->owner);
	g_free (PROCESS_DATA (element)->time_elapsed);
	g_free (PROCESS_DATA (element)->memory_ratio);
	g_free (PROCESS_DATA (element)->cpu_usage);
	g_free (PROCESS_DATA (element)->state);
	g_free (PROCESS_DATA (element)->command);

	g_free (element);
}

static void
set_store (GtkTreeStore *store)
{
	GSList *list = process_list;

	while (list != NULL)
	{
		ProcessData *process_data = PROCESS_DATA (list->data);

		GSList *parent_process = g_slist_find_custom (process_list, process_data->parent_process_id, find_parent);

		if (parent_process)
		{
			ProcessData *parent_process_data = PROCESS_DATA (parent_process->data);

			GtkTreeIter parent = parent_process_data->iter;

		  gtk_tree_store_append (store, &process_data->iter, &parent);
		}
		else
		{
			gtk_tree_store_append (store, &process_data->iter, NULL);
		}
		
		gtk_tree_store_set (store, &process_data->iter, PROCESS_ID_COLUMN, process_data->process_id, -1);
		gtk_tree_store_set (store, &process_data->iter, PARENT_PROCESS_ID_COLUMN, process_data->parent_process_id, -1);
		gtk_tree_store_set (store, &process_data->iter, OWNER_COLUMN, process_data->owner, -1);
		gtk_tree_store_set (store, &process_data->iter, TIME_ELAPSED_COLUMN, process_data->time_elapsed, -1);
		gtk_tree_store_set (store, &process_data->iter, MEMORY_RATIO_COLUMN, process_data->memory_ratio, -1);
		gtk_tree_store_set (store, &process_data->iter, CPU_USAGE_COLUMN, process_data->cpu_usage, -1);
		gtk_tree_store_set (store, &process_data->iter, STATE_COLUMN, process_data->state, -1);
		gtk_tree_store_set (store, &process_data->iter, COMMAND_COLUMN, process_data->command, -1);

		list = g_slist_next (list);
	}

	g_slist_foreach (process_list, free_process_list, NULL);
}

static void
child_watch (GPid pid, gint status, gpointer data)
{
	g_spawn_close_pid (pid);
}

static gboolean
output_watch (GIOChannel *channel, GIOCondition cond, GtkTreeView *tree)
{
	gchar *string;
	gsize size;

	if (cond == G_IO_HUP)
	{
		g_io_channel_unref (channel);

		GtkTreeStore *store = GTK_TREE_STORE (gtk_tree_view_get_model (tree));

		set_store (store);

		gtk_tree_view_expand_all (tree);

		return (FALSE);
	}

	g_io_channel_read_line (channel, &string, &size, NULL, NULL);

	parse_process_data (string);

	g_free (string);

	return (TRUE);
}

static gboolean
error_watch (GIOChannel *channel, GIOCondition cond, gpointer data)
{
	gchar *string;
	gsize size;

	if (cond == G_IO_HUP)
	{
		g_io_channel_unref (channel);
		return (FALSE);
	}

	g_io_channel_read_line (channel, &string, &size, NULL, NULL);

	/* TODO */

	g_free (string);

	return (TRUE);
}

static void
execute (GtkTreeView *tree)
{
	gchar *command[] = { "/bin/ps", "h", "-eo", "pid,ppid,user,etime,%mem,%cpu,stat,cmd", NULL };

	GIOChannel *output_channel;
	GIOChannel *error_channel;
	GPid process_id;
	gint process_output;
	gint process_error;
	gboolean result;

	result = g_spawn_async_with_pipes (NULL, command, NULL,
		G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &process_id,
		NULL, &process_output, &process_error, NULL);

	if (!result)
	{
		g_error ("g_spawn_async_with_pipes failed");
		return;
	}

	g_child_watch_add (process_id, (GChildWatchFunc) child_watch, NULL);

#ifdef G_OS_WIN32
	output_channel = g_io_channel_win32_new_fd (process_output);
	error_channel = g_io_channel_win32_new_fd (process_error);
#else
	output_channel = g_io_channel_unix_new (process_output);
	error_channel = g_io_channel_unix_new (process_error);
#endif

	g_io_add_watch (output_channel, G_IO_IN | G_IO_HUP, (GIOFunc) output_watch, tree);
	g_io_add_watch (error_channel, G_IO_IN | G_IO_HUP, (GIOFunc) error_watch, NULL);
}
