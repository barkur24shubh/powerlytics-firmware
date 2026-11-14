#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

#if defined(__has_include)
#  if __has_include("board_active_config.h")
#    include "board_active_config.h"
#  endif
#endif

#define POWERLYTICS_BOARD_B1_BETA_C  1U
#define POWERLYTICS_BOARD_B1_V1_C    101U
#define POWERLYTICS_BOARD_B1_V2_C    102U
#define POWERLYTICS_BOARD_B1_V3_C    103U
#define POWERLYTICS_BOARD_B1_V4_C    104U
#define POWERLYTICS_BOARD_B1_V1_E    201U
#define POWERLYTICS_BOARD_B1_V2_E    202U
#define POWERLYTICS_BOARD_B1_V3_E    203U
#define POWERLYTICS_BOARD_B1_V4_E    204U

/* Backward-compatible name used by the first board-profile refactor. */
#define POWERLYTICS_BOARD_POWERLYTICS_CELLULAR_V1 POWERLYTICS_BOARD_B1_BETA_C

#ifndef POWERLYTICS_ACTIVE_BOARD
#define POWERLYTICS_ACTIVE_BOARD POWERLYTICS_BOARD_B1_BETA_C
#endif

#if POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_BETA_C
#include "../../Board/powerlytics_b1_beta_c/board_profile.h"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V1_C
#error "POWERLYTICS_BOARD_B1_V1_C profile is not implemented yet"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V2_C
#error "POWERLYTICS_BOARD_B1_V2_C profile is not implemented yet"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V3_C
#error "POWERLYTICS_BOARD_B1_V3_C profile is not implemented yet"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V4_C
#include "../../Board/powerlytics_b1_v4_c/board_profile.h"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V1_E
#error "POWERLYTICS_BOARD_B1_V1_E profile is not implemented yet"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V2_E
#error "POWERLYTICS_BOARD_B1_V2_E profile is not implemented yet"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V3_E
#error "POWERLYTICS_BOARD_B1_V3_E profile is not implemented yet"
#elif POWERLYTICS_ACTIVE_BOARD == POWERLYTICS_BOARD_B1_V4_E
#error "POWERLYTICS_BOARD_B1_V4_E profile is not implemented yet"
#else
#error "Unsupported POWERLYTICS_ACTIVE_BOARD"
#endif

#endif /* BOARD_PROFILE_H */
