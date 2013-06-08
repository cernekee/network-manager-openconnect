/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/***************************************************************************
 * CVSID: $Id$
 *
 * nm-openconnect.c : GNOME UI dialogs for configuring openconnect VPN connections
 *
 * Copyright (C) 2005 David Zeuthen, <davidz@redhat.com>
 * Copyright (C) 2005 - 2008 Dan Williams, <dcbw@redhat.com>
 * Copyright (C) 2005 - 2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <string.h>
#include <gtk/gtk.h>

#include <openconnect.h>

#ifndef OPENCONNECT_CHECK_VER
#define OPENCONNECT_CHECK_VER(x,y) 0
#endif

#if !OPENCONNECT_CHECK_VER(2,1)
#define openconnect_has_stoken_support() 0
#endif
#if !OPENCONNECT_CHECK_VER(2,2)
#define openconnect_has_oath_support() 0
#endif

#define NM_VPN_API_SUBJECT_TO_CHANGE

#include <nm-vpn-plugin-ui-interface.h>
#include <nm-setting-vpn.h>
#include <nm-setting-connection.h>
#include <nm-setting-ip4-config.h>

#include "../src/nm-openconnect-service.h"
#include "nm-openconnect.h"
#include "auth-helpers.h"

#define OPENCONNECT_PLUGIN_NAME    _("Cisco AnyConnect Compatible VPN (openconnect)")
#define OPENCONNECT_PLUGIN_DESC    _("Compatible with Cisco AnyConnect SSL VPN.")
#define OPENCONNECT_PLUGIN_SERVICE NM_DBUS_SERVICE_OPENCONNECT 


/************** plugin class **************/

static void openconnect_plugin_ui_interface_init (NMVpnPluginUiInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (OpenconnectPluginUi, openconnect_plugin_ui, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_PLUGIN_UI_INTERFACE,
											   openconnect_plugin_ui_interface_init))

/************** UI widget class **************/

static void openconnect_plugin_ui_widget_interface_init (NMVpnPluginUiWidgetInterface *iface_class);

G_DEFINE_TYPE_EXTENDED (OpenconnectPluginUiWidget, openconnect_plugin_ui_widget, G_TYPE_OBJECT, 0,
						G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_PLUGIN_UI_WIDGET_INTERFACE,
											   openconnect_plugin_ui_widget_interface_init))

struct token_mode_entry {
	const char *pref_value;
	const char *label;
	gboolean token_secret_editable;
};

#define OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), OPENCONNECT_TYPE_PLUGIN_UI_WIDGET, OpenconnectPluginUiWidgetPrivate))

typedef struct {
	GtkBuilder *builder;
	GtkWidget *widget;
	GtkSizeGroup *group;
	GtkWindowGroup *window_group;
	gboolean window_added;
	GArray *token_mode_list;
} OpenconnectPluginUiWidgetPrivate;

#define COL_AUTH_NAME 0
#define COL_AUTH_PAGE 1
#define COL_AUTH_TYPE 2

