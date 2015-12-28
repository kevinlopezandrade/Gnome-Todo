/* gtd-window.c
 *
 * Copyright (C) 2015 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "interfaces/gtd-provider.h"
#include "interfaces/gtd-panel.h"
#include "gtd-application.h"
#include "gtd-enum-types.h"
#include "gtd-task-list-view.h"
#include "gtd-manager.h"
#include "gtd-notification.h"
#include "gtd-notification-widget.h"
#include "gtd-provider-dialog.h"
#include "gtd-task.h"
#include "gtd-task-list.h"
#include "gtd-task-list-item.h"
#include "gtd-window.h"

#include <glib/gi18n.h>

typedef struct
{
  GtkWidget                     *action_bar;
  GtkButton                     *back_button;
  GtkWidget                     *cancel_selection_button;
  GtkColorButton                *color_button;
  GtkWidget                     *gear_menu_button;
  GtkHeaderBar                  *headerbar;
  GtkFlowBox                    *lists_flowbox;
  GtkStack                      *main_stack;
  GtkWidget                     *new_list_button;
  GtkWidget                     *new_list_popover;
  GtdNotificationWidget         *notification_widget;
  GtkWidget                     *remove_button;
  GtkWidget                     *rename_button;
  GtkSearchBar                  *search_bar;
  GtkToggleButton               *search_button;
  GtkSearchEntry                *search_entry;
  GtkWidget                     *select_button;
  GtkStack                      *stack;
  GtkStackSwitcher              *stack_switcher;
  GtdProviderDialog             *provider_dialog;
  GtdTaskListView               *list_view;

  /* rename popover */
  GtkWidget                     *rename_entry;
  GtkWidget                     *rename_popover;
  GtkWidget                     *save_rename_button;

  /* mode */
  GtdWindowMode                  mode;

  /* loading notification */
  GtdNotification               *loading_notification;

  guint                          save_geometry_timeout_id;
  GtdManager                    *manager;
} GtdWindowPrivate;

struct _GtdWindow
{
  GtkApplicationWindow  application;

  /*< private >*/
  GtdWindowPrivate     *priv;
};

#define              SAVE_GEOMETRY_ID_TIMEOUT                    100 /* ms */

static void          gtd_window__create_new_list                 (GSimpleAction         *simple,
                                                                  GVariant              *parameter,
                                                                  gpointer               user_data);

static void          gtd_window__change_storage_action           (GSimpleAction         *simple,
                                                                  GVariant              *parameter,
                                                                  gpointer               user_data);

G_DEFINE_TYPE_WITH_PRIVATE (GtdWindow, gtd_window, GTK_TYPE_APPLICATION_WINDOW)

static const GActionEntry gtd_window_entries[] = {
  { "change-storage", gtd_window__change_storage_action },
  { "new-list", gtd_window__create_new_list }
};

enum {
  PROP_0,
  PROP_MANAGER,
  PROP_MODE,
  LAST_PROP
};

/* GtdManager's error notifications */
typedef struct
{
  GtdWindow *window;
  gchar     *primary_text;
  gchar     *secondary_text;
} ErrorData;

static void
gtd_window__panel_title_changed (GObject    *object,
                                 GParamSpec *pspec,
                                 GtdWindow  *window)
{
  GtdWindowPrivate *priv = gtd_window_get_instance_private (window);

  gtk_container_child_set (GTK_CONTAINER (priv->stack),
                           GTK_WIDGET (object),
                           "title", gtd_panel_get_title (GTD_PANEL (object)),
                           NULL);
}

static void
gtd_window__panel_added (GtdManager *manager,
                         GtdPanel   *panel,
                         GtdWindow  *window)
{
  GtdWindowPrivate *priv = gtd_window_get_instance_private (window);

  gtk_stack_add_titled (priv->stack,
                        GTK_WIDGET (panel),
                        gtd_panel_get_name (panel),
                        gtd_panel_get_title (panel));

  g_signal_connect (panel,
                    "notify::title",
                    G_CALLBACK (gtd_window__panel_title_changed),
                    window);
}

static void
gtd_window__panel_removed (GtdManager *manager,
                           GtdPanel   *panel,
                           GtdWindow  *window)
{
  GtdWindowPrivate *priv = gtd_window_get_instance_private (window);

  gtk_container_remove (GTK_CONTAINER (priv->stack), GTK_WIDGET (panel));
}

