#ifndef IOS7_LEO_TOUCH_WIRE_H
#define IOS7_LEO_TOUCH_WIRE_H
#include <stdint.h>
/* The original iOS7 ARM32 IOHIDEventCreateWithBytes contract: 28-byte
 * queue header followed by an 88-byte hand and an 88-byte finger. */
struct __attribute__((packed)) LeoHIDHeader {
 uint64_t timeStamp,senderID;
 uint32_t options,attributeLength,eventCount;
};
struct LeoHIDDigitizer {
 uint32_t size,type,options;
 uint8_t depth,reserved[3];
 int32_t x,y,z;
 uint32_t index,transducerType,identity,eventMask,childEventMask,buttons;
 int32_t pressure,auxiliaryPressure,twist;
 uint32_t orientationType;
 int32_t quality,density,irregularity,majorRadius,minorRadius;
};
struct __attribute__((packed)) LeoHIDPacket {
 LeoHIDHeader header;
 LeoHIDDigitizer hand,finger;
};
typedef char LeoHIDHeader28[(sizeof(LeoHIDHeader)==28)?1:-1];
typedef char LeoHIDDigitizer88[(sizeof(LeoHIDDigitizer)==88)?1:-1];
typedef char LeoHIDPacket204[(sizeof(LeoHIDPacket)==204)?1:-1];
typedef char LeoHIDCount24[(__builtin_offsetof(LeoHIDHeader,eventCount)==24)?1:-1];
/* Only validated real portrait reports may reach this serializer. The caller
 * suppresses idle-up reports and commits previousDown only after enqueue.
 * No guessed display UUID, client identity or gesture metadata is inserted. */
static bool leo_touch_make_hid_packet(uint32_t x,uint32_t y,uint32_t down,
 bool previousDown,uint64_t timestamp,uint64_t sender,LeoHIDPacket *packet)
{
 if(!packet || x>=480U || y>=800U || down>1U || !timestamp || !sender)return false;
 if(!down && !previousDown)return false;
 uint32_t options=1U|16U|0x80000U|(down?0x30000U:0U);
 uint32_t mask=4U|(down!=(unsigned)previousDown?3U:0U);
 if(down && !previousDown)mask|=1U<<5;
 LeoHIDPacket zero={};*packet=zero;
 packet->header.timeStamp=timestamp;packet->header.senderID=sender;
 packet->header.options=options|2U;packet->header.eventCount=2;
 LeoHIDDigitizer hand={};hand.size=sizeof(hand);hand.type=11;
 hand.options=options|2U;
 hand.x=(int32_t)(x*65536U/479U);hand.y=(int32_t)(y*65536U/799U);
 hand.transducerType=3;hand.eventMask=hand.childEventMask=mask;
 packet->hand=hand;
 LeoHIDDigitizer finger=hand;finger.options=options;finger.depth=1;
 finger.index=1;finger.identity=1;finger.transducerType=2;finger.childEventMask=0;
 finger.pressure=down?65536:0;finger.orientationType=2;
 finger.quality=finger.density=finger.irregularity=65536;
 finger.majorRadius=finger.minorRadius=5*65536;
 packet->finger=finger;return true;
}
#endif
