/*
 * rfoff.c - request check and answer of the radio switch-off (rfoff.h).
 */
#include "rfoff.h"

#include "sbp.h"

uint8_t rfoff_parse(const uint8_t *p, size_t len, bool from_net, rfoff_req_t *r)
{
    if (len < 4 || sbp_get_u32(p) != SBP_KEY_CONFIRM)
        return SBP_RESP_ERR_KEY;
    if (len != 7 || p[4] > 1)
        return SBP_RESP_ERR_PAYLOAD;
    r->on = p[4] == 1;
    r->secs = r->on ? 0 : sbp_get_u16(p + 5);
    if (!r->on && from_net && (r->secs == 0 || r->secs > RFOFF_NET_MAX_S))
        return SBP_RESP_ERR_PAYLOAD;
    return SBP_RESP_OK;
}

void rfoff_state(uint8_t out[RFOFF_STATE_LEN], bool on, uint16_t left)
{
    out[0] = on ? 1 : 0;
    sbp_put_u16(out + 1, on ? 0 : left);
}
