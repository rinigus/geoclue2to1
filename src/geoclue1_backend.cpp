#include "geoclue1_backend.h"

#include <cmath>
#include <utility>

// Number of location updates while velocity is considered fresh
const size_t VELOCITY_FRESH_STEPS = 2;

Geoclue1Backend::Geoclue1Backend(GDBusConnection * /*connection*/) {
    // GeoClue1 runs on the *session* bus, not the system bus.
    // We therefore connect to the session bus here, independently of
    // the system-bus connection used for the GeoClue2 side.
    GError *error = nullptr;
    m_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!m_connection) {
        g_warning("Geoclue1Backend: failed to connect to session bus: %s",
                  error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
        // Leave m_connection == nullptr; ensure_master_client() will fail
        // gracefully and callers will see start_tracking() do nothing.
        return;
    }

    g_message("Geoclue1Backend created (using session bus)");
}

Geoclue1Backend::~Geoclue1Backend() {
    // Ensure all D-Bus resources are cleaned up.
    g_message("Geoclue1Backend::~Geoclue1Backend: destroying master client");
    destroy_master_client();

    g_message("Geoclue1Backend destroyed");
}

void Geoclue1Backend::set_position_callback(PositionCallback cb) {
    m_position_callback = std::move(cb);
}

void Geoclue1Backend::set_velocity_callback(VelocityCallback cb) {
    m_velocity_callback = std::move(cb);
}

void Geoclue1Backend::start_tracking() {
    if (m_tracking) {
        g_message("Geoclue1Backend::start_tracking: already tracking");
        return;
    }

    if (!ensure_master_client()) {
        g_warning("Geoclue1Backend::start_tracking: failed to ensure GeoClue1 "
                  "master client");
        return;
    }

    g_message("Geoclue1Backend: starting tracking");
    // PositionChanged subscription is done once a provider is announced via
    // PositionProviderChanged, mirroring the Qt plugin behaviour.
    m_tracking = true;
}

void Geoclue1Backend::stop_tracking() {
    if (!m_tracking) {
        g_message("Geoclue1Backend::stop_tracking: not tracking");
        // Even if m_tracking is false, we may still have lingering proxies;
        // fall through to destroy_master_client() to be safe.
    } else {
        g_message("Geoclue1Backend: stopping tracking");
        // Unsubscribe from position-changed signals
        unsubscribe_signals();
        m_tracking = false;
    }

    // Fully tear down the GeoClue1 master client and provider so that
    // geoclue-master/hybris see that there are no more users and can
    // power down GPS.
    g_message("Geoclue1Backend::stop_tracking: destroying master client");
    destroy_master_client();
}

void Geoclue1Backend::subscribe_signals() {
    // PositionProviderChanged subscription is now done in ensure_master_client()
    // This method is kept for potential future use or cleanup
    g_message("Geoclue1Backend::subscribe_signals: PositionProviderChanged "
              "already subscribed");
}

void Geoclue1Backend::unsubscribe_signals() {
    if (m_position_subscription_id != 0) {
        g_dbus_connection_signal_unsubscribe(m_connection, m_position_subscription_id);
        m_position_subscription_id = 0;
    }

    if (m_velocity_subscription_id != 0) {
        g_dbus_connection_signal_unsubscribe(m_connection, m_velocity_subscription_id);
        m_velocity_subscription_id = 0;
    }

    if (m_position_provider_subscription_id != 0) {
        g_dbus_connection_signal_unsubscribe(m_connection, m_position_provider_subscription_id);
        m_position_provider_subscription_id = 0;
    }

    g_message("Geoclue1Backend: unsubscribed from signals");
}