static void
error_data_free (ErrorData *error_data)
{
  g_free (error_data->primary_text);
  g_free (error_data->secondary_text);
  g_free (error_data);
}

static void
error_message_notification_primary_action (GtdNotification *notification,
                                           gpointer         user_data)
{
  error_data_free (user_data);
}

static void
error_message_notification_secondary_action (GtdNotification *notification,
                                             gpointer         user_data)
{
  GtkWidget *message_dialog;
  ErrorData *data;

  data = user_data;
  message_dialog = gtk_message_dialog_new (GTK_WINDOW (data->window),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_WARNING,
                                           GTK_BUTTONS_CLOSE,
                                           "%s",
                                           data->primary_text);

  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (message_dialog),
                                            "%s",
                                            data->secondary_text);

  g_signal_connect (message_dialog,
                    "response",
                    G_CALLBACK (gtk_widget_destroy),
                    NULL);

  gtk_widget_show (message_dialog);

  error_data_free (data);
}

static void
gtd_window__load_geometry (GtdWindow *window)
{
  GSettings *settings;
  GVariant *variant;
  gboolean maximized;
  const gint32 *position;
  const gint32 *size;
  gsize n_elements;

  settings = gtd_manager_get_settings (window->priv->manager);

  /* load window settings: size */
  variant = g_settings_get_value (settings,
                                  "window-size");
  size = g_variant_get_fixed_array (variant,
                                    &n_elements,
                                    sizeof (gint32));
  if (n_elements == 2)
    gtk_window_set_default_size (GTK_WINDOW (window),
                                 size[0],
                                 size[1]);
  g_variant_unref (variant);

  /* load window settings: position */
  variant = g_settings_get_value (settings,
                                  "window-position");
  position = g_variant_get_fixed_array (variant,
                                        &n_elements,
                                        sizeof (gint32));
  if (n_elements == 2)
    gtk_window_move (GTK_WINDOW (window),
                     position[0],
                     position[1]);

  g_variant_unref (variant);

  /* load window settings: state */
  maximized = g_settings_get_boolean (settings,
                                      "window-maximized");
  if (maximized)
    gtk_window_maximize (GTK_WINDOW (window));
}

static gboolean
gtd_window__save_geometry (gpointer user_data)
{
  GtdWindowPrivate *priv;
  GdkWindowState state;
  GdkWindow *window;
  GtkWindow *self;
  GSettings *settings;
  gboolean maximized;
  GVariant *variant;
  gint32 size[2];
  gint32 position[2];

  self = GTK_WINDOW (user_data);

  window = gtk_widget_get_window (GTK_WIDGET (self));
  state = gdk_window_get_state (window);
  priv = GTD_WINDOW (self)->priv;

  settings = gtd_manager_get_settings (priv->manager);

  /* save window's state */
  maximized = state & GDK_WINDOW_STATE_MAXIMIZED;
  g_settings_set_boolean (settings,
                          "window-maximized",
                          maximized);

  if (maximized)
    {
      priv->save_geometry_timeout_id = 0;
      return FALSE;
    }

  /* save window's size */
  gtk_window_get_size (self,
                       (gint *) &size[0],
                       (gint *) &size[1]);
  variant = g_variant_new_fixed_array (G_VARIANT_TYPE_INT32,
                                       size,
                                       2,
                                       sizeof (size[0]));
  g_settings_set_value (settings,
                        "window-size",
                        variant);

  /* save windows's position */
  gtk_window_get_position (self,
                           (gint *) &position[0],
                           (gint *) &position[1]);
  variant = g_variant_new_fixed_array (G_VARIANT_TYPE_INT32,
                                       position,
                                       2,
                                       sizeof (position[0]));
  g_settings_set_value (settings,
                        "window-position",
                        variant);

  priv->save_geometry_timeout_id = 0;

  return FALSE;
}

static GtdTaskListItem*
get_selected_list (GtdWindow *window)
{
  GtdWindowPrivate *priv;
  GtdTaskListItem *item;
  GList *children;
  GList *l;

  priv = window->priv;
  item = NULL;

  /* Retrieve the only selected task list */
  children = gtk_container_get_children (GTK_CONTAINER (priv->lists_flowbox));

  for (l = children; l != NULL; l = l->next)
    {
      if (gtd_task_list_item_get_selected (l->data))
        {
          item = l->data;
          break;
        }
    }

  g_list_free (children);

  return item;
}

