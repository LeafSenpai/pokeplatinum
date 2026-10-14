/*
 * Enyo TTS bridge for cybervisi0n/pokeplatinum pc_port.
 *
 * Sends visible message-box pages to the user's existing Kokoro server using
 * the lightweight ENYO_TTS UDP protocol.
 */

#include "enyo_tts.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants/charcode.h"

#include "charcode.h"
#include "font.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET EnyoSocket;
typedef int EnyoSockLen;
#define ENYO_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int EnyoSocket;
typedef socklen_t EnyoSockLen;
#define ENYO_INVALID_SOCKET (-1)
#endif

#define ENYO_TTS_TEXT_MAX   4096
#define ENYO_TTS_PACKET_MAX 8192

typedef struct EnyoTTSConfig {
    int loaded;
    int enabled;
    int port;
    float speed;
    int interrupt;
    int allText;
    int debug;
    char host[64];
} EnyoTTSConfig;

static EnyoTTSConfig sConfig;
static EnyoSocket sSocket = ENYO_INVALID_SOCKET;
static struct sockaddr_in sServerAddr;

static int EnyoTTS_ParseBool(const char *value, int fallback)
{
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    if (value[0] == '0'
        || value[0] == 'n' || value[0] == 'N'
        || value[0] == 'f' || value[0] == 'F') {
        return 0;
    }

    return 1;
}

static void EnyoTTS_LoadConfig(void)
{
    if (sConfig.loaded) {
        return;
    }

    memset(&sConfig, 0, sizeof(sConfig));
    sConfig.loaded = 1;
    sConfig.enabled = EnyoTTS_ParseBool(getenv("ENYO_POKE_TTS"), 1);
    sConfig.port = 6050;
    sConfig.speed = 1.0f;
    sConfig.interrupt = EnyoTTS_ParseBool(getenv("ENYO_POKE_TTS_INTERRUPT"), 1);
    sConfig.allText = EnyoTTS_ParseBool(getenv("ENYO_POKE_TTS_ALL_TEXT"), 0);
    sConfig.debug = EnyoTTS_ParseBool(getenv("ENYO_POKE_TTS_DEBUG"), 0);
    strcpy(sConfig.host, "127.0.0.1");

    const char *host = getenv("ENYO_POKE_TTS_HOST");
    if (host != NULL && host[0] != '\0') {
        strncpy(sConfig.host, host, sizeof(sConfig.host) - 1);
        sConfig.host[sizeof(sConfig.host) - 1] = '\0';
    }

    const char *port = getenv("ENYO_POKE_TTS_PORT");
    if (port != NULL && port[0] != '\0') {
        int parsed = atoi(port);
        if (parsed > 0 && parsed <= 65535) {
            sConfig.port = parsed;
        }
    }

    const char *speed = getenv("ENYO_POKE_TTS_SPEED");
    if (speed != NULL && speed[0] != '\0') {
        float parsed = (float)atof(speed);
        if (parsed >= 0.25f && parsed <= 4.0f) {
            sConfig.speed = parsed;
        }
    }

    if (sConfig.debug) {
        fprintf(stderr,
            "[EnyoPokeTTS] enabled=%d host=%s port=%d speed=%.2f interrupt=%d all_text=%d\n",
            sConfig.enabled,
            sConfig.host,
            sConfig.port,
            (double)sConfig.speed,
            sConfig.interrupt,
            sConfig.allText);
    }
}

static int EnyoTTS_InitSocket(void)
{
    EnyoTTS_LoadConfig();

    if (!sConfig.enabled) {
        return 0;
    }

    if (sSocket != ENYO_INVALID_SOCKET) {
        return 1;
    }

#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        if (sConfig.debug) {
            fprintf(stderr, "[EnyoPokeTTS] WSAStartup failed.\n");
        }
        return 0;
    }
