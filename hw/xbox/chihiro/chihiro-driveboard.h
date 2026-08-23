#ifndef CHIHIRO_DRIVEBOARD_H
#define CHIHIRO_DRIVEBOARD_H

#include <stdint.h>
#include <stdbool.h>

#define DRIVEBOARD_RESP_SIZE 8

typedef struct DriveBoardState {
    uint8_t tx_buf[4];
    int     tx_pos;

    uint8_t resp_buf[DRIVEBOARD_RESP_SIZE];
    int     resp_head;
    int     resp_tail;

    /* Sega Universal Drive (SUD) effect state, per A.Geezer's protocol notes.
     * Centering and friction are PERSISTENT forces held until changed; the
     * playback pulse is a one-shot transient. (Command byte = wire & 0x7F, e.g.
     * wire 0x8B centering -> 0x0B, wire 0xFB playback -> 0x7B.) */
    bool    motor_active;
    uint8_t global_power;      /* 0x83 P1: overall strength (0x32=60%..0x60=100%) */
    uint8_t centering_power;   /* 0x8B P1: auto-centering strength (persistent) */
    uint8_t friction_power;    /* 0x86 P1: friction (persistent) */
    uint8_t vibration_power;   /* 0x85 P2: explicit vibration power (0-0x3F) */
    uint8_t vibration_speed;   /* 0x85 P1: vibration speed */
    uint8_t movement_dir;      /* 0x84 P1: 1 = left, 0 = right */
    uint8_t movement_power;    /* 0x84 P2: directional movement power */

    /* SUD effect packages (uploaded via 0x9D/0x9E): 16 packages x 16 movements,
     * each byte = (direction<<7) | power. Played on 0xFB. This is how OR2 gives
     * each event its own texture -- impacts play a max-power jolt package,
     * rough surfaces (sand/grass) play oscillating buzz packages of different
     * frequency. */
    uint8_t sud_pkg[16][16];
    uint8_t sud_up_pkg;   /* upload cursor, set by 0x9D */
    uint8_t sud_up_idx;
    int8_t  play_pkg;     /* package currently playing back, -1 = idle */
    uint8_t play_pos;     /* movement index within the package (0-15) */
    uint8_t play_sub;     /* frames spent on the current movement */

    /* Current transient magnitude (0-0x7F) produced by the active playback,
     * refreshed once per host frame by driveboard_get_ffb(). */
    uint8_t event_pulse;
} DriveBoardState;

/* Snapshot of the current force-feedback effect state, for the host output
 * layer (gamepad rumble or steering-wheel SDL_Haptic). All fields are 0 when
 * the motor is disabled. Persistent forces (centering/friction) drive a real
 * wheel; only the transient vibration channel is felt on a gamepad. */
typedef struct DriveBoardFFB {
    bool    active;            /* motor enabled */
    uint8_t global_power;      /* 0x83: game's FFB strength %, 0 = unset (full) */
    uint8_t centering_power;   /* wheel auto-centering spring */
    uint8_t friction_power;    /* wheel friction/damper */
    uint8_t movement_dir;      /* 0 = right, 1 = left */
    uint8_t movement_power;    /* wheel directional constant force */
    uint8_t vibration;         /* transient buzz (vibration + playback events) */
} DriveBoardFFB;

void     driveboard_init(DriveBoardState *db);
void     driveboard_receive_byte(DriveBoardState *db, uint8_t byte);
bool     driveboard_has_response(DriveBoardState *db);
uint8_t  driveboard_get_response(DriveBoardState *db);
uint16_t driveboard_get_rumble(DriveBoardState *db);
void     driveboard_get_ffb(DriveBoardState *db, DriveBoardFFB *out);

extern DriveBoardState *chihiro_driveboard_global;

#endif