static void
gtd_window__remove_button_clicked (GtdWindow *window)
{
  GtdWindowPrivate *priv;
  GtkWidget *dialog;
  GtkWidget *button;
  GList *children;
  GList *l;
  gint response;

  priv = window->priv;
  dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR,
                                   GTK_MESSAGE_QUESTION,
                                   GTK_BUTTONS_NONE,
                                   _("Remove the selected task lists?"));

  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                            _("Once removed, the task lists cannot be recovered."));

  /* Focus the Cancel button by default */
  gtk_dialog_add_button (GTK_DIALOG (dialog),
                         _("Cancel"),
                         GTK_RESPONSE_CANCEL);

  button = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);
  gtk_widget_grab_focus (button);

  /* Make the Remove button visually destructive */
  gtk_dialog_add_button (GTK_DIALOG (dialog),
                         _("Remove task lists"),
                         GTK_RESPONSE_ACCEPT);

  button = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  gtk_style_context_add_class (gtk_widget_get_style_context (button), "destructive-action");

  response = gtk_dialog_run (GTK_DIALOG (dialog));

  /* Remove selected lists */
  if (response == GTK_RESPONSE_ACCEPT)
    {
      children = gtk_container_get_children (GTK_CONTAINER (priv->lists_flowbox));

      for (l = children; l != NULL; l = l->next)
        {
          if (gtd_task_list_item_get_selected (l->data))
            {
              GtdTaskList *list;

              list = gtd_task_list_item_get_list (l->data);

              if (gtd_task_list_is_removable (list))
                gtd_manager_remove_task_list (priv->manager, list);
            }
        }

      g_list_free (children);
    }

  gtk_widget_destroy (dialog);

  /* After removing the lists, exit SELECTION mode */
  gtd_window_set_mode (window, GTD_WINDOW_MODE_NORMAL);
}

static void
gtd_window__rename_button_clicked (GtdWindow *window)
{
  GtdWindowPrivate *priv;
  GtdTaskListItem *item;

  priv = window->priv;
  item = get_selected_list (window);

  if (item)
    {
      GtdTaskList *list;

      list = gtd_task_list_item_get_list (item);

      gtk_popover_set_relative_to (GTK_POPOVER (priv->rename_popover), GTK_WIDGET (item));
      gtk_entry_set_text (GTK_ENTRY (priv->rename_entry), gtd_task_list_get_name (list));
      gtk_widget_show (priv->rename_popover);

      gtk_widget_grab_focus (priv->rename_entry);
    }
}

static void
gtd_window__rename_entry_text_changed (GObject *object,
                                       GParamSpec *pspec,
                                       GtdWindow *window)
{
  gtk_widget_set_sensitive (window->priv->save_rename_button,
                            gtk_entry_get_text_length (GTK_ENTRY (object)) > 0);
}

static void
gtd_window__rename_task_list (GtdWindow *window)
{
  GtdWindowPrivate *priv;
  GtdTaskListItem *item;

  priv = window->priv;

  /*
   * If the save_rename_button is insensitive, the list name is
   * empty and cannot be saved.
   */
  if (!gtk_widget_get_sensitive (priv->save_rename_button))
    return;

  item = get_selected_list (window);

  if (item)
    {
      GtdTaskList *list;

      list = gtd_task_list_item_get_list (item);

      gtd_task_list_set_name (list, gtk_entry_get_text (GTK_ENTRY (priv->rename_entry)));
      gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (priv->lists_flowbox));
      gtd_window_set_mode (window, GTD_WINDOW_MODE_NORMAL);

      gtk_widget_hide (priv->rename_popover);
    }
}

static void
gtd_window__stack_visible_child_cb (GtdWindow *window)
{
  GtdWindowPrivate *priv;
  gboolean is_list_view;

  priv = window->priv;
  is_list_view = g_strcmp0 (gtk_stack_get_visible_child_name (priv->main_stack), "overview") == 0 &&
                 g_strcmp0 (gtk_stack_get_visible_child_name (priv->stack), "lists") == 0;

  gtk_widget_set_visible (GTK_WIDGET (priv->search_button), is_list_view);
  gtk_widget_set_visible (GTK_WIDGET (priv->select_button), is_list_view);
  gtk_widget_set_visible (priv->gear_menu_button, !is_list_view);
}