#endif

    sSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSocket == ENYO_INVALID_SOCKET) {
        if (sConfig.debug) {
            fprintf(stderr, "[EnyoPokeTTS] UDP socket creation failed.\n");
        }
        return 0;
    }

    memset(&sServerAddr, 0, sizeof(sServerAddr));
    sServerAddr.sin_family = AF_INET;
    sServerAddr.sin_port = htons((unsigned short)sConfig.port);

    if (inet_pton(AF_INET, sConfig.host, &sServerAddr.sin_addr) != 1) {
        if (sConfig.debug) {
            fprintf(stderr, "[EnyoPokeTTS] Invalid IPv4 address: %s\n", sConfig.host);
        }
        return 0;
    }

    return 1;
}

static void EnyoTTS_AppendByte(char *out, size_t outSize, size_t *pos, char c)
{
    if (*pos + 1 >= outSize) {
        return;
    }

    out[(*pos)++] = c;
    out[*pos] = '\0';
}

static void EnyoTTS_AppendSpace(char *out, size_t outSize, size_t *pos)
{
    if (*pos == 0 || out[*pos - 1] == ' ') {
        return;
    }

    EnyoTTS_AppendByte(out, outSize, pos, ' ');
}

static void EnyoTTS_AppendUtf8(char *out, size_t outSize, size_t *pos, unsigned int cp)
{
    if (cp <= 0x7F) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)cp);
    } else if (cp <= 0x7FF) {
        if (*pos + 2 >= outSize) {
            return;
        }
        out[(*pos)++] = (char)(0xC0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        out[*pos] = '\0';
    } else {
        if (*pos + 3 >= outSize) {
            return;
        }
        out[(*pos)++] = (char)(0xE0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        out[*pos] = '\0';
    }
}

static void EnyoTTS_AppendWord(char *out, size_t outSize, size_t *pos, const char *word)
{
    while (*word != '\0' && *pos + 1 < outSize) {
        EnyoTTS_AppendByte(out, outSize, pos, *word++);
    }
}

static void EnyoTTS_AppendCharCode(charcode_t c, char *out, size_t outSize, size_t *pos)
{
    if (c >= CHAR_0 && c <= CHAR_9) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)('0' + (c - CHAR_0)));
        return;
    }
    if (c >= CHAR_A && c <= CHAR_Z) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)('A' + (c - CHAR_A)));
        return;
    }
    if (c >= CHAR_a && c <= CHAR_z) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)('a' + (c - CHAR_a)));
        return;
    }

    if (c >= CHAR_WIDE_0 && c <= CHAR_WIDE_9) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)('0' + (c - CHAR_WIDE_0)));
        return;
    }
    if (c >= CHAR_WIDE_A && c <= CHAR_WIDE_Z) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)('A' + (c - CHAR_WIDE_A)));
        return;
    }
    if (c >= CHAR_WIDE_a && c <= CHAR_WIDE_z) {
        EnyoTTS_AppendByte(out, outSize, pos, (char)('a' + (c - CHAR_WIDE_a)));
        return;
    }

    /* Platinum 0x015F..0x019E maps to Latin-1 U+00C0..U+00FF. */
    if (c >= CHAR_A_GRAVE && c <= CHAR_y_dieresis) {
        EnyoTTS_AppendUtf8(out, outSize, pos, 0x00C0u + (unsigned int)(c - CHAR_A_GRAVE));
        return;
    }

    switch (c) {
    case CHAR_CR:
        EnyoTTS_AppendSpace(out, outSize, pos);
        break;

    case CHAR_SPACE:
    case CHAR_NUM_SPACE:
    case CHAR_SPACE_1px:
    case CHAR_SPACE_2px:
    case CHAR_SPACE_4px:
    case CHAR_SPACE_8px:
    case CHAR_SPACE_16px:
    case CHAR_WIDE_SPACE:
    case CHAR_WIDE_TRANSPARENT:
        EnyoTTS_AppendSpace(out, outSize, pos);
        break;

    case CHAR_EXCLAMATION:
    case CHAR_WIDE_EXCLAMATION:
        EnyoTTS_AppendByte(out, outSize, pos, '!');
        break;
    case CHAR_QUESTION:
    case CHAR_WIDE_QUESTION:
        EnyoTTS_AppendByte(out, outSize, pos, '?');
        break;
    case CHAR_COMMA:
    case CHAR_WIDE_COMMA:
    case CHAR_JP_COMMA:
        EnyoTTS_AppendByte(out, outSize, pos, ',');
        break;
    case CHAR_PERIOD:
    case CHAR_DOT:
    case CHAR_WIDE_PERIOD:
    case CHAR_JP_PERIOD:
    case CHAR_JP_DOT:
        EnyoTTS_AppendByte(out, outSize, pos, '.');
        break;
    case CHAR_ELLIPSIS:
    case CHAR_JP_ELLIPSIS:
        EnyoTTS_AppendWord(out, outSize, pos, "...");
        break;
    case CHAR_SLASH:
    case CHAR_WIDE_SLASH:
        EnyoTTS_AppendByte(out, outSize, pos, '/');
        break;
    case CHAR_SINGLE_QUOTE_OPEN:
    case CHAR_SINGLE_QUOTE_CLOSE:
    case CHAR_JP_SINGLE_QUOTE_OPEN:
    case CHAR_JP_SINGLE_QUOTE_CLOSE:
        EnyoTTS_AppendByte(out, outSize, pos, '\'');
        break;
    case CHAR_DOUBLE_QUOTE_OPEN:
    case CHAR_DOUBLE_QUOTE_CLOSE:
    case CHAR_DOUBLE_QUOTE_CLOSE_INVERTED:
    case CHAR_JP_DOUBLE_QUOTE_OPEN:
    case CHAR_JP_DOUBLE_QUOTE_CLOSE:
        EnyoTTS_AppendByte(out, outSize, pos, '"');
        break;
    case CHAR_PAREN_OPEN:
    case CHAR_JP_PAREN_OPEN:
        EnyoTTS_AppendByte(out, outSize, pos, '(');
        break;
    case CHAR_PAREN_CLOSE:
    case CHAR_JP_PAREN_CLOSE:
        EnyoTTS_AppendByte(out, outSize, pos, ')');
        break;
    case CHAR_PLUS:
    case CHAR_WIDE_PLUS:
        EnyoTTS_AppendByte(out, outSize, pos, '+');
        break;
    case CHAR_MINUS:
    case CHAR_WIDE_MINUS:
        EnyoTTS_AppendByte(out, outSize, pos, '-');
        break;
    case CHAR_ASTERISK:
    case CHAR_WIDE_MULTIPLY:
        EnyoTTS_AppendByte(out, outSize, pos, '*');
        break;
    case CHAR_HASH:
        EnyoTTS_AppendByte(out, outSize, pos, '#');
        break;
    case CHAR_EQUALS:
    case CHAR_WIDE_EQUALS:
        EnyoTTS_AppendByte(out, outSize, pos, '=');
        break;
    case CHAR_AMPERSAND:
    case CHAR_WIDE_AMPERSAND:
        EnyoTTS_AppendByte(out, outSize, pos, '&');
        break;
    case CHAR_TILDE:
    case CHAR_WIDE_TILDE:
        EnyoTTS_AppendByte(out, outSize, pos, '~');
        break;
    case CHAR_COLON:
    case CHAR_WIDE_COLON:
        EnyoTTS_AppendByte(out, outSize, pos, ':');
        break;
    case CHAR_SEMICOLON:
    case CHAR_WIDE_SEMICOLON:
        EnyoTTS_AppendByte(out, outSize, pos, ';');
        break;
    case CHAR_AT_SIGN:
    case CHAR_WIDE_AT_SIGN:
        EnyoTTS_AppendByte(out, outSize, pos, '@');
        break;
    case CHAR_PERCENT:
    case CHAR_WIDE_PERCENT:
        EnyoTTS_AppendByte(out, outSize, pos, '%');
        break;
    case CHAR_UNDERSCORE:
    case CHAR_WIDE_UNDERSCORE:
        EnyoTTS_AppendByte(out, outSize, pos, '_');
        break;

    case CHAR_POKEDOLLARS:
        EnyoTTS_AppendByte(out, outSize, pos, '$');
        break;

    case CHAR_MALE:
    case CHAR_WIDE_MALE:
        EnyoTTS_AppendSpace(out, outSize, pos);
        EnyoTTS_AppendWord(out, outSize, pos, "male");
        EnyoTTS_AppendSpace(out, outSize, pos);
        break;

    case CHAR_FEMALE:
    case CHAR_WIDE_FEMALE:
        EnyoTTS_AppendSpace(out, outSize, pos);
        EnyoTTS_AppendWord(out, outSize, pos, "female");
        EnyoTTS_AppendSpace(out, outSize, pos);
        break;

    case CHAR_OE:
        EnyoTTS_AppendUtf8(out, outSize, pos, 0x0152);
        break;
    case CHAR_oe:
        EnyoTTS_AppendUtf8(out, outSize, pos, 0x0153);
        break;

    default:
        /* Icons, arrows, Korean/Japanese glyphs and other non-spoken glyphs are omitted in v0.1. */
        break;
    }
}