void Geoclue1Backend::on_position_changed(GDBusConnection * /*connection*/,
                                          const char * /*sender_name*/,
                                          const char * /*object_path*/,
                                          const char * /*interface_name*/,
                                          const char * /*signal_name*/, GVariant *parameters,
                                          gpointer user_data) {
    auto *backend = static_cast<Geoclue1Backend *>(user_data);
    if (!backend) {
        return;
    }

    gint fields = 0, timestamp_int = 0;
    gdouble latitude = 0.0, longitude = 0.0, altitude = 0.0;
    gint32 accuracy_level = 0;
    gdouble accuracy_h = 0.0, accuracy_v = 0.0;

    g_variant_get(parameters, "(iiddd(idd))", &fields, &timestamp_int, &latitude, &longitude,
                  &altitude, &accuracy_level, &accuracy_h, &accuracy_v);

    // g_debug("on_position_changed: fields=%d, ts=%d, lat=%f, lon=%f, alt=%f, "
    //         "acc_level=%d, acc_h=%f, acc_v=%f",
    //         fields, timestamp_int, latitude, longitude, altitude, accuracy_level, accuracy_h,
    //         accuracy_v);

    GeoClue1Position pos;

    pos.latitude = latitude;
    pos.longitude = longitude;
    pos.altitude = altitude;
    pos.accuracy = accuracy_h;
    pos.timestamp_iso8601 = std::to_string(timestamp_int);

    // Merge velocity data if available and fresh
    if (backend->m_last_velocity.is_fresh > 0) {
        pos.speed = backend->m_last_velocity.speed;
        pos.heading = backend->m_last_velocity.direction;
        pos.climb = backend->m_last_velocity.climb;
        backend->m_last_velocity.is_fresh -= 1;
        // g_debug("on_position_changed: merged velocity: speed=%f, heading=%f, climb=%f",
        // pos.speed, pos.heading, pos.climb);
    } else {
        pos.speed = -1.0;   // Unknown
        pos.heading = -1.0; // Unknown
        pos.climb = -1.0;   // Unknown
    }

    if (backend->m_position_callback) {
        backend->m_position_callback(pos);
    }
}

void Geoclue1Backend::on_velocity_changed(GDBusConnection * /*connection*/,
                                          const char * /*sender_name*/,
                                          const char * /*object_path*/,
                                          const char * /*interface_name*/,
                                          const char * /*signal_name*/, GVariant *parameters,
                                          gpointer user_data) {
    auto *backend = static_cast<Geoclue1Backend *>(user_data);
    if (!backend) {
        return;
    }

    gint fields = 0, timestamp_int = 0;
    gdouble speed = 0.0, direction = 0.0, climb = 0.0;

    g_variant_get(parameters, "(iiddd)", &fields, &timestamp_int, &speed, &direction, &climb);
    // g_debug("on_velocity_changed: fields=%d, ts=%d, speed=%f, direction=%f, climb=%f", fields,
    //         timestamp_int, speed, direction, climb);

    // Store velocity data for merging with next position update
    // Sanitize NaN values to -1.0 (GeoClue2 convention for "unknown")
    backend->m_last_velocity.speed = std::isnan(speed) ? -1.0 : speed;
    backend->m_last_velocity.direction = std::isnan(direction) ? -1.0 : direction;
    backend->m_last_velocity.climb = std::isnan(climb) ? -1.0 : climb;
    backend->m_last_velocity.is_fresh = VELOCITY_FRESH_STEPS;

    // Also call the velocity callback if set (for logging/debugging)
    if (backend->m_velocity_callback) {
        GeoClue1Velocity velocity;
        velocity.speed = speed;
        velocity.direction = direction;
        velocity.climb = climb;
        velocity.timestamp_iso8601 = std::to_string(timestamp_int);
        backend->m_velocity_callback(velocity);
    }
}