static void
update_action_bar_buttons (GtdWindow *window)
{
  GtdWindowPrivate *priv;
  GList *children;
  GList *l;
  gboolean all_lists_removable;
  gint selected_lists;

  priv = window->priv;
  children = gtk_container_get_children (GTK_CONTAINER (priv->lists_flowbox));
  selected_lists = 0;
  all_lists_removable = TRUE;

  for (l = children; l != NULL; l = l->next)
    {
      GtdTaskList *list;

      list = gtd_task_list_item_get_list (l->data);

      if (gtd_task_list_item_get_selected (l->data))
        {
          selected_lists++;

          if (!gtd_task_list_is_removable (list))
            all_lists_removable = FALSE;
        }
    }

  gtk_widget_set_sensitive (priv->remove_button, selected_lists > 0 && all_lists_removable);
  gtk_widget_set_sensitive (priv->rename_button, selected_lists == 1);
}

static void
gtd_window__select_button_toggled (GtkToggleButton *button,
                                   GtdWindow       *window)
{
  gtd_window_set_mode (window, gtk_toggle_button_get_active (button) ? GTD_WINDOW_MODE_SELECTION : GTD_WINDOW_MODE_NORMAL);
}

static void
gtd_window__cancel_selection_button_clicked (GtkWidget *button,
                                             GtdWindow *window)
{
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (window->priv->select_button), FALSE);
}

static void
gtd_window__create_new_list (GSimpleAction *simple,
                             GVariant      *parameter,
                             gpointer       user_data)
{
  GtdWindowPrivate *priv;

  g_return_if_fail (GTD_IS_WINDOW (user_data));

  priv = GTD_WINDOW (user_data)->priv;

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->new_list_button), TRUE);
}

static void
gtd_window__change_storage_action (GSimpleAction *simple,
                                   GVariant      *parameter,
                                   gpointer       user_data)
{
  GtdWindowPrivate *priv;

  g_return_if_fail (GTD_IS_WINDOW (user_data));

  priv = GTD_WINDOW (user_data)->priv;

  gtk_dialog_run (GTK_DIALOG (priv->provider_dialog));
}

static void
gtd_window__list_color_set (GtkColorChooser *button,
                            gpointer         user_data)
{
  GtdWindowPrivate *priv = GTD_WINDOW (user_data)->priv;
  GtdTaskList *list;
  GdkRGBA new_color;

  g_return_if_fail (GTD_IS_WINDOW (user_data));
  g_return_if_fail (gtd_task_list_view_get_task_list (priv->list_view));

  list = gtd_task_list_view_get_task_list (priv->list_view);

  g_debug ("%s: %s: %s",
           G_STRFUNC,
           _("Setting new color for task list"),
           gtd_task_list_get_name (list));

  gtk_color_chooser_get_rgba (button, &new_color);
  gtd_task_list_set_color (list, &new_color);

  gtd_manager_save_task_list (priv->manager, list);
}

static gboolean
gtd_window__on_key_press_event (GtkWidget    *widget,
                                GdkEvent     *event,
                                GtkSearchBar *bar)
{
  GtdWindowPrivate *priv;

  g_return_val_if_fail (GTD_IS_WINDOW (widget), TRUE);

  priv = GTD_WINDOW (widget)->priv;

  if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (priv->main_stack)), "overview") == 0 &&
      g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (priv->stack)), "lists") == 0)
    {
      return gtk_search_bar_handle_event (bar, event);
    }

  return FALSE;
}

static gint
gtd_window__flowbox_sort_func (GtdTaskListItem *a,
                               GtdTaskListItem *b,
                               gpointer         user_data)
{
  GtdProvider *p1;
  GtdProvider *p2;
  GtdTaskList *l1;
  GtdTaskList *l2;
  gint retval = 0;

  l1 = gtd_task_list_item_get_list (a);
  p1 = gtd_task_list_get_provider (l1);

  l2 = gtd_task_list_item_get_list (b);
  p2 = gtd_task_list_get_provider (l2);

  retval = g_strcmp0 (gtd_provider_get_description (p1), gtd_provider_get_description (p2));

  if (retval != 0)
    return retval;

  return g_strcmp0 (gtd_task_list_get_name (l1), gtd_task_list_get_name (l2));
}