static NMConnection *
import (NMVpnPluginUiInterface *iface, const char *path, GError **error)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingVPN *s_vpn;
	NMSettingIP4Config *s_ip4;
	GKeyFile *keyfile;
	GKeyFileFlags flags;
	const char *buf;
	gboolean bval;

	keyfile = g_key_file_new ();
	flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;

	if (!g_key_file_load_from_file (keyfile, path, flags, error)) {
		g_set_error (error, 0, 0, "does not look like a %s VPN connection (parse failed)", OPENCONNECT_PLUGIN_NAME);
		return NULL;
	}

	connection = nm_connection_new ();
	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());
	nm_connection_add_setting (connection, NM_SETTING (s_con));

	s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_OPENCONNECT, NULL);
	nm_connection_add_setting (connection, NM_SETTING (s_vpn));

	s_ip4 = NM_SETTING_IP4_CONFIG (nm_setting_ip4_config_new ());
	nm_connection_add_setting (connection, NM_SETTING (s_ip4));

	/* Host */
	buf = g_key_file_get_string (keyfile, "openconnect", "Host", NULL);
	if (buf) {
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_GATEWAY, buf);
	} else {
		g_set_error (error, 0, 0, "does not look like a %s VPN connection (no Host)", OPENCONNECT_PLUGIN_NAME);
		g_object_unref (connection);
		return NULL;
	}

	/* Optional Settings */

	/* Description */
	buf = g_key_file_get_string (keyfile, "openconnect", "Description", NULL);
	if (buf)
		g_object_set (s_con, NM_SETTING_CONNECTION_ID, buf, NULL);

	/* CA Certificate */
	buf = g_key_file_get_string (keyfile, "openconnect", "CACert", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_CACERT, buf);

	/* Proxy */
	buf = g_key_file_get_string (keyfile, "openconnect", "Proxy", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_PROXY, buf);

	/* Cisco Secure Desktop */
	bval = g_key_file_get_boolean (keyfile, "openconnect", "CSDEnable", NULL);
	if (bval)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_ENABLE, "yes");

	/* Cisco Secure Desktop wrapper */
	buf = g_key_file_get_string (keyfile, "openconnect", "CSDWrapper", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_WRAPPER, buf);

	/* User Certificate */
	buf = g_key_file_get_string (keyfile, "openconnect", "UserCertificate", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_USERCERT, buf);

	/* Private Key */
	buf = g_key_file_get_string (keyfile, "openconnect", "PrivateKey", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_PRIVKEY, buf);

	/* FSID */
	bval = g_key_file_get_boolean (keyfile, "openconnect", "FSID", NULL);
	if (bval)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_PEM_PASSPHRASE_FSID, "yes");

	/* Soft token mode */
	buf = g_key_file_get_string (keyfile, "openconnect", "StokenSource", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_MODE, buf);

	/* Soft token secret */
	buf = g_key_file_get_string (keyfile, "openconnect", "StokenString", NULL);
	if (buf)
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_SECRET, buf);

	return connection;
}

static gboolean
export (NMVpnPluginUiInterface *iface,
        const char *path,
        NMConnection *connection,
        GError **error)
{
	NMSettingConnection *s_con;
	NMSettingVPN *s_vpn;
	const char *value;
	const char *gateway = NULL;
	const char *cacert = NULL;
	const char *proxy = NULL;
	gboolean csd_enable = FALSE;
	const char *csd_wrapper = NULL;
	const char *usercert = NULL;
	const char *privkey = NULL;
	gboolean pem_passphrase_fsid = FALSE;
	const char *token_mode = NULL;
	const char *token_secret = NULL;
	gboolean success = FALSE;
	FILE *f;

	f = fopen (path, "w");
	if (!f) {
		g_set_error (error, 0, 0, "could not open file for writing");
		return FALSE;
	}

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));

	s_vpn = (NMSettingVPN *) nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_GATEWAY);
	if (value && strlen (value))
		gateway = value;
	else {
		g_set_error (error, 0, 0, "connection was incomplete (missing gateway)");
		goto done;
	}

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_CACERT);
	if (value && strlen (value))
		cacert = value;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_PROXY);
	if (value && strlen (value))
		proxy = value;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_ENABLE);
	if (value && !strcmp (value, "yes"))
		csd_enable = TRUE;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_WRAPPER);
	if (value && strlen (value))
		csd_wrapper = value;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_USERCERT);
	if (value && strlen (value))
		usercert = value;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_PRIVKEY);
	if (value && strlen (value))
		privkey = value;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_PEM_PASSPHRASE_FSID);
	if (value && !strcmp (value, "yes"))
		pem_passphrase_fsid = TRUE;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_MODE);
	if (value && strlen (value))
		token_mode = value;

	value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_SECRET);
	if (value && strlen (value))
		token_secret = value;

	fprintf (f,
		 "[openconnect]\n"
		 "Description=%s\n"
		 "Host=%s\n"
		 "CACert=%s\n"
		 "Proxy=%s\n"
		 "CSDEnable=%s\n"
		 "CSDWrapper=%s\n"
		 "UserCertificate=%s\n"
		 "PrivateKey=%s\n"
		 "FSID=%s\n"
		 "StokenSource=%s\n"
		 "StokenString=%s\n",
		 /* Description */           nm_setting_connection_get_id (s_con),
		 /* Host */                  gateway,
		 /* CA Certificate */        cacert,
		 /* Proxy */                 proxy ? proxy : "",
		 /* Cisco Secure Desktop */  csd_enable ? "1" : "0",
		 /* CSD Wrapper Script */    csd_wrapper ? csd_wrapper : "",
		 /* User Certificate */      usercert,
		 /* Private Key */           privkey,
		 /* FSID */                  pem_passphrase_fsid ? "1" : "0",
		 /* Soft token mode */       token_mode ? token_mode : "",
		 /* Soft token secret */     token_secret ? token_secret : "");

	success = TRUE;

