#ifndef VR_UPDATER_H
#define VR_UPDATER_H

/* In-app updater: checks the latest GitHub release, downloads its VPK,
 * unpacks it and hands over to the Vita Radio Updater title (VRADUPDTR),
 * which installs it and relaunches Vita Radio. */

typedef enum {
    UPD_IDLE,
    UPD_CHECKING,
    UPD_UP_TO_DATE,
    UPD_AVAILABLE,
    UPD_DOWNLOADING,
    UPD_INSTALLING,
    UPD_READY,        /* package staged; main loop must call updater_launch() */
    UPD_ERROR
} UpdateState;

typedef struct {
    UpdateState state;
    char        latest[32];     /* tag of the latest release, once known */
    char        message[160];   /* one line for the UI */
} UpdateStatus;

/* Also picks up the result left by the updater title after an install. */
void updater_init(const char *user_agent, const char *ca_file);
/* Starts a background check. Ignored while a check or install is running. */
void updater_check(void);
/* Starts download + staging. Only acts in UPD_AVAILABLE. */
void updater_install(void);
int  updater_busy(void);
void updater_get_status(UpdateStatus *out);
/* Launches the updater title and exits the process. Call after shutdown. */
void updater_launch(void);
/* Cancels a running download/check and joins the worker. */
void updater_shutdown(void);

#endif
