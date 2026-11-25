#include "wonder_trade/receive_guard.h"

#include <assert.h>

int main(void)
{
    assert(WtReceive_CheckReturn(WT_DELIVERY_RETURN, 0) == 0);
    assert(WtReceive_CheckReturn(WT_DELIVERY_RETURN, -1) == -1);
    assert(WtReceive_CheckReturn(WT_DELIVERY_RETURN, 1) == -1);
    assert(WtReceive_CheckReturn(WT_DELIVERY_RETURN, -37) == -1);

    /* Other deliveries keep their existing validation and receive effects. */
    assert(WtReceive_CheckReturn(WT_DELIVERY_MATCH, 0) == 0);
    assert(WtReceive_CheckReturn(WT_DELIVERY_MATCH, -1) == 0);
    assert(WtReceive_CheckReturn(WT_DELIVERY_MATCH, 1) == 0);
    assert(WtReceive_CheckReturn(WT_DELIVERY_NONE, -1) == 0);
    return 0;
}
