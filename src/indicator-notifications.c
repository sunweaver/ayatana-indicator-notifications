/*
An indicator to display recent notifications.

Adapted from: indicator-datetime/src/indicator-datetime.c by
    Ted Gould <ted@canonical.com>

This program is free software: you can redistribute it and/or modify it 
under the terms of the GNU General Public License version 3, as published 
by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but 
WITHOUT ANY WARRANTY; without even the implied warranties of 
MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR 
PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along 
with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <locale.h>
#include <langinfo.h>
#include <string.h>
#include <time.h>

/* GStuff */
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Indicator Stuff */
#include <libindicator/indicator.h>
#include <libindicator/indicator-object.h>
#include <libindicator/indicator-service-manager.h>

#include "dbus-spy.h"

#define INDICATOR_NOTIFICATIONS_TYPE            (indicator_notifications_get_type ())
#define INDICATOR_NOTIFICATIONS(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), INDICATOR_NOTIFICATIONS_TYPE, IndicatorNotifications))
#define INDICATOR_NOTIFICATIONS_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), INDICATOR_NOTIFICATIONS_TYPE, IndicatorNotificationsClass))
#define IS_INDICATOR_NOTIFICATIONS(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), INDICATOR_NOTIFICATIONS_TYPE))
#define IS_INDICATOR_NOTIFICATIONS_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), INDICATOR_NOTIFICATIONS_TYPE))
#define INDICATOR_NOTIFICATIONS_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), INDICATOR_NOTIFICATIONS_TYPE, IndicatorNotificationsClass))

typedef struct _IndicatorNotifications         IndicatorNotifications;
typedef struct _IndicatorNotificationsClass    IndicatorNotificationsClass;
typedef struct _IndicatorNotificationsPrivate  IndicatorNotificationsPrivate;

struct _IndicatorNotificationsClass {
  IndicatorObjectClass parent_class;
};

struct _IndicatorNotifications {
  IndicatorObject parent;
  IndicatorNotificationsPrivate *priv;
};

struct _IndicatorNotificationsPrivate {
  GtkImage    *image;

  GdkPixbuf   *pixbuf_read;
  GdkPixbuf   *pixbuf_unread;

  GList       *visible_items;
  GList       *hidden_items;

  gboolean     have_unread;

  GtkMenu     *menu;
  GtkWidget   *clear_item;
  GtkWidget   *clear_item_label;

  gchar       *accessible_desc;

  DBusSpy     *spy;
};

#define INDICATOR_NOTIFICATIONS_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE ((o), INDICATOR_NOTIFICATIONS_TYPE, IndicatorNotificationsPrivate))

/**
 * The maximum number of items to show in the indicator at once.
 * FIXME: Store this value in gsettings
 */
#define INDICATOR_MAX_ITEMS 5

#define INDICATOR_ICON_SIZE 22
#define INDICATOR_ICON_READ   "indicator-notification-read"
#define INDICATOR_ICON_UNREAD "indicator-notification-unread"

GType indicator_notifications_get_type(void);

/* Indicator Class Functions */
static void indicator_notifications_class_init(IndicatorNotificationsClass *klass);
static void indicator_notifications_init(IndicatorNotifications *self);
static void indicator_notifications_dispose(GObject *object);
static void indicator_notifications_finalize(GObject *object);

/* Indicator Standard Methods */
static GtkImage    *get_image(IndicatorObject *io);
static GtkMenu     *get_menu(IndicatorObject *io);
static const gchar *get_accessible_desc(IndicatorObject *io);

/* Utility Functions */
static void       clear_menuitems(IndicatorNotifications *self);
static void       insert_menuitem(IndicatorNotifications *self, GtkWidget *item);
static void       remove_menuitem(IndicatorNotifications *self, GtkWidget *item);
static GdkPixbuf *load_icon(const gchar *name, gint size);
static GtkWidget *new_notification_menuitem(Notification *note);
static void       update_clear_item_markup(IndicatorNotifications *self);

/* Callbacks */
static void clear_item_activated_cb(GtkMenuItem *menuitem, gpointer user_data);
static void menu_visible_notify_cb(GtkWidget *menu, GParamSpec *pspec, gpointer user_data);
static void message_received_cb(DBusSpy *spy, Notification *note, gpointer user_data);
static void notification_item_activated_cb(GtkMenuItem *menuitem, gpointer user_data);
static void style_changed_cb(GtkWidget *widget, GtkStyle *oldstyle, gpointer user_data);

