#include "geoclue2_manager.h"
#include "geoclue1_backend.h"
#include "geoclue2_client.h"
#include "geoclue2_location.h"

#include <memory>

// Maximum number of locations to keep in memory
const size_t MAX_STORED_LOCATIONS = 25;

/**
 * Implementation of the GeoClue2 Manager object.
 *
 * Responsibilities:
 * - Export org.freedesktop.GeoClue2.Manager on D-Bus
 * - Handle GetClient/CreateClient/DeleteClient methods
 * - Manage client registry and lifecycles
 * - Control GeoClue1 backend based on active clients
 * - Broadcast position updates to all active clients
 */

GeoClue2Manager::GeoClue2Manager(GDBusConnection *connection) : m_connection(connection) {
    g_return_if_fail(connection != nullptr);

    // Create skeleton for org.freedesktop.GeoClue2.Manager interface
    GClueManager *manager_iface = gclue_manager_skeleton_new();
    m_skeleton = GCLUE_MANAGER_SKELETON(manager_iface);

    // Set initial property values
    gclue_manager_set_in_use(manager_iface, FALSE);
    gclue_manager_set_available_accuracy_level(manager_iface,
                                               8); // EXACT level

    // Connect method handlers
    g_signal_connect(m_skeleton, "handle-get-client", G_CALLBACK(on_handle_get_client), this);
    g_signal_connect(m_skeleton, "handle-create-client", G_CALLBACK(on_handle_create_client), this);
    g_signal_connect(m_skeleton, "handle-delete-client", G_CALLBACK(on_handle_delete_client), this);
    g_signal_connect(m_skeleton, "handle-add-agent", G_CALLBACK(on_handle_add_agent), this);

    // Export the Manager on D-Bus
    GError *error = nullptr;
    m_registration_id = g_dbus_interface_skeleton_export(
        G_DBUS_INTERFACE_SKELETON(m_skeleton), m_connection, GEOCLUE2_MANAGER_OBJECT_PATH, &error);

    if (error) {
        g_warning("Failed to export Manager at %s: %s", GEOCLUE2_MANAGER_OBJECT_PATH,
                  error->message);
        g_error_free(error);
        g_object_unref(m_skeleton);
        m_skeleton = nullptr;
        m_registration_id = 0;
        return;
    }

    g_message("GeoClue2Manager exported at %s", GEOCLUE2_MANAGER_OBJECT_PATH);
}

GeoClue2Manager::~GeoClue2Manager() {
    if (m_grace_timeout_id != 0) {
        g_source_remove(m_grace_timeout_id);
        m_grace_timeout_id = 0;
    }

    // Clean up all clients
    m_clients_by_peer.clear();
    m_clients_by_path.clear();

    if (m_registration_id != 0) {
        g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(m_skeleton));
        m_registration_id = 0;
    }

    if (m_skeleton) {
        g_object_unref(m_skeleton);
        m_skeleton = nullptr;
    }

    g_message("GeoClue2Manager destroyed");
}

void GeoClue2Manager::set_backend(const std::shared_ptr<Geoclue1Backend> &backend) {
    m_backend = backend;
    if (m_backend) {
        g_message("GeoClue2Manager: backend set");
    } else {
        g_message("GeoClue2Manager: backend cleared");
    }
}

void GeoClue2Manager::client_became_active() {
    // Cancel any pending grace timeout
    if (m_grace_timeout_id != 0) {
        g_source_remove(m_grace_timeout_id);
        m_grace_timeout_id = 0;
    }

    ++m_active_clients;
    g_message("GeoClue2Manager: client became active (count=%u)", m_active_clients);

    // Update InUse property
    update_in_use_property();

    // If this is the first active client, start GeoClue1
    if (m_active_clients == 1 && m_backend) {
        g_message("GeoClue2Manager: starting GeoClue1 backend");
        m_backend->start_tracking();
    }
}

