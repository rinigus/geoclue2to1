#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <memory>
#include <string>

#include "geoclue1_backend.h"
#include "geoclue2-location.h"

/**
 * GeoClue2 Location interface.
 *
 * Represents an org.freedesktop.GeoClue2.Location object on D-Bus.
 * Exposes read-only properties such as Latitude, Longitude, Accuracy, etc.
 */

class GeoClue2Location {
  public:
    GeoClue2Location(GDBusConnection *connection, const std::string &object_path);
    ~GeoClue2Location();

    // Non-copyable
    GeoClue2Location(const GeoClue2Location &) = delete;
    GeoClue2Location &operator=(const GeoClue2Location &) = delete;

    // Set position data from GeoClue1
    void set_from_geoclue1_position(const GeoClue1Position &pos);

    // Get the object path
    const std::string &get_path() const { return m_object_path; }

  private:
    GDBusConnection *m_connection;
    std::string m_object_path;
    GClueLocationSkeleton *m_skeleton = nullptr;
    guint m_registration_id = 0;
};