/* Indicator Module Config */
INDICATOR_SET_VERSION
INDICATOR_SET_TYPE(INDICATOR_NOTIFICATIONS_TYPE)

G_DEFINE_TYPE (IndicatorNotifications, indicator_notifications, INDICATOR_OBJECT_TYPE);

static void
indicator_notifications_class_init(IndicatorNotificationsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private(klass, sizeof(IndicatorNotificationsPrivate));

  object_class->dispose = indicator_notifications_dispose;
  object_class->finalize = indicator_notifications_finalize;

  IndicatorObjectClass *io_class = INDICATOR_OBJECT_CLASS(klass);

  io_class->get_image = get_image;
  io_class->get_menu = get_menu;
  io_class->get_accessible_desc = get_accessible_desc;

  return;
}

static void
indicator_notifications_init(IndicatorNotifications *self)
{
  self->priv = INDICATOR_NOTIFICATIONS_GET_PRIVATE(self);

  self->priv->menu = NULL;

  self->priv->image = NULL;
  self->priv->pixbuf_read = NULL;
  self->priv->pixbuf_unread = NULL;

  self->priv->have_unread = FALSE;

  self->priv->accessible_desc = _("Notifications");

  self->priv->visible_items = NULL;
  self->priv->hidden_items = NULL;

  self->priv->menu = GTK_MENU(gtk_menu_new());
  g_signal_connect(self->priv->menu, "notify::visible", G_CALLBACK(menu_visible_notify_cb), self);

  /* Create the clear menuitem */
  self->priv->clear_item_label = gtk_label_new(NULL);
  gtk_misc_set_alignment(GTK_MISC(self->priv->clear_item_label), 0, 0);
  gtk_label_set_use_markup(GTK_LABEL(self->priv->clear_item_label), TRUE);
  update_clear_item_markup(self);
  gtk_widget_show(self->priv->clear_item_label);

  self->priv->clear_item = gtk_menu_item_new();
  g_signal_connect(self->priv->clear_item, "activate", G_CALLBACK(clear_item_activated_cb), self);
  gtk_container_add(GTK_CONTAINER(self->priv->clear_item), self->priv->clear_item_label);
  gtk_widget_show(self->priv->clear_item);

  gtk_menu_shell_prepend(GTK_MENU_SHELL(self->priv->menu), self->priv->clear_item);

  /* Watch for notifications from dbus */
  self->priv->spy = dbus_spy_new();
  g_signal_connect(self->priv->spy, DBUS_SPY_SIGNAL_MESSAGE_RECEIVED, G_CALLBACK(message_received_cb), self);
}

static void
indicator_notifications_dispose(GObject *object)
{
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(object);

  if(self->priv->image != NULL) {
    g_object_unref(G_OBJECT(self->priv->image));
    self->priv->image = NULL;
  }

  if(self->priv->pixbuf_read != NULL) {
    g_object_unref(G_OBJECT(self->priv->pixbuf_read));
    self->priv->pixbuf_read = NULL;
  }

  if(self->priv->pixbuf_unread != NULL) {
    g_object_unref(G_OBJECT(self->priv->pixbuf_unread));
    self->priv->pixbuf_unread = NULL;
  }

  if(self->priv->visible_items != NULL) {
    g_list_free_full(self->priv->visible_items, g_object_unref);
    self->priv->visible_items = NULL;
  }

  if(self->priv->hidden_items != NULL) {
    g_list_free_full(self->priv->hidden_items, g_object_unref);
    self->priv->hidden_items = NULL;
  }

  if(self->priv->menu != NULL) {
    g_object_unref(G_OBJECT(self->priv->menu));
    self->priv->menu = NULL;
  }

  if(self->priv->spy != NULL) {
    g_object_unref(G_OBJECT(self->priv->spy));
    self->priv->spy = NULL;
  }

  G_OBJECT_CLASS (indicator_notifications_parent_class)->dispose (object);
  return;
}

static void
indicator_notifications_finalize(GObject *object)
{
  /*IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(object);*/

  G_OBJECT_CLASS (indicator_notifications_parent_class)->finalize (object);
  return;
}

