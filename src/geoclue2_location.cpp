#include "geoclue2_location.h"

#include <ctime>

/**
 * Implementation of the GeoClue2 Location object.
 *
 * Uses the generated GClueLocationSkeleton to expose location properties
 * on D-Bus. Location objects are read-only and immutable once created.
 */

GeoClue2Location::GeoClue2Location(GDBusConnection *connection, const std::string &object_path)
    : m_connection(connection), m_object_path(object_path) {
    g_return_if_fail(connection != nullptr);

    // Create skeleton for org.freedesktop.GeoClue2.Location interface
    GClueLocation *location_iface = gclue_location_skeleton_new();
    m_skeleton = GCLUE_LOCATION_SKELETON(location_iface);

    // NOTE: Properties will be set via set_from_geoclue1_position()
    // BEFORE this object is exported to D-Bus. This ensures clients
    // immediately see valid data when the object appears.
    // Export is delayed until after properties are set.
}

GeoClue2Location::~GeoClue2Location() {
    if (m_registration_id != 0) {
        g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(m_skeleton));
        m_registration_id = 0;
    }

    if (m_skeleton) {
        g_object_unref(m_skeleton);
        m_skeleton = nullptr;
    }

    g_debug("GeoClue2Location destroyed at %s", m_object_path.c_str());
}

void GeoClue2Location::set_from_geoclue1_position(const GeoClue1Position &pos) {
    if (!m_skeleton) {
        return;
    }

    // Set basic position properties
    gclue_location_set_latitude(GCLUE_LOCATION(m_skeleton), pos.latitude);
    gclue_location_set_longitude(GCLUE_LOCATION(m_skeleton), pos.longitude);
    gclue_location_set_accuracy(GCLUE_LOCATION(m_skeleton), pos.accuracy);

    // Set altitude (real value from GeoClue1)
    gclue_location_set_altitude(GCLUE_LOCATION(m_skeleton), pos.altitude);

    // Set speed (-1.0 if unknown)
    gclue_location_set_speed(GCLUE_LOCATION(m_skeleton), pos.speed);

    // Set heading (-1.0 if unknown)
    gclue_location_set_heading(GCLUE_LOCATION(m_skeleton), pos.heading);

    // Set description (empty for now)
    gclue_location_set_description(GCLUE_LOCATION(m_skeleton), "");

    // Parse timestamp from ISO8601 string to (seconds, microseconds) tuple
    gint64 timestamp_sec = 0;
    gint64 timestamp_usec = 0;

    if (!pos.timestamp_iso8601.empty()) {
        // GeoClue1 provides Unix timestamp as string
        try {
            timestamp_sec = std::stoll(pos.timestamp_iso8601);
            timestamp_usec = 0;
        } catch (...) {
            // Fall back to current time
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            timestamp_sec = ts.tv_sec;
            timestamp_usec = ts.tv_nsec / 1000;
        }
    } else {
        // No timestamp provided, use current time
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        timestamp_sec = ts.tv_sec;
        timestamp_usec = ts.tv_nsec / 1000;
    }

    // Create GVariant tuple (tt) for timestamp
    GVariant *timestamp = g_variant_new("(tt)", (guint64)timestamp_sec, (guint64)timestamp_usec);
    gclue_location_set_timestamp(GCLUE_LOCATION(m_skeleton), timestamp);

    // NOW export to D-Bus after all properties are set
    // This ensures clients see valid data immediately
    if (m_registration_id == 0) {
        GError *error = nullptr;
        m_registration_id = g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(m_skeleton), m_connection, m_object_path.c_str(), &error);

        if (error) {
            g_warning("Failed to export Location at %s: %s", m_object_path.c_str(), error->message);
            g_error_free(error);
            return;
        }

        g_debug("GeoClue2Location exported at %s", m_object_path.c_str());
    }

    g_debug("Location updated at %s: lat=%.6f, lon=%.6f, alt=%.1f, "
            "acc=%.1f, speed=%.1f, heading=%.1f",
            m_object_path.c_str(), pos.latitude, pos.longitude, pos.altitude, pos.accuracy,
            pos.speed, pos.heading);
}