static gboolean
gtd_window__flowbox_filter_func (GtdTaskListItem *item,
                                 GtdWindow       *window)
{
  GtdWindowPrivate *priv;
  GtdTaskList *list;
  gboolean return_value;
  gchar *search_folded;
  gchar *list_name_folded;
  gchar *haystack;

  g_return_val_if_fail (GTD_IS_WINDOW (window), FALSE);

  priv = window->priv;
  list = gtd_task_list_item_get_list (item);
  haystack = NULL;
  search_folded = g_utf8_casefold (gtk_entry_get_text (GTK_ENTRY (priv->search_entry)), -1);
  list_name_folded = g_utf8_casefold (gtd_task_list_get_name (list), -1);

  if (!search_folded || search_folded[0] == '\0')
    {
      return_value = TRUE;
      goto out;
    }

  haystack = g_strstr_len (list_name_folded,
                           -1,
                           search_folded);

  return_value = (haystack != NULL);

out:
  g_free (search_folded);
  g_free (list_name_folded);

  return return_value;
}

static void
gtd_window__manager_ready_changed (GObject    *source,
                                   GParamSpec *spec,
                                   gpointer    user_data)
{
  GtdWindowPrivate *priv = GTD_WINDOW (user_data)->priv;

  g_return_if_fail (GTD_IS_WINDOW (user_data));

  if (gtd_object_get_ready (GTD_OBJECT (source)))
    {
      gtd_notification_widget_cancel (priv->notification_widget, priv->loading_notification);
    }
  else
    {
      gtd_notification_widget_notify (priv->notification_widget, priv->loading_notification);
    }
}

static void
gtd_window__back_button_clicked (GtkButton *button,
                                 gpointer   user_data)
{
  GtdWindowPrivate *priv = GTD_WINDOW (user_data)->priv;

  g_return_if_fail (GTD_IS_WINDOW (user_data));

  gtk_stack_set_visible_child_name (priv->main_stack, "overview");
  gtk_header_bar_set_custom_title (priv->headerbar, GTK_WIDGET (priv->stack_switcher));
  gtk_header_bar_set_title (priv->headerbar, _("To Do"));
  gtk_widget_hide (GTK_WIDGET (priv->back_button));
  gtk_widget_hide (GTK_WIDGET (priv->color_button));
}

