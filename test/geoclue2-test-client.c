/*
 * Simple GeoClue2 test client
 *
 * Demonstrates basic usage of the GeoClue2 D-Bus API.
 * Similar in functionality to geoclue2's where-am-i demo.
 */

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static GMainLoop *loop = NULL;
static GDBusProxy *manager_proxy = NULL;
static GDBusProxy *client_proxy = NULL;
static gchar *client_path = NULL;
static guint location_signal_id = 0;

// Signal handler for clean exit
static gboolean on_signal(gpointer user_data) {
    int signum = GPOINTER_TO_INT(user_data);
    g_print("\nReceived signal %d, stopping...\n", signum);
    if (loop)
        g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

// Helper to get a double property from a proxy
static gdouble get_double_property(GDBusProxy *proxy, const gchar *property) {
    GVariant *variant = g_dbus_proxy_get_cached_property(proxy, property);
    if (!variant)
        return 0.0;

    gdouble value = g_variant_get_double(variant);
    g_variant_unref(variant);
    return value;
}

// Helper to get a string property from a proxy
static gchar *get_string_property(GDBusProxy *proxy, const gchar *property) {
    GVariant *variant = g_dbus_proxy_get_cached_property(proxy, property);
    if (!variant)
        return g_strdup("");

    gchar *value = g_variant_dup_string(variant, NULL);
    g_variant_unref(variant);
    return value;
}

// Print location information
static void print_location(const gchar *location_path) {
    GError *error = NULL;
    GDBusProxy *location_proxy;
    gdouble lat, lon, accuracy, altitude, speed, heading;
    GVariant *timestamp_variant;
    guint64 timestamp_sec, timestamp_usec;

    if (g_strcmp0(location_path, "/") == 0) {
        g_print("Location: (none)\n");
        return;
    }

    location_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.GeoClue2", location_path,
        "org.freedesktop.GeoClue2.Location", NULL, &error);

    if (!location_proxy) {
        g_printerr("Failed to create Location proxy: %s\n", error->message);
        g_error_free(error);
        return;
    }

    // Get all properties
    lat = get_double_property(location_proxy, "Latitude");
    lon = get_double_property(location_proxy, "Longitude");
    accuracy = get_double_property(location_proxy, "Accuracy");
    altitude = get_double_property(location_proxy, "Altitude");
    speed = get_double_property(location_proxy, "Speed");
    heading = get_double_property(location_proxy, "Heading");

    timestamp_variant = g_dbus_proxy_get_cached_property(location_proxy, "Timestamp");
    if (timestamp_variant) {
        g_variant_get(timestamp_variant, "(tt)", &timestamp_sec, &timestamp_usec);
        g_variant_unref(timestamp_variant);
    } else {
        timestamp_sec = 0;
        timestamp_usec = 0;
    }

    // Print location data
    g_print("\n=== Location Update ===\n");
    g_print("Path:        %s\n", location_path);
    g_print("Latitude:    %.6f\n", lat);
    g_print("Longitude:   %.6f\n", lon);
    g_print("Accuracy:    %.1f meters\n", accuracy);

    // Only print altitude if it's a valid value (not the "unknown" sentinel)
    if (altitude > -1e308) {
        g_print("Altitude:    %.1f meters\n", altitude);
    }

    // Only print speed if it's known (>= 0)
    if (speed >= 0.0) {
        g_print("Speed:       %.2f m/s (%.1f km/h)\n", speed, speed * 3.6);
    }

    // Only print heading if it's known (>= 0)
    if (heading >= 0.0) {
        g_print("Heading:     %.1f from North\n", heading);
    }

    if (timestamp_sec > 0) {
        GDateTime *dt = g_date_time_new_from_unix_utc(timestamp_sec);
        gchar *timestamp_str = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S UTC");
        g_print("Timestamp:   %s\n", timestamp_str);
        g_free(timestamp_str);
        g_date_time_unref(dt);
    }

    g_object_unref(location_proxy);
}

// LocationUpdated signal handler
static void on_location_updated(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name,
                                GVariant *parameters, gpointer user_data) {
    const gchar *old_path, *new_path;

    if (g_strcmp0(signal_name, "LocationUpdated") != 0)
        return;

    g_variant_get(parameters, "(&o&o)", &old_path, &new_path);
    print_location(new_path);
}

int main(int argc, char *argv[]) {
    GError *error = NULL;
    GVariant *result;

    // Setup signal handlers
    g_unix_signal_add(SIGINT, on_signal, GINT_TO_POINTER(SIGINT));
    g_unix_signal_add(SIGTERM, on_signal, GINT_TO_POINTER(SIGTERM));

    g_print("GeoClue2 Test Client\n");
    g_print("====================\n\n");

    // Connect to Manager
    g_print("Connecting to GeoClue2 Manager...\n");
    manager_proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.GeoClue2",
        "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager", NULL, &error);

    if (!manager_proxy) {
        g_printerr("Failed to connect to Manager: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // Get Client
    g_print("Calling GetClient()...\n");
    result = g_dbus_proxy_call_sync(manager_proxy, "GetClient", NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                                    NULL, &error);

    if (!result) {
        g_printerr("GetClient failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    g_variant_get(result, "(o)", &client_path);
    g_variant_unref(result);

    g_print("Got client at: %s\n", client_path);

    // Create Client proxy
    client_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                                 "org.freedesktop.GeoClue2", client_path,
                                                 "org.freedesktop.GeoClue2.Client", NULL, &error);

    if (!client_proxy) {
        g_printerr("Failed to create Client proxy: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    // Set DesktopId property
    g_print("Setting DesktopId...\n");
    g_dbus_proxy_call_sync(client_proxy, "org.freedesktop.DBus.Properties.Set",
                           g_variant_new("(ssv)", "org.freedesktop.GeoClue2.Client", "DesktopId",
                                         g_variant_new_string("geoclue2-test-client")),
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

    // Subscribe to LocationUpdated signal
    location_signal_id =
        g_signal_connect(client_proxy, "g-signal", G_CALLBACK(on_location_updated), NULL);

    // Start client
    g_print("Starting location updates...\n");
    result = g_dbus_proxy_call_sync(client_proxy, "Start", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                    &error);

    if (!result) {
        g_printerr("Start failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    g_variant_unref(result);

    // Check if there's already a location available
    g_print("Checking for current location...\n");
    gchar *current_location = get_string_property(client_proxy, "Location");
    if (current_location && g_strcmp0(current_location, "/") != 0) {
        g_print("Current location available:\n");
        print_location(current_location);
    } else {
        g_print("No current location yet, waiting for updates...\n");
    }
    g_free(current_location);

    // Run main loop
    loop = g_main_loop_new(NULL, FALSE);
    g_print("\nListening for location updates (Ctrl+C to exit)...\n");
    g_main_loop_run(loop);

    // Cleanup on exit
    g_print("\nStopping client...\n");

    if (location_signal_id) {
        g_signal_handler_disconnect(client_proxy, location_signal_id);
    }

    g_dbus_proxy_call_sync(client_proxy, "Stop", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

    if (client_proxy)
        g_object_unref(client_proxy);
    if (manager_proxy)
        g_object_unref(manager_proxy);
    g_free(client_path);
    g_main_loop_unref(loop);

    g_print("Test client exited cleanly.\n");

    return 0;
}