done:
	fclose (f);
	return success;
}

GQuark
openconnect_plugin_ui_error_quark (void)
{
	static GQuark error_quark = 0;

	if (G_UNLIKELY (error_quark == 0))
		error_quark = g_quark_from_static_string ("openconnect-plugin-ui-error-quark");

	return error_quark;
}

/* This should really be standard. */
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }

GType
openconnect_plugin_ui_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			/* Unknown error. */
			ENUM_ENTRY (OPENCONNECT_PLUGIN_UI_ERROR_UNKNOWN, "UnknownError"),
			/* The specified property was invalid. */
			ENUM_ENTRY (OPENCONNECT_PLUGIN_UI_ERROR_INVALID_PROPERTY, "InvalidProperty"),
			/* The specified property was missing and is required. */
			ENUM_ENTRY (OPENCONNECT_PLUGIN_UI_ERROR_MISSING_PROPERTY, "MissingProperty"),
			{ 0, 0, 0 }
		};
		etype = g_enum_register_static ("OpenconnectPluginUiError", values);
	}
	return etype;
}

static gboolean
check_validity (OpenconnectPluginUiWidget *self, GError **error)
{
	OpenconnectPluginUiWidgetPrivate *priv = OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	GtkWidget *widget;
	const char *str;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (!str || !strlen (str)) {
		g_set_error (error,
		             OPENCONNECT_PLUGIN_UI_ERROR,
		             OPENCONNECT_PLUGIN_UI_ERROR_INVALID_PROPERTY,
		             NM_OPENCONNECT_KEY_GATEWAY);
		return FALSE;
	}


	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "proxy_entry"));
	str = gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && str[0] &&
		strncmp(str, "socks://", 8) && strncmp(str, "http://", 7)) {
		g_set_error (error,
		             OPENCONNECT_PLUGIN_UI_ERROR,
		             OPENCONNECT_PLUGIN_UI_ERROR_INVALID_PROPERTY,
		             NM_OPENCONNECT_KEY_PROXY);
		return FALSE;
	}

	if (!auth_widget_check_validity (priv->builder, error))
		return FALSE;

	return TRUE;
}

static void
stuff_changed_cb (GtkWidget *widget, gpointer user_data)
{
	g_signal_emit_by_name (OPENCONNECT_PLUGIN_UI_WIDGET (user_data), "changed");
}

static void
append_token_list_option (OpenconnectPluginUiWidgetPrivate *priv,
				const char *pref_value,
				const char *label,
				gboolean token_secret_editable)
{
	struct token_mode_entry e = {
		.pref_value		= pref_value,
		.label			= label,
		.token_secret_editable	= token_secret_editable,
	};
	g_array_append_val (priv->token_mode_list, e);
}

static gboolean
init_token_ui (OpenconnectPluginUiWidget *self,
				OpenconnectPluginUiWidgetPrivate *priv,
				NMSettingVPN *s_vpn)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkListStore *token_mode_list_store;
	const char *value;
	int i, active_option;

	priv->token_mode_list = g_array_new (FALSE, FALSE, sizeof (struct token_mode_entry));

	append_token_list_option (priv, "disabled", _("Disabled"), FALSE);
	if (openconnect_has_stoken_support ()) {
		append_token_list_option (priv, "stokenrc", _("RSA SecurID - read from ~/.stokenrc"), FALSE);
		append_token_list_option (priv, "manual", _("RSA SecurID - manually entered"), TRUE);
	}
	if (openconnect_has_oath_support ())
		append_token_list_option (priv, "totp", _("TOTP - manually entered"), TRUE);

	/* if "Disabled" is the only option, don't bother rendering the rest of the dialog */
	if (priv->token_mode_list->len == 1)
		return TRUE;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "token_vbox"));
	if (!widget)
		return FALSE;
	gtk_box_pack_start (GTK_BOX (priv->widget), widget, FALSE, FALSE, 0);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "token_mode"));
	if (!widget)
		return FALSE;

	token_mode_list_store = gtk_list_store_new (1, G_TYPE_STRING);
	value = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_MODE) : NULL;

	for (i = 0, active_option = 0; i < priv->token_mode_list->len; i++) {
		struct token_mode_entry *e =
			&g_array_index (priv->token_mode_list, struct token_mode_entry, i);
		GtkTreeIter iter;

		gtk_list_store_append (token_mode_list_store, &iter);
		gtk_list_store_set (token_mode_list_store, &iter, 0, e->label, -1);

		if (value && !strcmp (value, e->pref_value))
			active_option = i;
	}
	gtk_combo_box_set_model (GTK_COMBO_BOX (widget), GTK_TREE_MODEL (token_mode_list_store));
	gtk_combo_box_set_active (GTK_COMBO_BOX (widget), active_option);

	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "token_secret"));
	if (!widget)
		return FALSE;
	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (widget));
	if (!buffer)
		return FALSE;
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_SECRET);
		if (value)
			gtk_text_buffer_set_text (buffer, value, -1);
	}
	g_signal_connect (G_OBJECT (buffer), "changed", G_CALLBACK (stuff_changed_cb), self);

	return TRUE;
}