static void
gtd_window__list_selected (GtkFlowBox      *flowbox,
                           GtdTaskListItem *item,
                           gpointer         user_data)
{
  GtdWindowPrivate *priv = GTD_WINDOW (user_data)->priv;
  GtdProvider *provider;
  GtdTaskList *list;
  GdkRGBA *list_color;

  g_return_if_fail (GTD_IS_WINDOW (user_data));
  g_return_if_fail (GTD_IS_TASK_LIST_ITEM (item));

  switch (priv->mode)
    {
    case GTD_WINDOW_MODE_SELECTION:
      gtd_task_list_item_set_selected (item, !gtd_task_list_item_get_selected (item));
      update_action_bar_buttons (GTD_WINDOW (user_data));
      break;

    case GTD_WINDOW_MODE_NORMAL:
      list = gtd_task_list_item_get_list (item);
      provider = gtd_task_list_get_provider (list);
      list_color = gtd_task_list_get_color (list);

      g_signal_handlers_block_by_func (priv->color_button,
                                       gtd_window__list_color_set,
                                       user_data);

      gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (priv->color_button), list_color);

      gtk_stack_set_visible_child_name (priv->main_stack, "tasks");
      gtk_header_bar_set_title (priv->headerbar, gtd_task_list_get_name (list));
      gtk_header_bar_set_subtitle (priv->headerbar, gtd_provider_get_description (provider));
      gtk_header_bar_set_custom_title (priv->headerbar, NULL);
      gtk_search_bar_set_search_mode (priv->search_bar, FALSE);
      gtd_task_list_view_set_task_list (priv->list_view, list);
      gtd_task_list_view_set_show_completed (priv->list_view, FALSE);
      gtk_widget_show (GTK_WIDGET (priv->back_button));
      gtk_widget_show (GTK_WIDGET (priv->color_button));

      g_signal_handlers_unblock_by_func (priv->color_button,
                                         gtd_window__list_color_set,
                                         user_data);

      gdk_rgba_free (list_color);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
gtd_window__list_added (GtdManager  *manager,
                        GtdTaskList *list,
                        gpointer     user_data)
{
  GtdWindowPrivate *priv = GTD_WINDOW (user_data)->priv;
  GtkWidget *item;

  item = gtd_task_list_item_new (list);

  g_object_bind_property (user_data,
                          "mode",
                          item,
                          "mode",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  gtk_widget_show (item);

  gtk_flow_box_insert (priv->lists_flowbox,
                       item,
                       -1);
}

static void
gtd_window__list_removed (GtdManager  *manager,
                          GtdTaskList *list,
                          gpointer     user_data)
{
  GtdWindowPrivate *priv = GTD_WINDOW (user_data)->priv;
  GList *children;
  GList *l;

  children = gtk_container_get_children (GTK_CONTAINER (priv->lists_flowbox));

  for (l = children; l != NULL; l = l->next)
    {
      if (gtd_task_list_item_get_list (l->data) == list)
        gtk_widget_destroy (l->data);
    }

  g_list_free (children);
}

static void
gtd_window__show_error_message (GtdManager  *manager,
                                const gchar *primary_text,
                                const gchar *secondary_text,
                                GtdWindow   *window)
{
  GtdNotification *notification;
  ErrorData *error_data;

  error_data = g_new0 (ErrorData, 1);
  notification = gtd_notification_new (primary_text, 7500);

  error_data->window = window;
  error_data->primary_text = g_strdup (primary_text);
  error_data->secondary_text = g_strdup (secondary_text);

  gtd_notification_set_primary_action (notification,
                                       error_message_notification_primary_action,
                                       error_data);
  gtd_notification_set_secondary_action (notification,
                                         _("Details"),
                                         error_message_notification_secondary_action,
                                         error_data);

  gtd_window_notify (window, notification);
}

static gboolean
gtd_window_configure_event (GtkWidget         *widget,
                            GdkEventConfigure *event)
{
  GtdWindowPrivate *priv;
  GtdWindow *window;
  gboolean retval;

  window = GTD_WINDOW (widget);
  priv = window->priv;

  if (priv->save_geometry_timeout_id != 0)
    {
      g_source_remove (priv->save_geometry_timeout_id);
      priv->save_geometry_timeout_id = 0;
    }

  priv->save_geometry_timeout_id = g_timeout_add (SAVE_GEOMETRY_ID_TIMEOUT,
                                                  gtd_window__save_geometry,
                                                  window);

  retval = GTK_WIDGET_CLASS (gtd_window_parent_class)->configure_event (widget, event);

  return retval;
}

static gboolean
gtd_window_state_event (GtkWidget           *widget,
                        GdkEventWindowState *event)
{
  GtdWindowPrivate *priv;
  GtdWindow *window;
  gboolean retval;

  window = GTD_WINDOW (widget);
  priv = window->priv;

  if (priv->save_geometry_timeout_id != 0)
    {
      g_source_remove (priv->save_geometry_timeout_id);
      priv->save_geometry_timeout_id = 0;
    }

  priv->save_geometry_timeout_id = g_timeout_add (SAVE_GEOMETRY_ID_TIMEOUT,
                                                  gtd_window__save_geometry,
                                                  window);

  retval = GTK_WIDGET_CLASS (gtd_window_parent_class)->window_state_event (widget, event);

  return retval;
}

static void
gtd_window_constructed (GObject *object)
{
  GtdWindowPrivate *priv = GTD_WINDOW (object)->priv;
  GtkApplication *app;
  GMenu *menu;

  G_OBJECT_CLASS (gtd_window_parent_class)->constructed (object);

  /* load stored size */
  gtd_window__load_geometry (GTD_WINDOW (object));

  /* gear menu */
  app = GTK_APPLICATION (g_application_get_default ());
  menu = gtk_application_get_menu_by_id (app, "gear-menu");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (priv->gear_menu_button), G_MENU_MODEL (menu));

  gtk_flow_box_set_sort_func (priv->lists_flowbox,
                              (GtkFlowBoxSortFunc) gtd_window__flowbox_sort_func,
                              NULL,
                              NULL);

  gtk_flow_box_set_filter_func (priv->lists_flowbox,
                                (GtkFlowBoxFilterFunc) gtd_window__flowbox_filter_func,
                                object,
                                NULL);

  g_object_bind_property (object,
                          "manager",
                          priv->new_list_popover,
                          "manager",
                          G_BINDING_DEFAULT);
  g_object_bind_property (object,
                          "manager",
                          priv->provider_dialog,
                          "manager",
                          G_BINDING_DEFAULT);
}

GtkWidget*
gtd_window_new (GtdApplication *application)
{
  return g_object_new (GTD_TYPE_WINDOW,
                       "application", application,
                       "manager",     gtd_application_get_manager (application),
                       NULL);
}

static void
gtd_window_finalize (GObject *object)
{
  G_OBJECT_CLASS (gtd_window_parent_class)->finalize (object);
}

static void
gtd_window_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  GtdWindow *self = GTD_WINDOW (object);

  switch (prop_id)
    {
    case PROP_MANAGER:
      g_value_set_object (value, self->priv->manager);
      break;

    case PROP_MODE:
      g_value_set_enum (value, self->priv->mode);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtd_window_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  GtdWindow *self = GTD_WINDOW (object);
  GList *lists;
  GList *l;

  switch (prop_id)
    {
    case PROP_MANAGER:
      self->priv->manager = g_value_get_object (value);

      g_signal_connect (self->priv->manager,
                        "notify::ready",
                        G_CALLBACK (gtd_window__manager_ready_changed),
                        self);
      g_signal_connect (self->priv->manager,
                        "list-added",
                        G_CALLBACK (gtd_window__list_added),
                        self);
      g_signal_connect (self->priv->manager,
                        "list-removed",
                        G_CALLBACK (gtd_window__list_removed),
                        self);
      g_signal_connect (self->priv->manager,
                        "panel-added",
                        G_CALLBACK (gtd_window__panel_added),
                        self);
      g_signal_connect (self->priv->manager,
                        "panel-removed",
                        G_CALLBACK (gtd_window__panel_removed),
                        self);
      g_signal_connect (self->priv->manager,
                        "show-error-message",
                        G_CALLBACK (gtd_window__show_error_message),
                        self);

      /* Add already loaded lists */
      lists = gtd_manager_get_task_lists (self->priv->manager);

      for (l = lists; l != NULL; l = l->next)
        {
          gtd_window__list_added (self->priv->manager,
                                  l->data,
                                  object);
        }

      g_list_free (lists);
      g_object_notify (object, "manager");
      break;

    case PROP_MODE:
      gtd_window_set_mode (self, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtd_window_class_init (GtdWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gtd_window_finalize;
  object_class->constructed = gtd_window_constructed;
  object_class->get_property = gtd_window_get_property;
  object_class->set_property = gtd_window_set_property;

  widget_class->configure_event = gtd_window_configure_event;
  widget_class->window_state_event = gtd_window_state_event;

  /**
   * GtdWindow::manager:
   *
   * A weak reference to the application's #GtdManager instance.
   */
  g_object_class_install_property (
        object_class,
        PROP_MANAGER,
        g_param_spec_object ("manager",
                             "Manager of this window's application",
                             "The manager of the window's application",
                             GTD_TYPE_MANAGER,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * GtdWindow::mode:
   *
   * The current interaction mode of the window.
   */
  g_object_class_install_property (
        object_class,
        PROP_MODE,
        g_param_spec_enum ("mode",
                           "Mode of this window",
                           "The interaction mode of the window",
                           GTD_TYPE_WINDOW_MODE,
                           GTD_WINDOW_MODE_NORMAL,
                           G_PARAM_READWRITE));

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/todo/ui/window.ui");

  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, action_bar);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, back_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, cancel_selection_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, color_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, gear_menu_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, headerbar);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, lists_flowbox);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, list_view);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, main_stack);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, new_list_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, new_list_popover);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, notification_widget);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, provider_dialog);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, remove_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, rename_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, rename_entry);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, rename_popover);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, save_rename_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, stack);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, search_bar);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, search_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, search_entry);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, select_button);
  gtk_widget_class_bind_template_child_private (widget_class, GtdWindow, stack_switcher);

  gtk_widget_class_bind_template_callback (widget_class, gtd_window__back_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__cancel_selection_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__list_color_set);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__list_selected);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__on_key_press_event);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__remove_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__rename_button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__rename_entry_text_changed);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__rename_task_list);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__select_button_toggled);
  gtk_widget_class_bind_template_callback (widget_class, gtd_window__stack_visible_child_cb);
}

