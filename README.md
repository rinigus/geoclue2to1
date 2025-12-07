# GeoClue2-to-GeoClue1 Bridge for Sailfish OS

A D-Bus service that implements the GeoClue2 API and bridges it to the GeoClue1 backend available on Sailfish OS. This allows applications (including Qt6 apps) that require GeoClue2 to access location services on Sailfish OS devices.

## Install

geoclue2to1 service is available at Chum. There is no configuration needed after installation.

## Architecture

```
Qt6/Modern Apps → GeoClue2 API (system bus) → geoclue2to1 bridge → GeoClue1 API (session bus) → GPS Hardware
```

### Key Features

- **Full GeoClue2 D-Bus API**: Manager, Client, and Location interfaces
- **Position + Velocity Merging**: Combines GeoClue1's separate Position and Velocity signals
- **GPS Power Management**: Automatic GPS shutdown with configurable grace timeout
- **Multiple Client Support**: Handle multiple applications simultaneously
- **Complete Position Data**: Latitude, longitude, altitude, accuracy, speed, heading

## Testing

### Test Client Application

A test client `geoclue2-test-client` is included to verify the service works correctly. It is available in `geoclue2to1-test` package and should automatically activate geoclue2to1 service:

```bash
> geoclue2-test-client

GeoClue2 Test Client
====================

Connecting to GeoClue2 Manager...
Calling GetClient()...
Got client at: /org/freedesktop/GeoClue2/Client/1
Setting DesktopId...
Starting location updates...
Checking for current location...
Current location available:

=== Location Update ===
Path:        /org/freedesktop/GeoClue2/Location/1
Latitude:    59
Longitude:   24
Accuracy:    15.0 meters
Altitude:    45.2 meters
Speed:       0.50 m/s (1.8 km/h)
Heading:     185.3 from North
Timestamp:   2025-12-07 12:15:30 UTC

Listening for location updates (Ctrl+C to exit)...

=== Location Update ===
Path:        /org/freedesktop/GeoClue2/Location/2
...
```

### Command Line Options

```bash
geoclue2to1 [OPTIONS]

Options:
  --debug                 Enable debug logging
  --grace-timeout MSEC    Grace timeout in milliseconds before stopping
                          GPS when no clients are active (default: 15000)
  --help                  Show help message
```

## Implementation Details

### Data Flow

1. **Client Connection**: App calls `Manager.GetClient()` → creates Client object
2. **Start Tracking**: App calls `Client.Start()` → Manager starts GeoClue1 backend
3. **Position Updates**:
   - GeoClue1 emits `PositionChanged` + `VelocityChanged` signals (session bus)
   - Backend merges velocity data with position
   - Manager creates new Location object
   - Manager broadcasts to all active Clients via `LocationUpdated` signal
4. **Stop Tracking**: App calls `Client.Stop()` → Client becomes inactive
5. **GPS Shutdown**: After grace timeout (15s) with no active clients, Manager stops GeoClue1

### GPS Lifecycle Management

The bridge ensures proper GPS power management:

- **Reference Counting**: Calls `AddReference()` when starting, `RemoveReference()` when stopping
- **Grace Timeout**: Configurable delay (default 15s) before stopping GPS
- **Multiple Clients**: GPS remains active while any client is active
- **Clean Shutdown**: Proper cleanup on service stop or client crash

### Position Data Merging

Following the Qt5 GeoClue plugin pattern:

```cpp
// GeoClue1 sends separate signals:
VelocityChanged(speed, direction, climb) → stored as "fresh"
PositionChanged(lat, lon, alt, accuracy) → merged with fresh velocity

// Result in GeoClue2 Location:
- Latitude, Longitude, Altitude (from Position)
- Accuracy (from Position)
- Speed, Heading (from Velocity if fresh)
- All combined in single Location object
```

### Regenerate DBus API

```bash
# Clone repository
cd geoclue2to1/src/generated

# Generate D-Bus glue code (already done, but if needed)
gdbus-codegen --interface-prefix org.freedesktop.GeoClue2. \
              --c-namespace GClue \
              --generate-c-code geoclue2-manager \
              geoclue/interface/org.freedesktop.GeoClue2.Manager.xml

gdbus-codegen --interface-prefix org.freedesktop.GeoClue2. \
              --c-namespace GClue \
              --generate-c-code geoclue2-client \
              geoclue/interface/org.freedesktop.GeoClue2.Client.xml

gdbus-codegen --interface-prefix org.freedesktop.GeoClue2. \
              --c-namespace GClue \
              --generate-c-code geoclue2-location \
              geoclue/interface/org.freedesktop.GeoClue2.Location.xml

# Fix to comply with the older GLib
sed -i 's/g_variant_builder_init_static/g_variant_builder_init/g' geoclue2-manager.c geoclue2-client.c geoclue2-location.c
```

## License

BSD License - see source files for details

## Credits

- Based on GeoClue2 upstream: https://gitlab.freedesktop.org/geoclue/geoclue
- Reference Qt5 plugin from Jolla/Sailfish OS
- Implements GeoClue2 D-Bus specification from freedesktop.org

### Known Limitations

- No authorization/agent support (all clients allowed)
- Client Distance/TimeThreshold properties not enforced (GeoClue1 handles timing)
- Description field always empty

