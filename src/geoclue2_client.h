#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <functional>
#include <memory>
#include <string>

#include "geoclue2-client.h"

// Forward declarations
class GeoClue2Manager;
class GeoClue2Location;

/**
 * GeoClue2 Client interface.
 *
 * Represents a single org.freedesktop.GeoClue2.Client object on D-Bus.
 * Tracks per-client state and exposes Start/Stop and properties.
 */

class GeoClue2Client {
  public:
    using ActiveChangedCallback = std::function<void(bool active)>;

    GeoClue2Client(GDBusConnection *connection, const std::string &object_path,
                   GeoClue2Manager *manager);
    ~GeoClue2Client();

    // Non-copyable
    GeoClue2Client(const GeoClue2Client &) = delete;
    GeoClue2Client &operator=(const GeoClue2Client &) = delete;

    // Get the object path
    const std::string &get_path() const { return m_object_path; }

    // Check if client is active (started)
    bool is_active() const { return m_active; }

    // Update location and emit LocationUpdated signal
    void notify_location_update(const std::string &new_location_path);

    // Set callback for when active state changes
    void set_active_changed_callback(ActiveChangedCallback cb) { m_active_changed_callback = cb; }

  private:
    GDBusConnection *m_connection;
    std::string m_object_path;
    GeoClue2Manager *m_manager;
    GClueClientSkeleton *m_skeleton = nullptr;
    guint m_registration_id = 0;

    // Client state
    bool m_active = false;
    std::string m_desktop_id;
    guint m_requested_accuracy_level = 0;
    guint m_distance_threshold = 0;
    guint m_time_threshold = 0;
    std::string m_location_path = "/"; // "/" means no location yet

    // Callback for active state changes
    ActiveChangedCallback m_active_changed_callback;

    // D-Bus method handlers (static callbacks)
    static gboolean on_handle_start(GClueClient *object, GDBusMethodInvocation *invocation,
                                    gpointer user_data);

    static gboolean on_handle_stop(GClueClient *object, GDBusMethodInvocation *invocation,
                                   gpointer user_data);

    // Helper to set active state and notify manager
    void set_active(bool active);
};