static void
gtd_window_init (GtdWindow *self)
{
  self->priv = gtd_window_get_instance_private (self);

  self->priv->loading_notification = gtd_notification_new (_("Loading your task lists…"), 0);
  gtd_object_set_ready (GTD_OBJECT (self->priv->loading_notification), FALSE);

  /* add actions */
  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   gtd_window_entries,
                                   G_N_ELEMENTS (gtd_window_entries),
                                   self);

  gtk_widget_init_template (GTK_WIDGET (self));
}

/**
 * gtd_window_get_manager:
 * @window: a #GtdWindow
 *
 * Retrieves a weak reference for the @window's #GtdManager.
 *
 * Returns: (transfer none): the #GtdManager of @window.
 */
GtdManager*
gtd_window_get_manager (GtdWindow *window)
{
  g_return_val_if_fail (GTD_IS_WINDOW (window), NULL);

  return window->priv->manager;
}

/**
 * gtd_window_notify:
 * @window: a #GtdWindow
 * @notification: a #GtdNotification
 *
 * Shows a notification on the top of the main window.
 *
 * Returns:
 */
void
gtd_window_notify (GtdWindow       *window,
                   GtdNotification *notification)
{
  GtdWindowPrivate *priv;

  g_return_if_fail (GTD_IS_WINDOW (window));

  priv = window->priv;

  gtd_notification_widget_notify (priv->notification_widget, notification);
}

