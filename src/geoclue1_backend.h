#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <functional>
#include <string>

/**
 * GeoClue1 backend interface.
 *
 * Talks to the GeoClue1 D-Bus API (e.g. org.freedesktop.Geoclue.Master),
 * starts/stops tracking, and emits callbacks when new position data arrives.
 */

struct GeoClue1Position {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double accuracy = 0.0;
    double speed = -1.0;   // m/s (-1.0 = unknown, from VelocityChanged)
    double heading = -1.0; // degrees from north (-1.0 = unknown)
    double climb = -1.0;   // m/s vertical speed (-1.0 = unknown)
    std::string timestamp_iso8601;
};

struct GeoClue1Velocity {
    double speed = 0.0;     // meters per second
    double direction = 0.0; // degrees from north
    double climb = 0.0;     // meters per second
    std::string timestamp_iso8601;
};

class Geoclue1Backend {
  public:
    using PositionCallback = std::function<void(const GeoClue1Position &)>;
    using VelocityCallback = std::function<void(const GeoClue1Velocity &)>;

    explicit Geoclue1Backend(GDBusConnection *connection);
    ~Geoclue1Backend();

    // Non-copyable
    Geoclue1Backend(const Geoclue1Backend &) = delete;
    Geoclue1Backend &operator=(const Geoclue1Backend &) = delete;

    // Configure callbacks for new positions and velocity
    void set_position_callback(PositionCallback cb);
    void set_velocity_callback(VelocityCallback cb);

    // Control tracking based on active GeoClue2 clients
    void start_tracking();
    void stop_tracking();

  private:
    GDBusConnection *m_connection = nullptr;
    PositionCallback m_position_callback;
    VelocityCallback m_velocity_callback;
    bool m_tracking = false;

    // Last velocity data for merging with position updates
    struct {
        bool is_fresh = false;
        double speed = -1.0;     // m/s
        double direction = -1.0; // degrees from north
        double climb = -1.0;     // m/s vertical speed
    } m_last_velocity;

    // Proxies for GeoClue1 objects:
    //
    //  Service:  "org.freedesktop.Geoclue.Master"
    //  Master:   path "/org/freedesktop/Geoclue/Master",
    //            interface "org.freedesktop.Geoclue.Master"
    //  Client:   path returned by Master.Create(),
    //            interface "org.freedesktop.Geoclue.MasterClient"
    //  Provider: service/path obtained from the MasterClient
    //            PositionProviderChanged signal, interfaces:
    //              - "org.freedesktop.Geoclue"
    //              - "org.freedesktop.Geoclue.Position"
    GDBusProxy *m_master_proxy = nullptr;
    GDBusProxy *m_client_proxy = nullptr;
    GDBusProxy *m_provider_proxy = nullptr;
    GDBusProxy *m_position_proxy = nullptr;

    // Master/client lifecycle
    bool ensure_master_client();
    void destroy_master_client();

    // Internal helpers
    void subscribe_signals();
    void unsubscribe_signals();

    // Signal subscription IDs for cleanup
    guint m_position_provider_subscription_id = 0;
    guint m_position_subscription_id = 0;
    guint m_velocity_subscription_id = 0;

    // GeoClue1 PositionChanged signal handler
    static void on_position_changed(GDBusConnection *connection, const char *sender_name,
                                    const char *object_path, const char *interface_name,
                                    const char *signal_name, GVariant *parameters,
                                    gpointer user_data);

    // GeoClue1 MasterClient PositionProviderChanged signal handler
    static void on_position_provider_changed(GDBusConnection *connection, const char *sender_name,
                                             const char *object_path, const char *interface_name,
                                             const char *signal_name, GVariant *parameters,
                                             gpointer user_data);

    // GeoClue1 VelocityChanged signal handler
    static void on_velocity_changed(GDBusConnection *connection, const char *sender_name,
                                    const char *object_path, const char *interface_name,
                                    const char *signal_name, GVariant *parameters,
                                    gpointer user_data);
};