static gboolean
init_plugin_ui (OpenconnectPluginUiWidget *self, NMConnection *connection, GError **error)
{
	OpenconnectPluginUiWidgetPrivate *priv = OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	NMSettingVPN *s_vpn;
	GtkWidget *widget;
	const char *value;

	s_vpn = (NMSettingVPN *) nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	priv->group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_GATEWAY);
		if (value)
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "proxy_entry"));
	if (!widget)
		return FALSE;
	gtk_size_group_add_widget (priv->group, widget);
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_PROXY);
		if (value)
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "fsid_button"));
	if (!widget)
		return FALSE;
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_PEM_PASSPHRASE_FSID);
		if (value && !strcmp(value, "yes"))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON (widget), TRUE);
	}
	g_signal_connect (G_OBJECT (widget), "toggled", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "csd_button"));
	if (!widget)
		return FALSE;
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_ENABLE);
		if (value && !strcmp(value, "yes"))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON (widget), TRUE);
	}
	g_signal_connect (G_OBJECT (widget), "toggled", G_CALLBACK (stuff_changed_cb), self);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "csd_wrapper_entry"));
	if (!widget)
		return FALSE;
	if (s_vpn) {
		value = nm_setting_vpn_get_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_WRAPPER);
		if (value)
			gtk_entry_set_text (GTK_ENTRY (widget), value);
	}
	g_signal_connect (G_OBJECT (widget), "changed", G_CALLBACK (stuff_changed_cb), self);

	if (init_token_ui (self, priv, s_vpn) == FALSE)
		return FALSE;

	tls_pw_init_auth_widget (priv->builder, priv->group, s_vpn, stuff_changed_cb, self);

	return TRUE;
}

static GObject *
get_widget (NMVpnPluginUiWidgetInterface *iface)
{
	OpenconnectPluginUiWidget *self = OPENCONNECT_PLUGIN_UI_WIDGET (iface);
	OpenconnectPluginUiWidgetPrivate *priv = OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE (self);

	return G_OBJECT (priv->widget);
}

