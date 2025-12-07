#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "geoclue2-manager.h"

// Forward declarations
class GeoClue2Client;
class GeoClue2Location;
class Geoclue1Backend;
struct GeoClue1Position;

/**
 * GeoClue2 Manager interface.
 *
 * Responsible for exporting org.freedesktop.GeoClue2.Manager on
 * /org/freedesktop/GeoClue2/Manager and managing Client objects.
 */

// Canonical D-Bus identifiers for the GeoClue2 Manager
inline constexpr const char *GEOCLUE2_MANAGER_OBJECT_PATH = "/org/freedesktop/GeoClue2/Manager";
inline constexpr const char *GEOCLUE2_MANAGER_INTERFACE = "org.freedesktop.GeoClue2.Manager";

class GeoClue2Manager {
  public:
    explicit GeoClue2Manager(GDBusConnection *connection);
    ~GeoClue2Manager();

    // Non-copyable
    GeoClue2Manager(const GeoClue2Manager &) = delete;
    GeoClue2Manager &operator=(const GeoClue2Manager &) = delete;

    // Backend wiring - so Manager can control GeoClue1 lifecycle
    void set_backend(const std::shared_ptr<Geoclue1Backend> &backend);

    // Client lifecycle hooks (called from Client::set_active())
    void client_became_active();
    void client_became_inactive();

    // Position update handler (called from backend callback)
    void handle_position_update(const GeoClue1Position &pos);

    // Get the D-Bus connection
    GDBusConnection *get_connection() const { return m_connection; }

  private:
    GDBusConnection *m_connection;
    GClueManagerSkeleton *m_skeleton = nullptr;
    guint m_registration_id = 0;

    // GeoClue1 backend for GPS tracking
    std::shared_ptr<Geoclue1Backend> m_backend;

    // Client registry
    std::unordered_map<std::string, std::shared_ptr<GeoClue2Client>> m_clients_by_peer;
    std::unordered_map<std::string, std::shared_ptr<GeoClue2Client>> m_clients_by_path;
    guint m_next_client_id = 0;

    // Location management
    guint m_next_location_id = 0;
    std::deque<std::shared_ptr<GeoClue2Location>> m_locations;

    // Active client tracking
    guint m_active_clients = 0;
    guint m_grace_timeout_ms = 15000; // 15 seconds
    guint m_grace_timeout_id = 0;

    // D-Bus method handlers
    static gboolean on_handle_get_client(GClueManager *object, GDBusMethodInvocation *invocation,
                                         gpointer user_data);

    static gboolean on_handle_create_client(GClueManager *object, GDBusMethodInvocation *invocation,
                                            gpointer user_data);

    static gboolean on_handle_delete_client(GClueManager *object, GDBusMethodInvocation *invocation,
                                            const gchar *client_path, gpointer user_data);

    static gboolean on_handle_add_agent(GClueManager *object, GDBusMethodInvocation *invocation,
                                        const gchar *agent_id, gpointer user_data);

    // Peer monitoring
    static void on_peer_vanished(GDBusConnection *connection, const gchar *name,
                                 gpointer user_data);

    // Helper methods
    std::shared_ptr<GeoClue2Client> create_client_for_peer(const std::string &peer, bool reuse);
    void remove_client(const std::string &client_path);
    void update_in_use_property();

    // Grace timeout callback
    static gboolean on_grace_timeout(gpointer user_data);
};

/**
 * Helper to create and register Manager instance on D-Bus.
 */
std::shared_ptr<GeoClue2Manager> geoclue2_manager_register(GDBusConnection *connection);
