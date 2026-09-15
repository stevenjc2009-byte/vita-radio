#ifndef VR_PLAYER_H
#define VR_PLAYER_H

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_CONNECTING,   /* request sent, no headers yet */
    PLAYER_BUFFERING,    /* receiving, filling the pre-buffer (also after an underrun) */
    PLAYER_PLAYING,
    PLAYER_ERROR         /* see PlayerStatus.error */
} PlayerState;

typedef struct {
    PlayerState   state;
    char          url[512];
    char          title[256];     /* ICY StreamTitle, "" if none */
    char          codec[32];      /* decoder_profile_name(), "" until known */
    char          content_type[64];
    int           in_rate;        /* source sample rate after decode, 0 until known */
    int           in_channels;
    long          http_status;
    unsigned      buffer_pct;     /* ring buffer fill 0..100 */
    unsigned long bytes_received;
    char          error[256];
} PlayerStatus;

/* Owns one HttpStream + one decode/audio thread. All calls come from the UI thread. */
int  player_init(const char *user_agent, const char *ca_file);  /* 0 ok */
void player_play(const char *url);     /* stops whatever is playing, then starts url */
void player_stop(void);                /* back to IDLE */
void player_get_status(PlayerStatus *out);  /* thread-safe snapshot */
void player_shutdown(void);

#endif