static GtkImage *
get_image(IndicatorObject *io)
{
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(io);

  if(self->priv->image == NULL) {
    self->priv->image = GTK_IMAGE(gtk_image_new());

    self->priv->pixbuf_read = load_icon(INDICATOR_ICON_READ, INDICATOR_ICON_SIZE);

    if(self->priv->pixbuf_read == NULL) {
      g_error("Failed to load read icon");
      return NULL;
    }

    self->priv->pixbuf_unread = load_icon(INDICATOR_ICON_UNREAD, INDICATOR_ICON_SIZE);

    if(self->priv->pixbuf_unread == NULL) {
      g_error("Failed to load unread icon");
      return NULL;
    }

    gtk_image_set_from_pixbuf(self->priv->image, self->priv->pixbuf_read);

    g_signal_connect(G_OBJECT(self->priv->image), "style-set", G_CALLBACK(style_changed_cb), self);

    gtk_widget_show(GTK_WIDGET(self->priv->image));
  }

  return self->priv->image;
}

static GtkMenu *
get_menu(IndicatorObject *io)
{
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(io);

  return GTK_MENU(self->priv->menu);
}

static const gchar *
get_accessible_desc(IndicatorObject *io)
{
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(io);

  return self->priv->accessible_desc;
}

/**
 * clear_menuitems:
 * @self: the indicator
 *
 * Clear all notification menuitems from the menu and the visible/hidden lists.
 **/
static void
clear_menuitems(IndicatorNotifications *self)
{
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(self));
  GList *item;

  /* Remove each visible item from the menu */
  for(item = self->priv->visible_items; item; item = item->next) {
    gtk_container_remove(GTK_CONTAINER(self->priv->menu), GTK_WIDGET(item->data));
  }

  /* Clear the lists */
  g_list_free_full(self->priv->visible_items, g_object_unref);
  self->priv->visible_items = NULL;

  g_list_free_full(self->priv->hidden_items, g_object_unref);
  self->priv->hidden_items = NULL;

  update_clear_item_markup(self);
}

/**
 * insert_menuitem:
 * @self: the indicator
 * @item: the menuitem to insert
 *
 * Inserts a menuitem into the indicator's menu and updates the visible and
 * hidden lists.
 **/
static void
insert_menuitem(IndicatorNotifications *self, GtkWidget *item)
{
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(self));
  g_return_if_fail(GTK_IS_MENU_ITEM(item));
  GList     *last_item;
  GtkWidget *last_widget;

  /* List holds a ref to the menuitem */
  self->priv->visible_items = g_list_prepend(self->priv->visible_items, g_object_ref(item));
  gtk_menu_shell_prepend(GTK_MENU_SHELL(self->priv->menu), item);

  /* Move items that overflow to the hidden list */
  while(g_list_length(self->priv->visible_items) > INDICATOR_MAX_ITEMS) {
    last_item = g_list_last(self->priv->visible_items);  
    last_widget = GTK_WIDGET(last_item->data);
    /* Steal the ref from the visible list */
    self->priv->visible_items = g_list_delete_link(self->priv->visible_items, last_item);
    self->priv->hidden_items = g_list_prepend(self->priv->hidden_items, last_widget);
    gtk_container_remove(GTK_CONTAINER(self->priv->menu), last_widget);
    last_item = NULL;
    last_widget = NULL;
  }

  update_clear_item_markup(self);
}

/**
 * remove_menuitem:
 * @self: the indicator object
 * @item: the menuitem
 *
 * Removes a menuitem from the indicator menu and the visible list.
 **/
static void
remove_menuitem(IndicatorNotifications *self, GtkWidget *item)
{
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(self));
  g_return_if_fail(GTK_IS_MENU_ITEM(item));

  GList *list_item = g_list_find(self->priv->visible_items, item);

  if(list_item == NULL) {
    g_warning("Attempt to remove menuitem not in visible list");
    return;
  }

  /* Remove the item */
  gtk_container_remove(GTK_CONTAINER(self->priv->menu), item);
  self->priv->visible_items = g_list_delete_link(self->priv->visible_items, list_item);
  g_object_unref(item);

  /* Add an item from the hidden list, if available */
  if(g_list_length(self->priv->hidden_items) > 0) {
    list_item = g_list_first(self->priv->hidden_items);
    GtkWidget *list_widget = GTK_WIDGET(list_item->data);
    self->priv->hidden_items = g_list_delete_link(self->priv->hidden_items, list_item);
    gtk_menu_shell_insert(GTK_MENU_SHELL(self->priv->menu), list_widget,
        g_list_length(self->priv->visible_items));
    /* Steal the ref back from the hidden list */
    self->priv->visible_items = g_list_append(self->priv->visible_items, list_widget);
  }

  update_clear_item_markup(self);
}

