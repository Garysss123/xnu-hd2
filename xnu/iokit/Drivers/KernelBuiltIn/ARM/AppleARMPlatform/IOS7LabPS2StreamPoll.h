/* Staged transport candidate: preserve stream packets and standard AUX_POLL
 * replies. ACK is recognized only while a command is pending, at a complete
 * packet boundary. No coordinate warp, fabricated button, or event injection. */
#ifndef IOS7LAB_PS2_STREAM_POLL_H
#define IOS7LAB_PS2_STREAM_POLL_H
#include "IOS7LabPS2.h"
struct IOS7LabPS2PollState {
    unsigned phase, ticks, commands, acknowledgements, failures;
    bool disabled;
};
enum { IOS7LAB_PS2_NO_PACKET=0, IOS7LAB_PS2_REAL_PACKET=1,
       IOS7LAB_PS2_COMMAND_ACK=2, IOS7LAB_PS2_IDLE_POLL=3 };
static bool ios7lab_ps2_poll_begin(IOS7LabPS2PollState *poll,
                                  const IOS7LabPS2State *decoder)
{
    if(poll->disabled || poll->phase || decoder->count) return false;
    poll->phase=1;poll->ticks=0;++poll->commands;return true;
}
static bool ios7lab_ps2_poll_tick(IOS7LabPS2PollState *poll)
{
    if(!poll->phase || poll->disabled) return false;
    if(++poll->ticks<8) return false;
    /* Stop issuing new commands; preserve an outstanding reply's framing so
     * a late ACK/data response remains correctly parsed. Never flush data. */
    poll->disabled=true;++poll->failures;return true;
}
static unsigned ios7lab_ps2_poll_byte(IOS7LabPS2PollState *poll,
    IOS7LabPS2State *decoder,uint8_t byte,uint32_t width,uint32_t height)
{
    if(poll->phase==1 && !decoder->count && byte==0xfa){
        poll->phase=2;++poll->acknowledgements;return IOS7LAB_PS2_COMMAND_ACK;
    }
    uint32_t oldButtons=decoder->buttons;
    if(!ios7lab_ps2_byte(decoder,byte,width,height))return IOS7LAB_PS2_NO_PACKET;
    bool polled=poll->phase==2;
    if(polled){poll->phase=0;poll->ticks=0;}
    if(polled && !decoder->bytes[1] && !decoder->bytes[2] &&
       decoder->buttons==oldButtons)return IOS7LAB_PS2_IDLE_POLL;
    return IOS7LAB_PS2_REAL_PACKET;
}
#endif