static gboolean
update_connection (NMVpnPluginUiWidgetInterface *iface,
                   NMConnection *connection,
                   GError **error)
{
	OpenconnectPluginUiWidget *self = OPENCONNECT_PLUGIN_UI_WIDGET (iface);
	OpenconnectPluginUiWidgetPrivate *priv = OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE (self);
	NMSettingVPN *s_vpn;
	GtkWidget *widget;
	char *str;
	gint idx;
	gboolean token_secret_editable = FALSE;
	GtkTextIter iter_start, iter_end;
	GtkTextBuffer *buffer;
	const char *auth_type = NULL;

	s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_OPENCONNECT, NULL);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "gateway_entry"));
	str = (char *) gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_GATEWAY, str);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "proxy_entry"));
	str = (char *) gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_PROXY, str);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "fsid_button"));
	str = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (widget))?"yes":"no";
	nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_PEM_PASSPHRASE_FSID, str);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "csd_button"));
	str = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (widget))?"yes":"no";
	nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_ENABLE, str);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "csd_wrapper_entry"));
	str = (char *) gtk_entry_get_text (GTK_ENTRY (widget));
	if (str && strlen (str))
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_CSD_WRAPPER, str);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "token_mode"));
	idx = gtk_combo_box_get_active (GTK_COMBO_BOX (widget));

	if (idx < priv->token_mode_list->len) {
		struct token_mode_entry *e =
			&g_array_index (priv->token_mode_list, struct token_mode_entry, idx);
		nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_MODE, e->pref_value);
		token_secret_editable = e->token_secret_editable;
	}

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "token_secret_label"));
	gtk_widget_set_sensitive (widget, token_secret_editable);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "token_secret"));
	gtk_widget_set_sensitive (widget, token_secret_editable);

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (widget));
	gtk_text_buffer_get_start_iter (buffer, &iter_start);
	gtk_text_buffer_get_end_iter (buffer, &iter_end);
	str = (char *) gtk_text_buffer_get_text (buffer, &iter_start, &iter_end, TRUE);
	if (str) {
		char *src = str, *dst = str;

		/* zap invalid characters */
		for (; *src; src++)
			if (*src >= ' ' && *src <= '~')
				*(dst++) = *src;
		*dst = 0;

		if (strlen (str))
			nm_setting_vpn_add_data_item (s_vpn, NM_OPENCONNECT_KEY_TOKEN_SECRET, str);
	}

	if (!check_validity (self, error))
		return FALSE;

	/* These are different for every login session, and should not be stored */
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "gwcert",
								 NM_SETTING_SECRET_FLAG_NOT_SAVED, NULL);
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "cookie",
								 NM_SETTING_SECRET_FLAG_NOT_SAVED, NULL);
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "gateway",
								 NM_SETTING_SECRET_FLAG_NOT_SAVED, NULL);

	/* These are purely internal data for the auth-dialog, and should be stored */
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "xmlconfig",
								 NM_SETTING_SECRET_FLAG_NONE, NULL);
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "lasthost",
								 NM_SETTING_SECRET_FLAG_NONE, NULL);
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "autoconnect",
								 NM_SETTING_SECRET_FLAG_NONE, NULL);
	nm_setting_set_secret_flags (NM_SETTING (s_vpn), "certsigs",
								 NM_SETTING_SECRET_FLAG_NONE, NULL);
	/* Note that the auth-dialog will also store "extra" secrets for form
	   entries, depending on the arbitrary forms that we're offered by the
	   server during authentication. We can't know about those in advance,
	   but the presence of the above four is sufficient to trigger a write
	   of the new secrets, and the code in the keyfile plugin will treat the
	   absence of a flags configuration for a given secret as equivalent to
	   FLAG_NONE, and thus save our "extra" secrets too. */

	auth_widget_update_connection (priv->builder, auth_type, s_vpn);

	nm_connection_add_setting (connection, NM_SETTING (s_vpn));
	return TRUE;
}

static NMVpnPluginUiWidgetInterface *
nm_vpn_plugin_ui_widget_interface_new (NMConnection *connection, GError **error)
{
	NMVpnPluginUiWidgetInterface *object;
	OpenconnectPluginUiWidgetPrivate *priv;
	char *ui_file;

	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	object = NM_VPN_PLUGIN_UI_WIDGET_INTERFACE (g_object_new (OPENCONNECT_TYPE_PLUGIN_UI_WIDGET, NULL));
	if (!object) {
		g_set_error (error, OPENCONNECT_PLUGIN_UI_ERROR, 0, "could not create openconnect object");
		return NULL;
	}

	priv = OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE (object);

	ui_file = g_strdup_printf ("%s/%s", UIDIR, "nm-openconnect-dialog.ui");
	priv->builder = gtk_builder_new ();

	gtk_builder_set_translation_domain (priv->builder, GETTEXT_PACKAGE);

	if (!gtk_builder_add_from_file (priv->builder, ui_file, error)) {
		g_warning ("Couldn't load builder file: %s",
		           error && *error ? (*error)->message : "(unknown)");
		g_clear_error (error);
		g_set_error (error, OPENCONNECT_PLUGIN_UI_ERROR, 0,
		             "could not load required resources at %s", ui_file);
		g_free (ui_file);
		g_object_unref (object);
		return NULL;
	}
	g_free (ui_file);

	priv->widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "openconnect-vbox"));
	if (!priv->widget) {
		g_set_error (error, OPENCONNECT_PLUGIN_UI_ERROR, 0, "could not load UI widget");
		g_object_unref (object);
		return NULL;
	}
	g_object_ref_sink (priv->widget);

	priv->window_group = gtk_window_group_new ();

	if (!init_plugin_ui (OPENCONNECT_PLUGIN_UI_WIDGET (object), connection, error)) {
		g_object_unref (object);
		return NULL;
	}

	return object;
}