/**
 * load_icon:
 * @name: the icon name
 * @size: the icon size
 *
 * Tries to load the icon first from the default icon theme, then from an
 * absolute path.
 *
 * Returns: a new gdk pixbuf
 **/
static GdkPixbuf *
load_icon(const gchar *name, gint size)
{
  g_return_val_if_fail(name != NULL, NULL);
  g_return_val_if_fail(size > 0, NULL);
  GError *error = NULL;
  GdkPixbuf *pixbuf = NULL;

  /* First try to load the icon from the icon theme */
  GtkIconTheme *theme = gtk_icon_theme_get_default();

  if(gtk_icon_theme_has_icon(theme, name)) {
    pixbuf = gtk_icon_theme_load_icon(theme, name, size, GTK_ICON_LOOKUP_FORCE_SVG, &error);

    if(error != NULL) {
      g_warning("Failed to load icon '%s' from icon theme: %s", name, error->message);
    }
    else {
      return pixbuf;
    }
  }

  /* Otherwise load from the icon installation path */
  gchar *path = g_strdup_printf(ICONS_DIR "/hicolor/scalable/status/%s.svg", name);
  pixbuf = gdk_pixbuf_new_from_file_at_scale(path, size, size, FALSE, &error);

  if(error != NULL) {
    g_warning("Failed to load icon at '%s': %s", path, error->message);
    pixbuf = NULL;
  }

  g_free(path);

  return pixbuf;
}

/**
 * new_notification_menuitem:
 * @note: the notification object
 *
 * Constructs a new notification menuitem from the given notification object.
 *
 * Returns: a new notification menuitem
 **/
static GtkWidget *
new_notification_menuitem(Notification *note)
{
  g_return_val_if_fail(IS_NOTIFICATION(note), NULL);
  gchar *unescaped_timestamp_string = notification_timestamp_for_locale(note);

  gchar *app_name = g_markup_escape_text(notification_get_app_name(note), -1);
  gchar *summary = g_markup_escape_text(notification_get_summary(note), -1);
  gchar *body = g_markup_escape_text(notification_get_body(note), -1);
  gchar *timestamp_string = g_markup_escape_text(unescaped_timestamp_string, -1);

  gchar *markup = g_strdup_printf("<b>%s</b>\n%s\n<small><i>%s %s <b>%s</b></i></small>",
      summary, body, timestamp_string, _("from"), app_name);

  g_free(app_name);
  g_free(summary);
  g_free(body);
  g_free(unescaped_timestamp_string);
  g_free(timestamp_string);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *label = gtk_label_new(NULL);
  gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
  gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);

  gtk_label_set_max_width_chars(GTK_LABEL(label), 42);

  gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
  gtk_widget_show(label);

  g_free(markup);

  GtkWidget *item = gtk_check_menu_item_new();
  gtk_container_add(GTK_CONTAINER(item), hbox);
  gtk_widget_show(hbox);
  gtk_widget_show(item);

  return item;
}

/**
 * update_clear_item_markup:
 * @self: the indicator object
 *
 * Updates the clear menuitem's label markup based on the number of
 * notifications available.
 **/
static void
update_clear_item_markup(IndicatorNotifications *self)
{
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(self));
  guint visible_length = g_list_length(self->priv->visible_items);
  guint hidden_length = g_list_length(self->priv->hidden_items);
  guint total_length = visible_length + hidden_length;

  gchar *markup = g_strdup_printf(ngettext(
        "Clear <small>(%d Notification)</small>",
        "Clear <small>(%d Notifications)</small>",
        total_length),
      total_length);

  gtk_label_set_markup(GTK_LABEL(self->priv->clear_item_label), markup);
  g_free(markup);
}

