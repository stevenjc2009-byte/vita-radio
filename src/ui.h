#ifndef VR_UI_H
#define VR_UI_H

#include "player.h"
#include "stations.h"
#include "updater.h"

void ui_init(void);
/* Draws one full frame (start drawing .. swap buffers). */
void ui_draw(const BuiltinStation *list, int count, int selected, const PlayerStatus *st,
             const UpdateStatus *upd);
void ui_shutdown(void);

#endif
