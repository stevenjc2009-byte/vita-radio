#ifndef VR_STATIONS_H
#define VR_STATIONS_H

typedef struct {
    const char *name;
    const char *url;
    const char *kind;   /* short label shown in the UI */
} BuiltinStation;

extern const BuiltinStation g_builtin_stations[];
extern const int g_builtin_station_count;

#endif
