#include "usb_midi_packet.h"

/* USB Device Class Definition for MIDI Devices — Table 4-1 (Code Index Number) */
enum {
    CIN_SYSEX_3 = 0x4,
    CIN_SYSEX_2 = 0x5,
    CIN_SYSEX_1 = 0x6,
    CIN_SYSEX_END_3 = 0x7,
    CIN_SYSEX_END_2 = 0x8,
    CIN_SYSEX_END_1 = 0x9,
};

static int write_pkt(uint8_t **wp, size_t *remain, uint8_t hdr, uint8_t d0, uint8_t d1, uint8_t d2)
{
    if (*remain < 4) {
        return -1;
    }
    uint8_t *p = *wp;
    p[0] = hdr;
    p[1] = d0;
    p[2] = d1;
    p[3] = d2;
    *wp += 4;
    *remain -= 4;
    return 0;
}

int usb_midi_pack_sysex(uint8_t cable, const uint8_t *sysex, size_t sysex_len,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!sysex || !out || !out_len || sysex_len < 2) {
        return -1;
    }
    if (sysex[0] != 0xF0 || sysex[sysex_len - 1] != 0xF7) {
        return -1;
    }

    const size_t max_needed = sysex_len * 4;
    if (out_cap < max_needed) {
        return -1;
    }

    uint8_t *wp = out;
    size_t room = out_cap;
    size_t i = 0;
    const uint8_t cn = (uint8_t)((cable & 0x0F) << 4);

    while (i < sysex_len) {
        const size_t rem = sysex_len - i;

        if (rem >= 4) {
            /* Last 4 bytes are ... XX F7 — use END_3 on the final 3-byte chunk */
            if (sysex[i + 3] == 0xF7) {
                if (write_pkt(&wp, &room, (uint8_t)(cn | CIN_SYSEX_END_3), sysex[i], sysex[i + 1], sysex[i + 2]) != 0) {
                    return -1;
                }
                i += 4;
                break;
            }
            if (write_pkt(&wp, &room, (uint8_t)(cn | CIN_SYSEX_3), sysex[i], sysex[i + 1], sysex[i + 2]) != 0) {
                return -1;
            }
            i += 3;
            continue;
        }

        if (rem == 3) {
            uint8_t cin = (sysex[i + 2] == 0xF7) ? (uint8_t)(cn | CIN_SYSEX_END_3) : (uint8_t)(cn | CIN_SYSEX_3);
            if (write_pkt(&wp, &room, cin, sysex[i], sysex[i + 1], sysex[i + 2]) != 0) {
                return -1;
            }
            i += 3;
            break;
        }

        if (rem == 2) {
            uint8_t cin = (sysex[i + 1] == 0xF7) ? (uint8_t)(cn | CIN_SYSEX_END_2) : (uint8_t)(cn | CIN_SYSEX_2);
            if (write_pkt(&wp, &room, cin, sysex[i], sysex[i + 1], 0) != 0) {
                return -1;
            }
            i += 2;
            break;
        }

        uint8_t cin = (sysex[i] == 0xF7) ? (uint8_t)(cn | CIN_SYSEX_END_1) : (uint8_t)(cn | CIN_SYSEX_1);
        if (write_pkt(&wp, &room, cin, sysex[i], 0, 0) != 0) {
            return -1;
        }
        i += 1;
        break;
    }

    if (i != sysex_len) {
        return -1;
    }

    *out_len = (size_t)(wp - out);
    if ((*out_len % 4) != 0) {
        return -1;
    }
    return 0;
}

int usb_midi_pack_program_change(uint8_t cable, uint8_t midi_channel, uint8_t program, uint8_t out[4])
{
    if (!out) {
        return -1;
    }
    const uint8_t cn = (uint8_t)((cable & 0x0F) << 4);
    out[0] = (uint8_t)(cn | 0x0C);
    out[1] = (uint8_t)(0xC0 | (midi_channel & 0x0F));
    out[2] = (uint8_t)(program & 0x7F);
    out[3] = 0;
    return 0;
}
