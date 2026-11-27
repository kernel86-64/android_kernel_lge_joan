/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __Q6VOICE_H__
#define __Q6VOICE_H__

#include <linux/types.h>

enum q6voice_session_id {
	Q6VOICE_SESSION_MMODE1 = 0,
	Q6VOICE_SESSION_MMODE2,
	Q6VOICE_SESSION_VOICE,
	Q6VOICE_SESSION_MAX,
};

enum q6voice_rx_port_id {
	Q6VOICE_RX_SLIMBUS_0 = 0,
	Q6VOICE_RX_TERT_MI2S,
	Q6VOICE_RX_QUAT_MI2S,
	Q6VOICE_RX_PORT_MAX,
};

#if IS_REACHABLE(CONFIG_SND_SOC_QDSP6_VOICE)
bool q6voice_get_state(void);
int q6voice_set_state(bool enable);
int q6voice_get_session(void);
int q6voice_set_session(int session);
int q6voice_get_rx_port_idx(void);
int q6voice_set_rx_port_idx(int idx);
#else
static inline bool q6voice_get_state(void) { return false; }
static inline int q6voice_set_state(bool enable) { return -ENODEV; }
static inline int q6voice_get_session(void) { return Q6VOICE_SESSION_MMODE1; }
static inline int q6voice_set_session(int session) { return -ENODEV; }
static inline int q6voice_get_rx_port_idx(void) { return Q6VOICE_RX_SLIMBUS_0; }
static inline int q6voice_set_rx_port_idx(int idx) { return -ENODEV; }
#endif

#endif /* __Q6VOICE_H__ */
