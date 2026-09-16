#include "stations.h"

/*
 * Built-in station list.
 *
 * Every entry below was fetched live with a ranged GET (curl -r 0-2047,
 * User-Agent "VitaRadio/2.0.0 (PS Vita)", follow redirects) and kept only if
 * it answered 200/206 with an audio or HLS playlist content-type.  HLS entries
 * were additionally confirmed to return a body containing #EXTM3U.
 *
 * The BBC entries all use the "uk" simulcast path; every one of them also
 * answers on the "nonuk" path, so no station here depends on a UK IP.
 */
const BuiltinStation g_builtin_stations[] = {
    /* --- BBC nationals and nations (HLS) --- */
    { "BBC Radio 1",            "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_one.m3u8",                    "HLS / AAC" },
    { "BBC Radio 1Xtra",        "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_1xtra.m3u8",                       "HLS / AAC" },
    { "BBC Radio 1 Dance",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_one_dance.m3u8",              "HLS / AAC" },
    { "BBC Radio 1 Anthems",    "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_one_anthems.m3u8",            "HLS / AAC" },
    { "BBC Radio 1 Relax",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_one_relax.m3u8",              "HLS / AAC" },
    { "BBC Radio 2",            "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_two.m3u8",                    "HLS / AAC" },
    { "BBC Radio 3",            "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_three.m3u8",                  "HLS / AAC" },
    { "BBC Radio 3 Unwind",     "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_three_unwind.m3u8",           "HLS / AAC" },
    { "BBC Radio 4 FM",         "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_fourfm.m3u8",                 "HLS / AAC" },
    { "BBC Radio 4 LW",         "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_fourlw.m3u8",                 "HLS / AAC" },
    { "BBC Radio 4 Extra",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_four_extra.m3u8",             "HLS / AAC" },
    { "BBC Radio 5 Live",       "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_five_live.m3u8",              "HLS / AAC" },
    { "BBC 5 Sports Extra",     "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_five_live_sports_extra.m3u8", "HLS / AAC" },
    { "BBC Radio 6 Music",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_6music.m3u8",                       "HLS / AAC" },
    { "BBC Asian Network",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_asian_network.m3u8",                "HLS / AAC" },
    { "BBC World Service",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service.m3u8",                "HLS / AAC" },
    { "BBC WS News",            "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service_news_internet.m3u8",  "HLS / AAC" },
    { "BBC WS Americas",        "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service_americas.m3u8",       "HLS / AAC" },
    { "BBC WS Europe",          "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service_europe.m3u8",         "HLS / AAC" },
    { "BBC WS East Asia",       "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service_east_asia.m3u8",      "HLS / AAC" },
    { "BBC WS South Asia",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service_south_asia.m3u8",     "HLS / AAC" },
    { "BBC WS Australasia",     "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_world_service_australasia.m3u8",    "HLS / AAC" },
    { "BBC Radio Scotland",     "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_scotland_fm.m3u8",            "HLS / AAC" },
    { "BBC Radio nan Gàidheal", "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_nan_gaidheal.m3u8",           "HLS / AAC" },
    { "BBC Radio Wales",        "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_wales_fm.m3u8",               "HLS / AAC" },
    { "BBC Radio Cymru",        "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_cymru.m3u8",                  "HLS / AAC" },
    { "BBC Radio Cymru 2",      "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_cymru_2.m3u8",                "HLS / AAC" },
    { "BBC Radio Ulster",       "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_ulster.m3u8",                 "HLS / AAC" },
    { "BBC Radio Foyle",        "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_radio_foyle.m3u8",                  "HLS / AAC" },
    { "BBC Radio London",       "http://a.files.bbci.co.uk/ms6/live/3441A116-B12E-4D2F-ACA8-C1984642FA4B/audio/simulcast/hls/uk/pc_hd_abr_v2/cf/bbc_london.m3u8",                       "HLS / AAC" },

    /* --- UK & Ireland --- */
    { "Classic FM",             "https://media-ssl.musicradio.com/ClassicFMMP3",                "MP3" },
    { "LBC",                    "https://media-ssl.musicradio.com/LBCUK",                       "AAC" },
    { "Heart UK",               "https://media-ssl.musicradio.com/HeartUK",                     "AAC" },
    { "Capital FM",             "https://media-ssl.musicradio.com/CapitalMP3",                  "MP3" },
    { "Radio X",                "https://media-ssl.musicradio.com/RadioXLondon",                "AAC" },
    { "talkSPORT",              "https://radio.talksport.com/stream",                           "AAC" },
    { "NTS Radio",              "https://stream-relay-geo.ntslive.net/stream",                  "MP3" },
    { "RTÉ Radio 1",            "https://icecast.rte.ie/radio1",                                "MP3" },

    /* --- News & talk --- */
    { "RFI Monde",              "https://rfimonde64k.ice.infomaniak.ch/rfimonde-64.mp3",        "MP3" },
    { "Radio Free Europe",      "https://rfe-ingest.akamaized.net/hls/live/2035254/axia04/playlist.m3u8", "HLS / AAC" },
    { "ABC News Radio",         "http://abc.streamguys1.com/live/newsradio/icecast.audio",      "HE-AAC" },
    { "CBC Radio 1 Toronto",    "https://cbcradiolive.akamaized.net/hls/live/2041036/ES_R1ETR/master.m3u8", "HLS / AAC" },

    /* --- Europe: pop & rock --- */
    { "1LIVE",                  "https://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3", "MP3" },
    { "Hitradio Ö3",            "https://orf-live.ors-shoutcast.at/oe3-q2a",                    "MP3" },
    { "FM4",                    "https://orf-live.ors-shoutcast.at/fm4-q1a",                    "MP3" },
    { "RTL France",             "https://live.m6radio.quortex.io/webM89Hc99XApzgfhXNX8ASN5/grouprtl/national/short/audio-64000/index.m3u8", "HLS / AAC" },
    { "RMC",                    "https://hls-rmc.nextradiotv.com/no_ssai/128k/media.m3u8",      "HLS / AAC" },

    /* --- Europe: public radio --- */
    { "France Inter",           "https://icecast.radiofrance.fr/franceinter-hifi.aac",          "AAC" },
    { "France Culture",         "https://icecast.radiofrance.fr/franceculture-hifi.aac",        "AAC" },
    { "Deutschlandfunk",        "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3",      "MP3" },
    { "RAI Radio 1",            "https://icestreaming.rai.it/1.mp3",                            "MP3" },

    /* --- Classical --- */
    { "Radio Swiss Classic",    "https://stream.srg-ssr.ch/m/rsc_de/mp3_128",                   "MP3" },
    { "France Musique",         "https://icecast.radiofrance.fr/francemusique-hifi.aac",        "AAC" },
    { "Venice Classic Radio",   "https://uk2.streamingpulse.com/ssl/vcr1",                      "MP3" },
    { "Yle Klassinen",          "https://icecast.live.yle.fi/radio/YleKlassinen/icecast.audio", "HE-AAC" },

    /* --- Jazz & blues --- */
    { "Radio Swiss Jazz",       "https://stream.srg-ssr.ch/m/rsj/mp3_128",                      "MP3" },
    { "TSF Jazz",               "https://tsfjazz.ice.infomaniak.ch/tsfjazz-high.mp3",           "MP3" },
    { "KCSM Jazz 91",           "https://ice5.securenetsystems.net/KCSM",                       "AAC" },
    { "Bossa Jazz Brasil",      "https://centova5.transmissaodigital.com:20104/live",           "HE-AAC" },

    /* --- US public & eclectic --- */
    { "WNYC FM",                "https://fm939.wnyc.org/wnycfm",                                "MP3" },
    { "WFMU",                   "https://stream0.wfmu.org/freeform-128k",                       "MP3" },
    { "KEXP Seattle",           "http://live-mp3-128.kexp.org/kexp128.mp3",                     "MP3" },
    { "WXPN Philadelphia",      "https://wxpnhi.xpn.org/xpnhi",                                 "MP3" },
    { "KCRW Eclectic24",        "https://streams.kcrw.com/e24_aac",                             "AAC" },

    /* --- Radio Paradise --- */
    { "Radio Paradise",         "https://stream.radioparadise.com/aac-128",                     "AAC" },
    { "Radio Paradise Mellow",  "https://stream.radioparadise.com/mellow-128",                  "AAC" },

    /* --- SomaFM --- */
    { "SomaFM Groove Salad",    "https://ice2.somafm.com/groovesalad-128-aac",                  "AAC" },
    { "SomaFM Drone Zone",      "https://ice1.somafm.com/dronezone-128-mp3",                    "MP3" },
    { "SomaFM Indie Pop Rocks", "https://ice2.somafm.com/indiepop-128-aac",                     "AAC" },
    { "SomaFM Lush",            "https://ice2.somafm.com/lush-128-aac",                         "AAC" },
    { "SomaFM Secret Agent",    "https://ice1.somafm.com/secretagent-128-mp3",                  "MP3" },
    { "SomaFM Underground 80s", "https://ice1.somafm.com/u80s-128-mp3",                         "MP3" },
    { "SomaFM Boot Liquor",     "https://ice1.somafm.com/bootliquor-128-mp3",                   "MP3" },
    { "SomaFM DEF CON Radio",   "https://ice2.somafm.com/defcon-128-aac",                       "AAC" },

    /* --- Electronic & dance --- */
    { "FIP",                    "https://icecast.radiofrance.fr/fip-hifi.aac",                  "AAC" },
    { "Ibiza Global Radio",     "http://ibizaglobalradio.streaming-pro.com:8024/",              "MP3" },
    { "Tomorrowland One World", "https://playerservices.streamtheworld.com/api/livestream-redirect/OWR_INTERNATIONAL_ADP.aac", "HE-AAC" },
    { "Radio Record",           "https://radiorecord.hostingradio.ru/rr_main96.aacp",           "HE-AAC" },

    /* --- Asia-Pacific --- */
    { "RTHK Radio 1",           "https://rthkaudio1-lh.akamaihd.net/i/radio1_1@355864/master.m3u8", "HLS / AAC" },
    { "RTHK Radio 2",           "https://rthkaudio2-lh.akamaihd.net/i/radio2_1@355865/master.m3u8", "HLS / AAC" },
    { "AIR Vividh Bharati",     "https://air.pc.cdn.bitgravity.com/air/live/pbaudio001/playlist.m3u8", "HLS / AAC" },
    { "CBS Music FM (KR)",      "https://m-aac.cbs.co.kr/cbs939/_definst_/cbs939.stream/playlist.m3u8", "HLS / AAC" },
    { "RNZ National",           "http://radionz-ice.streamguys.com/national",                   "AAC" },

    /* --- Latin America --- */
    { "La 100 (AR)",            "https://playerservices.streamtheworld.com/api/livestream-redirect/FM999_56.mp3", "MP3" },
    { "LOS40 México",           "http://15723.live.streamtheworld.com/LOS40_MEXICO_SC",         "MP3" },
    { "Antena 1 Brasil",        "http://antena1.newradio.it/stream",                            "HE-AAC" },

    /* --- Africa & Middle East --- */
    { "Jacaranda FM",           "https://edge.iono.fm/xice/jacarandafm_live_medium.aac",        "HE-AAC" },
    { "Kameme FM (KE)",         "https://kamemefm-atunwadigital.streamguys1.com/kamemefm",      "AAC" },
    { "Galgalatz",              "https://glzwizzlv.bynetcdn.com/glglz_mp3?awCollectionId=misc&awEpisodeId=glglz", "MP3" },
    { "TRT FM",                 "https://trt.radyotvonline.net/trtfm",                          "HE-AAC" },
};

const int g_builtin_station_count =
    (int)(sizeof(g_builtin_stations) / sizeof(g_builtin_stations[0]));
