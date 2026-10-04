/* iOS7 ARM32 event-queue wire format, checked against its native decoder.
 * Unlike the newer Mac header, this 28-byte prefix has no creation timestamp. */
#ifndef IOS7LAB_HID_WIRE_H
#define IOS7LAB_HID_WIRE_H
#include <stdint.h>
#include "IOS7LabPS2.h"
struct __attribute__((packed)) IOS7LabHIDHeader {
    uint64_t timeStamp, senderID;
    uint32_t options, attributeLength, eventCount;
};
struct IOS7LabDigitizer {
    uint32_t size, type, options;
    uint8_t depth, reserved[3];
    int32_t x,y,z;
    uint32_t index, transducerType, identity, eventMask, childEventMask, buttons;
    int32_t pressure, auxiliaryPressure, twist;
    uint32_t orientationType;
    int32_t quality, density, irregularity, majorRadius, minorRadius;
};
struct __attribute__((packed)) IOS7LabHIDPacket {
    IOS7LabHIDHeader header;
    IOS7LabDigitizer hand, finger;
};
typedef char IOS7LabHIDHeaderMustBe28[sizeof(IOS7LabHIDHeader)==28?1:-1];
typedef char IOS7LabHIDDigitizerMustBe88[sizeof(IOS7LabDigitizer)==88?1:-1];
typedef char IOS7LabHIDPacketMustBe204[sizeof(IOS7LabHIDPacket)==204?1:-1];
typedef char IOS7LabHIDCountOffset[__builtin_offsetof(IOS7LabHIDHeader,eventCount)==24?1:-1];

static bool ios7lab_make_hid_packet(const IOS7LabPS2State *state, bool previousDown,
    uint64_t timestamp, uint64_t sender, IOS7LabHIDPacket *packet)
{
    if (state->x>=640 || state->y>=960 || !timestamp || !sender) return false;
    unsigned down=state->buttons&1;
    uint32_t options=1|16|0x80000|(down?0x30000:0);
    uint32_t mask=4|(down!=(unsigned)previousDown?3:0);
    /* Native IOHIDEventService marks a newly entering contact's identity. */
    if(down && !previousDown)mask|=1U<<5;
    IOS7LabHIDPacket zero={};
    *packet=zero;
    packet->header.timeStamp=timestamp;
    packet->header.senderID=sender;
    packet->header.options=options|2;
    packet->header.eventCount=2;
    IOS7LabDigitizer hand={};
    hand.size=sizeof(hand);hand.type=11;hand.options=options|2;
    hand.x=(int32_t)(state->x*65536U/639U);
    hand.y=(int32_t)(state->y*65536U/959U);
    hand.transducerType=3;hand.eventMask=mask;hand.childEventMask=mask;
    packet->hand=hand;
    IOS7LabDigitizer finger=hand;
    finger.options=options;finger.depth=1;finger.index=1;finger.identity=1;
    finger.transducerType=2;finger.childEventMask=0;
    finger.pressure=down?65536:0;finger.orientationType=2;
    finger.quality=finger.density=finger.irregularity=65536;
    finger.majorRadius=finger.minorRadius=5*65536;
    packet->finger=finger;
    return true;
}
#endif
