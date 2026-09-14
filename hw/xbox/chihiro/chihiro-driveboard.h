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

    /* Sega drive-board FFB state. Command byte = wire & 0x7F (e.g. wire 0x87
     * SPRING -> 0x07, wire 0x8B VIBRATION -> 0x0B, wire 0xFB playback -> 0x7B).
     * Mapping is from docs/chihiro-force-feedback.md, re-confirmed by register-
     * level disassembly of outrun2.xbe (db_dispatch_effects / db_dispatch_mode).
     * SPRING, damper and torque are PERSISTENT (held until changed); vibration
     * and the package pulse are transients. */
    bool    initializing;      /* from RESET (0xFF) to the first poll (0xFD) or start (0xFC): status 1 */
    bool    motor_active;      /* 0x80: 00 00 = off, else on */
    uint8_t global_power;      /* 0x83 P1: overall strength (0x40=80%..0x60=100%) */
    bool    spring_active;     /* 0x87 SPRING seen: wheel auto-centering engaged */
    uint8_t damper_level;      /* 0x88 P2: speed-dependent damper (0x08 slow/0x04 fast) */
    uint8_t friction_power;    /* 0x86 P2: constant torque / road-resistance FORCE
                                * (DAT_002fd0dd, base ~0x02, halved at idle). P1 is a
                                * fixed 0x2F range constant, NOT the force. */
    uint8_t road_power;        /* 0x8B P1: road/engine vibration power (0x20-0x78, rises with speed) */
    uint8_t road_freq;         /* 0x8B P2: vibration frequency (freq*2) */
    uint8_t vibration_power;   /* 0x85 P2: explicit vibration power (OR2 never sends 0x85) */
    uint8_t vibration_speed;   /* 0x85 P1: vibration speed */
    uint8_t movement_dir;      /* 0x84 P1: 1 = left, 0 = right (recenter/cancel) */
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
    uint8_t global_power;      /* game FFB strength (0x83), 0 = unset -> full */
    uint8_t centering_power;   /* SPRING (0x87): normalized auto-centering strength */
    uint8_t friction_power;    /* DAMPER (0x86 torque + 0x88 damper): resistance, pre-normalized (higher = stronger) */
    uint8_t movement_dir;      /* 0 = right, 1 = left */
    uint8_t movement_power;    /* CONSTANT (0x84): directional push */
    uint8_t vibration;         /* SINE continuous: road/engine buzz (0x8B, 0x85) */
    uint8_t event;             /* SINE transient: package jolt this frame (0xFB) */
} DriveBoardFFB;

void     driveboard_init(DriveBoardState *db);
void     driveboard_receive_byte(DriveBoardState *db, uint8_t byte);
bool     driveboard_has_response(DriveBoardState *db);
uint8_t  driveboard_get_response(DriveBoardState *db);
uint16_t driveboard_get_rumble(DriveBoardState *db);
void     driveboard_get_ffb(DriveBoardState *db, DriveBoardFFB *out);

extern DriveBoardState *chihiro_driveboard_global;

#endif
