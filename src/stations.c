#include "stations.h"

/* Phase 1 test stations: one per transport/codec combination we need to prove. */
const BuiltinStation g_builtin_stations[] = {
    { "Latina Reggaeton",       "http://latinareggaeton.ice.infomaniak.ch/latinareggaeton.mp3", "MP3 / http" },
    { "Gold (UK)",              "https://media-ssl.musicradio.com/GoldMP3",                     "MP3 / https" },
    { "SomaFM Indie Pop Rocks", "https://ice2.somafm.com/indiepop-128-aac",                     "AAC / https" },
    { "France Info",            "http://icecast.radiofrance.fr/franceinfo-hifi.aac",            "AAC / http, no ICY" },
    { "R101",                   "http://icecast.unitedradio.it/r101",                           "HE-AAC / http" },
    { "TRT FM",                 "https://trt.radyotvonline.net/trtfm",                          "HE-AAC / https TLS1.2" },
    { "RMC (HLS)",              "https://hls-rmc.nextradiotv.com/no_ssai/128k/media.m3u8",      "HLS - expect 'not supported yet'" },
};

const int g_builtin_station_count =
    (int)(sizeof(g_builtin_stations) / sizeof(g_builtin_stations[0]));
