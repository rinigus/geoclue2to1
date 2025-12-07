#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "geoclue1_backend.h"
#include "geoclue2_manager.h"

/**
 * Entry point for the geoclue2to1 bridge daemon.
 *
 * This is intentionally minimal at this stage:
 * - Initializes GLib and dbus-glib
 * - Connects to the system bus
 * - Requests the org.freedesktop.GeoClue2 name
 * - Starts the GLib main loop
 *
 * TODO:
 * - Register the GeoClue2 Manager/Client/Location objects
 * - Wire them to the Geoclue1 backend
 * - Add CLI options for debug/grace-timeout
 */

namespace {

const char *GEOCLUE2_BUS_NAME = "org.freedesktop.GeoClue2";

// Global state for clean shutdown
GMainLoop *g_main_loop_ptr = nullptr;
GDBusConnection *g_connection_ptr = nullptr;
std::shared_ptr<GeoClue2Manager> g_manager;
std::shared_ptr<Geoclue1Backend> g_backend;

/**
 * Common signal handler for SIGINT/SIGTERM.
 * Runs in the GLib main context via g_unix_signal_add(), so it is safe to
 * call GLib APIs and our C++ objects here.
 */
gboolean on_unix_signal(gpointer data) {
    int signum = GPOINTER_TO_INT(data);
    g_message("Received signal %d, initiating shutdown", signum);

    // Shut down backend - any active clients will be cleaned up automatically
    if (g_backend) {
        g_backend->stop_tracking();
        g_backend.reset(); // triggers Geoclue1Backend destructor and cleanup
    }

    if (g_main_loop_ptr) {
        g_main_loop_quit(g_main_loop_ptr);
    }

    return G_SOURCE_REMOVE;
}

void setup_unix_signal_handlers() {
#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, on_unix_signal, GINT_TO_POINTER(SIGINT));
    g_unix_signal_add(SIGTERM, on_unix_signal, GINT_TO_POINTER(SIGTERM));
#endif
}

void log_init(bool debug_enabled) {
    if (debug_enabled) {
        g_log_set_handler(nullptr, static_cast<GLogLevelFlags>(G_LOG_LEVEL_MASK),
                          g_log_default_handler, nullptr);
        g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
    }
}

struct CommandLineOptions {
    bool debug = false;
    int grace_timeout_ms = 15000; // default 15 seconds
};

CommandLineOptions parse_command_line(int *argc, char ***argv) {
    CommandLineOptions opts;

    GOptionEntry entries[] = {
        {"debug", 0, 0, G_OPTION_ARG_NONE, &opts.debug, "Enable debug logging", nullptr},
        {"grace-timeout", 0, 0, G_OPTION_ARG_INT, &opts.grace_timeout_ms,
         "Grace timeout in milliseconds before stopping "
         "GeoClue1 when no clients are active",
         "MILLISECONDS"},
        {nullptr}};

    GError *error = nullptr;
    GOptionContext *context = g_option_context_new("- GeoClue2 to GeoClue1 bridge daemon");
    g_option_context_add_main_entries(context, entries, nullptr);

    if (!g_option_context_parse(context, argc, argv, &error)) {
        g_printerr("Failed to parse options: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(context);
        std::exit(EXIT_FAILURE);
    }

    g_option_context_free(context);
    return opts;
}

GDBusConnection *connect_system_bus() {
    GError *error = nullptr;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!connection) {
        g_printerr("Failed to connect to system bus: %s\n",
                   error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
        std::exit(EXIT_FAILURE);
    }
    return connection;
}

void acquire_bus_name(GDBusConnection *connection) {
    GError *error = nullptr;

    GVariant *result = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", GEOCLUE2_BUS_NAME, 0 /* flags */),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

    if (!result) {
        g_printerr("Failed to request bus name '%s': %s\n", GEOCLUE2_BUS_NAME,
                   error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
        std::exit(EXIT_FAILURE);
    }

    guint request_name_result;
    g_variant_get(result, "(u)", &request_name_result);
    g_variant_unref(result);

    g_message("Acquired D-Bus name '%s' with result code %u", GEOCLUE2_BUS_NAME,
              request_name_result);
}

} // namespace

int main(int argc, char **argv) {
#if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
#endif

    // Parse command line options
    CommandLineOptions options = parse_command_line(&argc, &argv);

    // Initialize logging
    log_init(options.debug);
    g_message("Starting geoclue2to1 bridge daemon");

    // Connect to the system bus
    GDBusConnection *connection = connect_system_bus();
    g_connection_ptr = connection;

    // Request org.freedesktop.GeoClue2 name
    acquire_bus_name(connection);

    // Initialize and register:
    // - GeoClue2 Manager object at /org/freedesktop/GeoClue2/Manager
    //   (Client and Location objects will be created on demand)
    // - Geoclue1 backend and client registry (to be implemented), using
    //   options.grace_timeout_ms for GPS stop grace timeout
    std::shared_ptr<GeoClue2Manager> manager = geoclue2_manager_register(connection);
    if (!manager) {
        g_printerr("Failed to register GeoClue2 Manager on D-Bus\n");
        return EXIT_FAILURE;
    }
    g_manager = manager;

    // Start the GLib main loop
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    if (!loop) {
        g_printerr("Failed to create main loop\n");
        return EXIT_FAILURE;
    }
    g_main_loop_ptr = loop;

    // Install signal handlers for clean shutdown (Ctrl+C, systemd stop)
    setup_unix_signal_handlers();

    // Create GeoClue1 backend
    g_backend = std::make_shared<Geoclue1Backend>(connection);

    // Wire position callback to broadcast updates to all active GeoClue2 clients
    g_backend->set_position_callback([manager](const GeoClue1Position &pos) {
        g_debug("GeoClue1 position: lat=%.6f lon=%.6f alt=%.1f acc=%.1f "
                "speed=%.1f heading=%.1f",
                pos.latitude, pos.longitude, pos.altitude, pos.accuracy, pos.speed, pos.heading);

        // Broadcast to all active clients
        manager->handle_position_update(pos);
    });

    // Velocity callback for debugging (actual data is merged in on_position_changed)
    g_backend->set_velocity_callback([](const GeoClue1Velocity &vel) {
        g_debug("GeoClue1 velocity: speed=%.1f direction=%.1f climb=%.1f", vel.speed, vel.direction,
                vel.climb);
    });

    // Hand backend to the Manager so it can manage GPS lifecycle
    manager->set_backend(g_backend);

    g_message("GeoClue2 bridge ready - waiting for client connections");

    g_main_loop_run(loop);

    g_message("Shutting down");

    // Explicit cleanup in case we exited without a signal
    if (g_backend) {
        g_backend->stop_tracking();
        g_backend.reset();
    }

    g_main_loop_unref(loop);
    g_main_loop_ptr = nullptr;

    return EXIT_SUCCESS;
}