/**
 * gtd_window_cancel_notification:
 * @window: a #GtdManager
 * @notification: a #GtdNotification
 *
 * Cancels @notification.
 *
 * Returns:
 */
void
gtd_window_cancel_notification (GtdWindow       *window,
                                GtdNotification *notification)
{
  GtdWindowPrivate *priv;

  g_return_if_fail (GTD_IS_WINDOW (window));

  priv = window->priv;

  gtd_notification_widget_cancel (priv->notification_widget, notification);
}

/**
 * gtd_window_get_mode:
 * @window: a #GtdWindow
 *
 * Retrieves the current mode of @window.
 *
 * Returns: the #GtdWindow::mode property value
 */
GtdWindowMode
gtd_window_get_mode (GtdWindow *window)
{
  g_return_val_if_fail (GTD_IS_WINDOW (window), GTD_WINDOW_MODE_NORMAL);

  return window->priv->mode;
}

/**
 * gtd_window_set_mode:
 * @window: a #GtdWindow
 * @mode: a #GtdWindowMode
 *
 * Sets the current window mode to @mode.
 *
 * Returns:
 */
void
gtd_window_set_mode (GtdWindow     *window,
                     GtdWindowMode  mode)
{
  GtdWindowPrivate *priv;

  g_return_if_fail (GTD_IS_WINDOW (window));

  priv = window->priv;

  if (priv->mode != mode)
    {
      GtkStyleContext *context;
      gboolean is_selection_mode;

      priv->mode = mode;
      context = gtk_widget_get_style_context (GTK_WIDGET (priv->headerbar));
      is_selection_mode = (mode == GTD_WINDOW_MODE_SELECTION);

      gtk_widget_set_visible (priv->select_button, !is_selection_mode);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->select_button), is_selection_mode);
      gtk_widget_set_visible (priv->cancel_selection_button, is_selection_mode);
      gtk_widget_set_visible (GTK_WIDGET (priv->new_list_button), !is_selection_mode);
      gtk_widget_set_visible (GTK_WIDGET (priv->action_bar), is_selection_mode);
      gtk_header_bar_set_show_close_button (priv->headerbar, !is_selection_mode);
      gtk_header_bar_set_subtitle (priv->headerbar, NULL);

      if (is_selection_mode)
        {
          gtk_style_context_add_class (context, "selection-mode");
          gtk_header_bar_set_custom_title (priv->headerbar, NULL);
          gtk_header_bar_set_title (priv->headerbar, _("Click a task list to select"));

          update_action_bar_buttons (window);
        }
      else
        {
          /* Unselect all items when leaving selection mode */
          gtk_container_foreach (GTK_CONTAINER (priv->lists_flowbox),
                                 (GtkCallback) gtd_task_list_item_set_selected,
                                 FALSE);

          gtk_style_context_remove_class (context, "selection-mode");
          gtk_header_bar_set_custom_title (priv->headerbar, GTK_WIDGET (priv->stack_switcher));
          gtk_header_bar_set_title (priv->headerbar, _("To Do"));
        }

      g_object_notify (G_OBJECT (window), "mode");
    }
}
