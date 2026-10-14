#ifndef POKEPLATINUM_PORT_ENYO_TTS_H
#define POKEPLATINUM_PORT_ENYO_TTS_H

#include "render_text.h"

#ifdef __cplusplus
extern "C" {
#endif

void EnyoTTS_OnPrinterStart(TextPrinter *printer);
void EnyoTTS_OnPageAdvance(TextPrinter *printer);

#ifdef __cplusplus
}
#endif

#endif // POKEPLATINUM_PORT_ENYO_TTS_H
