#include "geoclue2_client.h"
#include "geoclue2_manager.h"

/**
 * Implementation of the GeoClue2 Client object.
 *
 * Represents a single GeoClue2 client with Start/Stop methods,
 * properties, and LocationUpdated signal emission.
 */

GeoClue2Client::GeoClue2Client(GDBusConnection *connection, const std::string &object_path,
                               GeoClue2Manager *manager)
    : m_connection(connection), m_object_path(object_path), m_manager(manager) {
    g_return_if_fail(connection != nullptr);
    g_return_if_fail(manager != nullptr);

    // Create skeleton for org.freedesktop.GeoClue2.Client interface
    GClueClient *client_iface = gclue_client_skeleton_new();
    m_skeleton = GCLUE_CLIENT_SKELETON(client_iface);

    // Set initial property values
    gclue_client_set_location(client_iface, m_location_path.c_str());
    gclue_client_set_distance_threshold(client_iface, m_distance_threshold);
    gclue_client_set_time_threshold(client_iface, m_time_threshold);
    gclue_client_set_desktop_id(client_iface, m_desktop_id.c_str());
    gclue_client_set_requested_accuracy_level(client_iface, m_requested_accuracy_level);
    gclue_client_set_active(client_iface, m_active);

    // Connect method handlers
    g_signal_connect(m_skeleton, "handle-start", G_CALLBACK(on_handle_start), this);
    g_signal_connect(m_skeleton, "handle-stop", G_CALLBACK(on_handle_stop), this);

    // Export the skeleton on D-Bus
    GError *error = nullptr;
    m_registration_id = g_dbus_interface_skeleton_export(
        G_DBUS_INTERFACE_SKELETON(m_skeleton), m_connection, m_object_path.c_str(), &error);

    if (error) {
        g_warning("Failed to export Client at %s: %s", m_object_path.c_str(), error->message);
        g_error_free(error);
        g_object_unref(m_skeleton);
        m_skeleton = nullptr;
        m_registration_id = 0;
        return;
    }

    g_message("GeoClue2Client exported at %s", m_object_path.c_str());
}

GeoClue2Client::~GeoClue2Client() {
    // If still active, stop before destroying
    if (m_active) {
        set_active(false);
    }

    if (m_registration_id != 0) {
        g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(m_skeleton));
        m_registration_id = 0;
    }

    if (m_skeleton) {
        g_object_unref(m_skeleton);
        m_skeleton = nullptr;
    }

    g_message("GeoClue2Client destroyed at %s", m_object_path.c_str());
}

void GeoClue2Client::set_active(bool active) {
    if (m_active == active) {
        return;
    }

    m_active = active;

    if (m_skeleton) {
        gclue_client_set_active(GCLUE_CLIENT(m_skeleton), m_active);
    }

    // Notify manager of state change
    if (m_active_changed_callback) {
        m_active_changed_callback(m_active);
    }

    g_message("Client %s is now %s", m_object_path.c_str(), m_active ? "active" : "inactive");
}

void GeoClue2Client::notify_location_update(const std::string &new_location_path) {
    if (!m_active || !m_skeleton) {
        return; // Only send updates to active clients
    }

    std::string old_location = m_location_path;
    m_location_path = new_location_path;

    // Update the Location property
    gclue_client_set_location(GCLUE_CLIENT(m_skeleton), m_location_path.c_str());

    // Emit LocationUpdated signal
    gclue_client_emit_location_updated(GCLUE_CLIENT(m_skeleton), old_location.c_str(),
                                       new_location_path.c_str());

    g_debug("Client %s: LocationUpdated(%s -> %s)", m_object_path.c_str(), old_location.c_str(),
            new_location_path.c_str());
}

/* static */ gboolean GeoClue2Client::on_handle_start(GClueClient *object,
                                                      GDBusMethodInvocation *invocation,
                                                      gpointer user_data) {
    auto *client = static_cast<GeoClue2Client *>(user_data);
    if (!client) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Internal error: client is null");
        return TRUE;
    }

    g_message("Client %s: Start() called", client->m_object_path.c_str());

    if (client->m_active) {
        // Already started, just complete successfully
        gclue_client_complete_start(object, invocation);
        return TRUE;
    }

    // Activate this client
    client->set_active(true);

    // Complete the method call
    gclue_client_complete_start(object, invocation);

    return TRUE;
}

/* static */ gboolean GeoClue2Client::on_handle_stop(GClueClient *object,
                                                     GDBusMethodInvocation *invocation,
                                                     gpointer user_data) {
    auto *client = static_cast<GeoClue2Client *>(user_data);
    if (!client) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Internal error: client is null");
        return TRUE;
    }

    g_message("Client %s: Stop() called", client->m_object_path.c_str());

    if (!client->m_active) {
        // Already stopped, just complete successfully
        gclue_client_complete_stop(object, invocation);
        return TRUE;
    }

    // Deactivate this client
    client->set_active(false);

    // Complete the method call
    gclue_client_complete_stop(object, invocation);

    return TRUE;
}