/**
 * clear_item_activated_cb:
 * @menuitem: the clear menuitem
 * @user_data: the indicator object
 *
 * Called when the clear menuitem is activated.
 **/
static void
clear_item_activated_cb(GtkMenuItem *menuitem, gpointer user_data)
{
  g_return_if_fail(GTK_IS_MENU_ITEM(menuitem));
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(user_data));
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(user_data);

  clear_menuitems(self);
}

/**
 * menu_visible_notify_cb:
 * @menu: the menu
 * @pspec: unused
 * @user_data: the indicator object
 *
 * Called when the indicator's menu is shown or hidden.
 **/
static void
menu_visible_notify_cb(GtkWidget *menu, G_GNUC_UNUSED GParamSpec *pspec, gpointer user_data)
{
  g_return_if_fail(GTK_IS_MENU(menu));
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(user_data));
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(user_data);

  gboolean visible;
  g_object_get(G_OBJECT(menu), "visible", &visible, NULL);
  if(!visible) {
    if(self->priv->pixbuf_read != NULL) {
      self->priv->have_unread = FALSE;
      gtk_image_set_from_pixbuf(self->priv->image, self->priv->pixbuf_read);
    }
  }
}

/**
 * message_received_cb:
 * @spy: the dbus notification monitor
 * @note: the notification received
 * @user_data: the indicator object
 *
 * Called when a notification arrives on dbus.
 **/
static void
message_received_cb(DBusSpy *spy, Notification *note, gpointer user_data)
{
  g_return_if_fail(IS_DBUS_SPY(spy));
  g_return_if_fail(IS_NOTIFICATION(note));
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(user_data));
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(user_data);

  /* Discard volume notifications */
  if(notification_is_volume(note) || notification_is_empty(note))
    return;

  GtkWidget *item = new_notification_menuitem(note);
  g_signal_connect(item, "activate", G_CALLBACK(notification_item_activated_cb), self);
  g_object_unref(note);

  insert_menuitem(self, item);

  if(self->priv->pixbuf_unread != NULL) {
    self->priv->have_unread = TRUE;
    gtk_image_set_from_pixbuf(self->priv->image, self->priv->pixbuf_unread);
  }
}

/**
 * notification_item_activated_cb:
 * @menuitem: the menuitem
 * @user_data: the indicator object
 *
 * Call when a notification item is activated.
 **/
static void
notification_item_activated_cb(GtkMenuItem *menuitem, gpointer user_data)
{
  g_return_if_fail(GTK_IS_MENU_ITEM(menuitem));
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(user_data));
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(user_data);

  remove_menuitem(self, GTK_WIDGET(menuitem));
}

/**
 * style_changed_cb:
 * @widget: the indicator image
 * @oldstyle: unused
 * @user_data: the indicator object
 *
 * Called when the theme changes, and reloads the indicator icons.
 **/
static void 
style_changed_cb(GtkWidget *widget, GtkStyle *oldstyle, gpointer user_data)
{
  g_return_if_fail(GTK_IS_IMAGE(widget));
  g_return_if_fail(IS_INDICATOR_NOTIFICATIONS(user_data));
  IndicatorNotifications *self = INDICATOR_NOTIFICATIONS(user_data);
  GdkPixbuf *pixbuf_read = NULL, *pixbuf_unread = NULL;

  /* Attempt to load the new pixbufs, but keep the ones we have if this fails */
  pixbuf_read = load_icon(INDICATOR_ICON_READ, INDICATOR_ICON_SIZE);
  if(pixbuf_read != NULL) {
    g_object_unref(self->priv->pixbuf_read);
    self->priv->pixbuf_read = pixbuf_read;

    if(!self->priv->have_unread) {
      gtk_image_set_from_pixbuf(self->priv->image, pixbuf_read);
    }
  }
  else {
    g_warning("Failed to update read icon to new theme");
  }

  pixbuf_unread = load_icon(INDICATOR_ICON_UNREAD, INDICATOR_ICON_SIZE);
  if(pixbuf_unread != NULL) {
    g_object_unref(self->priv->pixbuf_unread);
    self->priv->pixbuf_unread = pixbuf_unread;

    if(self->priv->have_unread) {
      gtk_image_set_from_pixbuf(self->priv->image, pixbuf_unread);
    }
  }
  else {
    g_warning("Failed to update unread icon to new theme");
  }
}