void GeoClue2Manager::client_became_inactive() {
    if (m_active_clients == 0) {
        g_warning("GeoClue2Manager::client_became_inactive called with count=0");
        return;
    }

    --m_active_clients;
    g_message("GeoClue2Manager: client became inactive (count=%u)", m_active_clients);

    // Update InUse property
    update_in_use_property();

    if (m_active_clients == 0) {
        // No more active clients: schedule GPS shutdown with grace timeout
        if (m_grace_timeout_id != 0) {
            g_source_remove(m_grace_timeout_id);
            m_grace_timeout_id = 0;
        }

        m_grace_timeout_id =
            g_timeout_add(m_grace_timeout_ms, &GeoClue2Manager::on_grace_timeout, this);

        g_message("GeoClue2Manager: scheduled GeoClue1 stop in %u ms", m_grace_timeout_ms);
    }
}

void GeoClue2Manager::handle_position_update(const GeoClue1Position &pos) {
    // Create a new Location object for this position
    std::string location_path =
        "/org/freedesktop/GeoClue2/Location/" + std::to_string(++m_next_location_id);

    auto location = std::make_shared<GeoClue2Location>(m_connection, location_path);

    // Set properties BEFORE exporting to D-Bus
    // This ensures clients see valid data when object appears
    location->set_from_geoclue1_position(pos);

    // Store location to keep it alive while clients may reference it
    m_locations.push_back(location);

    // Broadcast to all active clients
    for (auto &pair : m_clients_by_path) {
        auto &client = pair.second;
        if (client && client->is_active()) {
            client->notify_location_update(location_path);
        }
    }

    g_debug("GeoClue2Manager: broadcasted location %s to %u active clients", location_path.c_str(),
            m_active_clients);

    // Clean up old locations to prevent memory growth
    // Following geoclue-2 pattern: keep some locations (clients may be slow)
    // Only clean up locations > MAX_STORED_LOCATIONS updates old (enough buffer for any client)
    while (m_locations.size() > MAX_STORED_LOCATIONS) {
        m_locations.pop_front();
    }
}

std::shared_ptr<GeoClue2Client> GeoClue2Manager::create_client_for_peer(const std::string &peer,
                                                                        bool reuse) {
    // Check if we should reuse existing client
    if (reuse) {
        auto it = m_clients_by_peer.find(peer);
        if (it != m_clients_by_peer.end()) {
            g_message("GeoClue2Manager: reusing existing client for peer %s", peer.c_str());
            return it->second;
        }
    }

    // Create new client
    std::string client_path =
        "/org/freedesktop/GeoClue2/Client/" + std::to_string(++m_next_client_id);

    auto client = std::make_shared<GeoClue2Client>(m_connection, client_path, this);

    // Set up active state callback to track GPS lifecycle
    client->set_active_changed_callback([this](bool active) {
        if (active) {
            this->client_became_active();
        } else {
            this->client_became_inactive();
        }
    });

    // Register client
    m_clients_by_peer[peer] = client;
    m_clients_by_path[client_path] = client;

    // Monitor peer for vanishing (disconnection/crash)
    g_bus_watch_name_on_connection(m_connection, peer.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   nullptr, // name appeared (not needed, already appeared)
                                   on_peer_vanished, this, nullptr);

    g_message("GeoClue2Manager: created client %s for peer %s", client_path.c_str(), peer.c_str());

    return client;
}

void GeoClue2Manager::remove_client(const std::string &client_path) {
    auto it = m_clients_by_path.find(client_path);
    if (it == m_clients_by_path.end()) {
        g_warning("GeoClue2Manager::remove_client: client %s not found", client_path.c_str());
        return;
    }

    auto client = it->second;

    // Remove from both registries
    m_clients_by_path.erase(it);

    // Find and remove from peer registry
    for (auto it2 = m_clients_by_peer.begin(); it2 != m_clients_by_peer.end(); ++it2) {
        if (it2->second == client) {
            m_clients_by_peer.erase(it2);
            break;
        }
    }

    g_message("GeoClue2Manager: removed client %s", client_path.c_str());
    update_in_use_property();
}

void GeoClue2Manager::update_in_use_property() {
    gboolean in_use = (m_active_clients > 0);
    if (m_skeleton) {
        gclue_manager_set_in_use(GCLUE_MANAGER(m_skeleton), in_use);
    }
}