static void
dispose (GObject *object)
{
	OpenconnectPluginUiWidget *plugin = OPENCONNECT_PLUGIN_UI_WIDGET (object);
	OpenconnectPluginUiWidgetPrivate *priv = OPENCONNECT_PLUGIN_UI_WIDGET_GET_PRIVATE (plugin);

	if (priv->group)
		g_object_unref (priv->group);

	if (priv->window_group)
		g_object_unref (priv->window_group);

	if (priv->widget)
		g_object_unref (priv->widget);

	if (priv->builder)
		g_object_unref (priv->builder);

	if (priv->token_mode_list)
		g_array_free (priv->token_mode_list, TRUE);

	G_OBJECT_CLASS (openconnect_plugin_ui_widget_parent_class)->dispose (object);
}

static void
openconnect_plugin_ui_widget_class_init (OpenconnectPluginUiWidgetClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	g_type_class_add_private (req_class, sizeof (OpenconnectPluginUiWidgetPrivate));

	object_class->dispose = dispose;
}

static void
openconnect_plugin_ui_widget_init (OpenconnectPluginUiWidget *plugin)
{
}

static void
openconnect_plugin_ui_widget_interface_init (NMVpnPluginUiWidgetInterface *iface_class)
{
	/* interface implementation */
	iface_class->get_widget = get_widget;
	iface_class->update_connection = update_connection;
}

static guint32
get_capabilities (NMVpnPluginUiInterface *iface)
{
	return (NM_VPN_PLUGIN_UI_CAPABILITY_IMPORT |
	        NM_VPN_PLUGIN_UI_CAPABILITY_EXPORT |
	        NM_VPN_PLUGIN_UI_CAPABILITY_IPV6);
}

static NMVpnPluginUiWidgetInterface *
ui_factory (NMVpnPluginUiInterface *iface, NMConnection *connection, GError **error)
{
	return nm_vpn_plugin_ui_widget_interface_new (connection, error);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case NM_VPN_PLUGIN_UI_INTERFACE_PROP_NAME:
		g_value_set_string (value, OPENCONNECT_PLUGIN_NAME);
		break;
	case NM_VPN_PLUGIN_UI_INTERFACE_PROP_DESC:
		g_value_set_string (value, OPENCONNECT_PLUGIN_DESC);
		break;
	case NM_VPN_PLUGIN_UI_INTERFACE_PROP_SERVICE:
		g_value_set_string (value, OPENCONNECT_PLUGIN_SERVICE);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
openconnect_plugin_ui_class_init (OpenconnectPluginUiClass *req_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (req_class);

	object_class->get_property = get_property;

	g_object_class_override_property (object_class,
									  NM_VPN_PLUGIN_UI_INTERFACE_PROP_NAME,
									  NM_VPN_PLUGIN_UI_INTERFACE_NAME);

	g_object_class_override_property (object_class,
									  NM_VPN_PLUGIN_UI_INTERFACE_PROP_DESC,
									  NM_VPN_PLUGIN_UI_INTERFACE_DESC);

	g_object_class_override_property (object_class,
									  NM_VPN_PLUGIN_UI_INTERFACE_PROP_SERVICE,
									  NM_VPN_PLUGIN_UI_INTERFACE_SERVICE);
}

static void
openconnect_plugin_ui_init (OpenconnectPluginUi *plugin)
{
}

static void
openconnect_plugin_ui_interface_init (NMVpnPluginUiInterface *iface_class)
{
	/* interface implementation */
	iface_class->ui_factory = ui_factory;
	iface_class->get_capabilities = get_capabilities;
	iface_class->import_from_file = import;
	iface_class->export_to_file = export;
}

G_MODULE_EXPORT NMVpnPluginUiInterface *
nm_vpn_plugin_ui_factory (GError **error)
{
	if (error)
		g_return_val_if_fail (*error == NULL, NULL);

	return NM_VPN_PLUGIN_UI_INTERFACE (g_object_new (OPENCONNECT_TYPE_PLUGIN_UI, NULL));
}

