#ifndef WONDER_TRADE_RECEIVE_GUARD_H
#define WONDER_TRADE_RECEIVE_GUARD_H

#include "wonder_trade/wire.h"

/* Absent succeeds only after the complete inventory can be read. */
static int WtReceive_CheckReturn(unsigned char deliveryKind, int absentResult)
{
    return deliveryKind == WT_DELIVERY_RETURN && absentResult != 0 ? -1 : 0;
}

#endif