bool Geoclue1Backend::ensure_master_client() {
    if (m_master_proxy && m_client_proxy) {
        // Already have a master client set up.
        return true;
    }

    g_return_val_if_fail(m_connection != nullptr, false);

    GError *error = nullptr;

    // Create master proxy if needed.
    if (!m_master_proxy) {
        m_master_proxy = g_dbus_proxy_new_sync(
            m_connection, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.freedesktop.Geoclue.Master",
            "/org/freedesktop/Geoclue/Master", "org.freedesktop.Geoclue.Master", nullptr, &error);

        if (!m_master_proxy) {
            g_warning("Geoclue1Backend::ensure_master_client: failed to create "
                      "master proxy: %s",
                      error ? error->message : "unknown");
            if (error)
                g_error_free(error);
            return false;
        }
    }

    // Call Master.Create() to get client object path.
    GVariant *create_result = g_dbus_proxy_call_sync(m_master_proxy, "Create", g_variant_new("()"),
                                                     G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!create_result) {
        g_warning("Geoclue1Backend::ensure_master_client: Master.Create failed: %s",
                  error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return false;
    }

    const char *client_path_cstr;
    g_variant_get(create_result, "(o)", &client_path_cstr);
    g_variant_unref(create_result);

    std::string client_path = client_path_cstr ? client_path_cstr : "";

    if (client_path.empty()) {
        g_warning("Geoclue1Backend::ensure_master_client: Master.Create returned "
                  "empty path");
        return false;
    }

    g_message("Geoclue1Backend: created GeoClue1 client at %s", client_path.c_str());

    // Subscribe to PositionProviderChanged signal from our specific MasterClient
    m_position_provider_subscription_id = g_dbus_connection_signal_subscribe(
        m_connection,
        nullptr,                                // sender (any)
        "org.freedesktop.Geoclue.MasterClient", // interface
        "PositionProviderChanged",              // signal name
        client_path.c_str(),                    // object path (our specific client)
        nullptr,                                // arg0 (any)
        G_DBUS_SIGNAL_FLAGS_NONE, &Geoclue1Backend::on_position_provider_changed, this, nullptr);

    g_message("Geoclue1Backend: subscribed to PositionProviderChanged on %s", client_path.c_str());

    // Create MasterClient proxy on the returned path.
    m_client_proxy = g_dbus_proxy_new_sync(m_connection, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                           "org.freedesktop.Geoclue.Master", client_path.c_str(),
                                           "org.freedesktop.Geoclue.MasterClient", nullptr, &error);

    if (!m_client_proxy) {
        g_warning("Geoclue1Backend::ensure_master_client: failed to create "
                  "MasterClient proxy: %s",
                  error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return false;
    }

    // The MasterClient also implements org.freedesktop.Geoclue interface.
    // We must call AddReference() on it to properly activate GPS resources,
    // matching the pattern used by qtlocation-geoclue plugin.
    GDBusProxy *client_geoclue_proxy = g_dbus_proxy_new_sync(
        m_connection, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.freedesktop.Geoclue.Master",
        client_path.c_str(), "org.freedesktop.Geoclue", nullptr, &error);

    if (client_geoclue_proxy) {
        GVariant *add_ref_result =
            g_dbus_proxy_call_sync(client_geoclue_proxy, "AddReference", g_variant_new("()"),
                                   G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (!add_ref_result) {
            g_warning("Geoclue1Backend::ensure_master_client: AddReference on client "
                      "failed: %s",
                      error ? error->message : "unknown");
            if (error)
                g_error_free(error);
            // Continue anyway, but GPS refcount may be off
        } else {
            g_variant_unref(add_ref_result);
            g_message("Geoclue1Backend::ensure_master_client: AddReference on client "
                      "succeeded");
        }
        g_object_unref(client_geoclue_proxy);
    } else {
        if (error)
            g_error_free(error);
    }

    // SetRequirements(accuracyLevel, time, requireUpdates, allowedResources)
    // For now, use Accuracy::None (0), time 0, requireUpdates=true, ResourceAll =
    // (1 << 10) - 1.
    const gint accuracy_level = 0;
    const gint time_limit = 0;
    const gboolean require_updates = TRUE;
    const gint allowed_resources = (1 << 10) - 1;

    GVariant *set_req_result = g_dbus_proxy_call_sync(
        m_client_proxy, "SetRequirements",
        g_variant_new("(iibi)", accuracy_level, time_limit, require_updates, allowed_resources),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!set_req_result) {
        g_warning("Geoclue1Backend::ensure_master_client: SetRequirements failed: %s",
                  error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return false;
    }
    g_variant_unref(set_req_result);

    // Start positioning.
    GVariant *pos_start_result =
        g_dbus_proxy_call_sync(m_client_proxy, "PositionStart", g_variant_new("()"),
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!pos_start_result) {
        g_warning("Geoclue1Backend::ensure_master_client: PositionStart failed: %s",
                  error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return false;
    }
    g_variant_unref(pos_start_result);

    // IMPORTANT: We do NOT treat the absence of a provider
    // immediately as fatal. geoclue-master will emit PositionProviderChanged
    // when a provider is selected. That signal will create m_provider_proxy
    // and m_position_proxy and call AddReference(), mirroring the Qt plugin.
    return true;
}

void Geoclue1Backend::destroy_master_client() {
    GError *error = nullptr;
    g_message("Geoclue1Backend::destroy_master_client: begin teardown");

    if (m_position_proxy) {
        g_message("Geoclue1Backend::destroy_master_client: unref Position proxy");
        g_object_unref(m_position_proxy);
        m_position_proxy = nullptr;
    }

    if (m_provider_proxy) {
        g_message("Geoclue1Backend::destroy_master_client: calling RemoveReference "
                  "on provider");
        GVariant *rem_ref_result =
            g_dbus_proxy_call_sync(m_provider_proxy, "RemoveReference", g_variant_new("()"),
                                   G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (!rem_ref_result) {
            g_warning("Geoclue1Backend::destroy_master_client: RemoveReference call "
                      "failed: %s",
                      error ? error->message : "unknown");
            if (error)
                g_error_free(error);
        } else {
            g_variant_unref(rem_ref_result);
            g_message("Geoclue1Backend::destroy_master_client: RemoveReference succeeded");
        }

        g_message("Geoclue1Backend::destroy_master_client: unref provider proxy");
        g_object_unref(m_provider_proxy);
        m_provider_proxy = nullptr;
    } else {
        g_message("Geoclue1Backend::destroy_master_client: no provider proxy to "
                  "RemoveReference");
    }

    if (m_client_proxy) {
        // The MasterClient object also implements org.freedesktop.Geoclue interface
        // and needs RemoveReference() called on it to properly release GPS
        // resources. Create a proxy for the Geoclue interface on the same client
        // path.
        g_message("Geoclue1Backend::destroy_master_client: calling RemoveReference "
                  "on MasterClient");

        GDBusProxy *client_geoclue_proxy = g_dbus_proxy_new_sync(
            m_connection, G_DBUS_PROXY_FLAGS_NONE, nullptr, g_dbus_proxy_get_name(m_client_proxy),
            g_dbus_proxy_get_object_path(m_client_proxy), "org.freedesktop.Geoclue", nullptr,
            &error);

        if (client_geoclue_proxy) {
            GVariant *rem_ref_client_result =
                g_dbus_proxy_call_sync(client_geoclue_proxy, "RemoveReference", g_variant_new("()"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
            if (!rem_ref_client_result) {
                g_warning("Geoclue1Backend::destroy_master_client: RemoveReference on "
                          "client failed: %s",
                          error ? error->message : "unknown");
                if (error)
                    g_error_free(error);
            } else {
                g_variant_unref(rem_ref_client_result);
                g_message("Geoclue1Backend::destroy_master_client: RemoveReference on "
                          "client succeeded");
            }
            g_object_unref(client_geoclue_proxy);
        } else {
            if (error)
                g_error_free(error);
        }

        g_message("Geoclue1Backend::destroy_master_client: unref MasterClient proxy");
        g_object_unref(m_client_proxy);
        m_client_proxy = nullptr;
    }

    if (m_master_proxy) {
        g_message("Geoclue1Backend::destroy_master_client: unref Master proxy");
        g_object_unref(m_master_proxy);
        m_master_proxy = nullptr;
    }

    m_tracking = false;
    g_message("Geoclue1Backend::destroy_master_client: done");
}

void Geoclue1Backend::on_position_provider_changed(GDBusConnection * /*connection*/,
                                                   const char * /*sender_name*/,
                                                   const char * /*object_path*/,
                                                   const char * /*interface_name*/,
                                                   const char * /*signal_name*/,
                                                   GVariant *parameters, gpointer user_data) {
    GError *error = nullptr;
    auto *backend = static_cast<Geoclue1Backend *>(user_data);
    if (!backend) {
        return;
    }

    const char *name, *description, *service, *path;
    g_variant_get(parameters, "(ssss)", &name, &description, &service, &path);

    const char *service_c = service ? service : "";
    const char *path_c = path ? path : "";

    g_message("Geoclue1Backend::on_position_provider_changed: name=%s desc=%s "
              "service=%s path=%s",
              name ? name : "(null)", description ? description : "(null)", service_c, path_c);

    // geoclue-master may emit an empty service/path while deciding; ignore it.
    if (service_c[0] == '\0' || path_c[0] == '\0') {
        g_message("Geoclue1Backend::on_position_provider_changed: empty "
                  "service/path, ignoring");
        return;
    }

    // If we already have a provider/position, replace them.
    if (backend->m_position_proxy) {
        g_message("Geoclue1Backend::on_position_provider_changed: replacing "
                  "existing Position proxy");
        g_object_unref(backend->m_position_proxy);
        backend->m_position_proxy = nullptr;
    }
    if (backend->m_provider_proxy) {
        g_message("Geoclue1Backend::on_position_provider_changed: replacing "
                  "existing provider proxy");
        GError *error = nullptr;
        GVariant *rem_ref_old_result = g_dbus_proxy_call_sync(
            backend->m_provider_proxy, "RemoveReference", g_variant_new("()"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (!rem_ref_old_result) {
            g_warning("Geoclue1Backend::on_position_provider_changed: "
                      "RemoveReference (old) failed: %s",
                      error->message);
            g_error_free(error);
        } else {
            g_variant_unref(rem_ref_old_result);
        }
        g_object_unref(backend->m_provider_proxy);
        backend->m_provider_proxy = nullptr;
    }

    // Create provider proxy (org.freedesktop.Geoclue) on the new service/path.
    backend->m_provider_proxy =
        g_dbus_proxy_new_sync(backend->m_connection, G_DBUS_PROXY_FLAGS_NONE, nullptr, service_c,
                              path_c, "org.freedesktop.Geoclue", nullptr, &error);

    if (!backend->m_provider_proxy) {
        g_warning("Geoclue1Backend::on_position_provider_changed: failed to create "
                  "provider proxy: %s",
                  error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return;
    }

    // AddReference() so the provider stays alive.
    GVariant *add_ref_new_result =
        g_dbus_proxy_call_sync(backend->m_provider_proxy, "AddReference", g_variant_new("()"),
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!add_ref_new_result) {
        g_warning("Geoclue1Backend::on_position_provider_changed: AddReference "
                  "failed: %s",
                  error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        // Keep going; provider might still be usable, but GPS refcount may be off.
    } else {
        g_variant_unref(add_ref_new_result);
        g_message("Geoclue1Backend::on_position_provider_changed: AddReference "
                  "succeeded");
    }

    // Create Position proxy (org.freedesktop.Geoclue.Position).
    backend->m_position_proxy =
        g_dbus_proxy_new_sync(backend->m_connection, G_DBUS_PROXY_FLAGS_NONE, nullptr, service_c,
                              path_c, "org.freedesktop.Geoclue.Position", nullptr, &error);

    if (!backend->m_position_proxy) {
        g_warning("Geoclue1Backend::on_position_provider_changed: failed to create "
                  "Position proxy: %s",
                  error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return;
    }

    g_message("Geoclue1Backend::on_position_provider_changed: provider "
              "service=%s path=%s",
              service_c, path_c);

    // Subscribe to PositionChanged signal from the provider
    backend->m_position_subscription_id = g_dbus_connection_signal_subscribe(
        backend->m_connection,
        service_c,                          // sender (specific service)
        "org.freedesktop.Geoclue.Position", // interface
        "PositionChanged",                  // signal name
        path_c,                             // object path (specific path)
        nullptr,                            // arg0 (any)
        G_DBUS_SIGNAL_FLAGS_NONE, &Geoclue1Backend::on_position_changed, backend, nullptr);

    g_message("Geoclue1Backend: subscribed to PositionChanged from %s", service_c);

    // Subscribe to VelocityChanged signal from the provider
    backend->m_velocity_subscription_id = g_dbus_connection_signal_subscribe(
        backend->m_connection,
        service_c,                          // sender (specific service)
        "org.freedesktop.Geoclue.Velocity", // interface
        "VelocityChanged",                  // signal name
        path_c,                             // object path (specific path)
        nullptr,                            // arg0 (any)
        G_DBUS_SIGNAL_FLAGS_NONE, &Geoclue1Backend::on_velocity_changed, backend, nullptr);

    g_message("Geoclue1Backend: subscribed to VelocityChanged from %s", service_c);
}