static size_t EnyoTTS_ExtractVisiblePage(const charcode_t *src, char *out, size_t outSize)
{
    const charcode_t *p = src;
    size_t pos = 0;

    if (outSize == 0) {
        return 0;
    }
    out[0] = '\0';

    while (p != NULL && *p != CHAR_EOS) {
        charcode_t c = *p;

        /* These are the \r and \f page boundaries in the source text JSON. */
        if (c == CHAR_CONTROL_CLEAR || c == CHAR_CONTROL_SCROLL) {
            break;
        }

        if (c == CHAR_FORMAT_ARG) {
            p = CharCode_SkipFormatArg(p);
            continue;
        }

        if (c == CHAR_PLACEHOLDER_BEGIN) {
            p += 2;
            continue;
        }

        EnyoTTS_AppendCharCode(c, out, outSize, &pos);
        p++;
    }

    while (pos > 0 && isspace((unsigned char)out[pos - 1])) {
        out[--pos] = '\0';
    }

    return pos;
}

static int EnyoTTS_ShouldSpeak(const TextPrinter *printer)
{
    EnyoTTS_LoadConfig();

    if (!sConfig.enabled || printer == NULL) {
        return 0;
    }

    if (!sConfig.allText && printer->template.fontID != FONT_MESSAGE) {
        return 0;
    }

    return 1;
}

