
#ifndef NWAY_RST_H
#define NWAY_RST_H
#include "switch.h"

#define INV "INV "
#define BYE "BYE "
#define DATA "DATA"
#define ACK "ACK "

#define log_err(fmt, args...) switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, fmt,##args)
#define log_dbg(fmt, args...) switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, fmt,##args)
#define log_ntc(fmt, args...) switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, fmt,##args)

#endif