/* static */ gboolean GeoClue2Manager::on_handle_get_client(GClueManager *object,
                                                            GDBusMethodInvocation *invocation,
                                                            gpointer user_data) {
    auto *manager = static_cast<GeoClue2Manager *>(user_data);
    if (!manager) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Internal error: manager is null");
        return TRUE;
    }

    const gchar *peer = g_dbus_method_invocation_get_sender(invocation);
    g_message("GeoClue2Manager: GetClient() called by %s", peer);

    // Create or reuse client for this peer
    auto client = manager->create_client_for_peer(peer, true);
    if (!client) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Failed to create client");
        return TRUE;
    }

    // Complete the method call with client path
    gclue_manager_complete_get_client(object, invocation, client->get_path().c_str());

    return TRUE;
}

/* static */ gboolean GeoClue2Manager::on_handle_create_client(GClueManager *object,
                                                               GDBusMethodInvocation *invocation,
                                                               gpointer user_data) {
    auto *manager = static_cast<GeoClue2Manager *>(user_data);
    if (!manager) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Internal error: manager is null");
        return TRUE;
    }

    const gchar *peer = g_dbus_method_invocation_get_sender(invocation);
    g_message("GeoClue2Manager: CreateClient() called by %s", peer);

    // Always create new client (never reuse)
    auto client = manager->create_client_for_peer(peer, false);
    if (!client) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Failed to create client");
        return TRUE;
    }

    // Complete the method call with client path
    gclue_manager_complete_create_client(object, invocation, client->get_path().c_str());

    return TRUE;
}

/* static */ gboolean GeoClue2Manager::on_handle_delete_client(GClueManager *object,
                                                               GDBusMethodInvocation *invocation,
                                                               const gchar *client_path,
                                                               gpointer user_data) {
    auto *manager = static_cast<GeoClue2Manager *>(user_data);
    if (!manager) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Internal error: manager is null");
        return TRUE;
    }

    g_message("GeoClue2Manager: DeleteClient(%s) called", client_path);

    // Remove the client
    manager->remove_client(client_path);

    // Complete the method call
    gclue_manager_complete_delete_client(object, invocation);

    return TRUE;
}

/* static */ gboolean GeoClue2Manager::on_handle_add_agent(GClueManager *object,
                                                           GDBusMethodInvocation *invocation,
                                                           const gchar *agent_id,
                                                           gpointer user_data) {
    // Agent API not implemented - we skip authorization
    g_message("GeoClue2Manager: AddAgent(%s) called (not implemented)", agent_id);

    // Just complete successfully
    gclue_manager_complete_add_agent(object, invocation);

    return TRUE;
}

/* static */ void GeoClue2Manager::on_peer_vanished(GDBusConnection * /*connection*/,
                                                    const gchar *name, gpointer user_data) {
    auto *manager = static_cast<GeoClue2Manager *>(user_data);
    if (!manager) {
        return;
    }

    g_message("GeoClue2Manager: peer %s vanished", name);

    // Find all clients belonging to this peer and remove them
    std::vector<std::string> paths_to_remove;

    for (auto &pair : manager->m_clients_by_peer) {
        if (pair.first == name) {
            paths_to_remove.push_back(pair.second->get_path());
        }
    }

    for (const auto &path : paths_to_remove) {
        manager->remove_client(path);
    }
}

/* static */ gboolean GeoClue2Manager::on_grace_timeout(gpointer user_data) {
    auto *self = static_cast<GeoClue2Manager *>(user_data);
    if (!self) {
        return G_SOURCE_REMOVE;
    }

    self->m_grace_timeout_id = 0;

    if (self->m_active_clients == 0 && self->m_backend) {
        g_message("GeoClue2Manager: grace timeout expired, stopping GeoClue1 backend");
        self->m_backend->stop_tracking();
    } else {
        g_message("GeoClue2Manager: grace timeout expired, but clients=%u, "
                  "skipping stop",
                  self->m_active_clients);
    }

    return G_SOURCE_REMOVE;
}

std::shared_ptr<GeoClue2Manager> geoclue2_manager_register(GDBusConnection *connection) {
    g_return_val_if_fail(connection != nullptr, nullptr);

    auto manager = std::make_shared<GeoClue2Manager>(connection);

    if (manager && manager->get_connection()) {
        g_message("GeoClue2Manager successfully registered on D-Bus");
        return manager;
    }

    g_warning("GeoClue2Manager failed to register");
    return nullptr;
}