static void EnyoTTS_SendPage(const TextPrinter *printer)
{
    if (!EnyoTTS_ShouldSpeak(printer)) {
        return;
    }

    char spoken[ENYO_TTS_TEXT_MAX];
    size_t len = EnyoTTS_ExtractVisiblePage(printer->template.toPrint.raw, spoken, sizeof(spoken));

    if (len < 2) {
        return;
    }

    if (!EnyoTTS_InitSocket()) {
        return;
    }

    char packet[ENYO_TTS_PACKET_MAX];
    int packetLen = snprintf(
        packet,
        sizeof(packet),
        "ENYO_TTS\t2\t%.2f\t%d\t%s",
        (double)sConfig.speed,
        sConfig.interrupt ? 1 : 0,
        spoken);

    if (packetLen <= 0) {
        return;
    }

    if ((size_t)packetLen >= sizeof(packet)) {
        packetLen = (int)sizeof(packet) - 1;
        packet[packetLen] = '\0';
    }

    int sent = (int)sendto(
        sSocket,
        packet,
        packetLen,
        0,
        (const struct sockaddr *)&sServerAddr,
        (EnyoSockLen)sizeof(sServerAddr));

    if (sConfig.debug) {
        if (sent < 0) {
            fprintf(stderr, "[EnyoPokeTTS] UDP send failed.\n");
        } else {
            fprintf(stderr, "[EnyoPokeTTS] sent %lu chars: %.120s\n",
                (unsigned long)len, spoken);
        }
    }
}

void EnyoTTS_OnPrinterStart(TextPrinter *printer)
{
    EnyoTTS_SendPage(printer);
}

void EnyoTTS_OnPageAdvance(TextPrinter *printer)
{
    EnyoTTS_SendPage(printer);
}
