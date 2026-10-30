/**
 * @file src/trackpad_packet.h
 * @brief The precision touchpad packet of the input stream (a Vibeshine
 *        protocol extension, sent by LiSendTrackpadEvent() of our
 *        moonlight-common-c): every finger on the client's trackpad in one
 *        frame, for the host to recognize scrolling and zooming itself.
 */
#pragma once

#include <cstdint>

#include <moonlight-common-c/src/Input.h>

#ifndef SS_TRACKPAD_MAGIC
  #define SS_TRACKPAD_MAGIC 0x55000008
  #define SS_TRACKPAD_MAX_CONTACTS 5

  #pragma pack(push, 1)

typedef struct _SS_TRACKPAD_CONTACT {
  uint32_t contactId;
  netfloat x;  ///< 0..1 across the touchpad
  netfloat y;  ///< 0..1 down the touchpad
} SS_TRACKPAD_CONTACT, *PSS_TRACKPAD_CONTACT;

typedef struct _SS_TRACKPAD_PACKET {
  NV_INPUT_HEADER header;
  uint8_t contactCount;
  uint8_t zero[3];
  netfloat width;  ///< the touchpad's physical size, in millimeters
  netfloat height;
  SS_TRACKPAD_CONTACT contacts[SS_TRACKPAD_MAX_CONTACTS];
  uint32_t timestamp;  ///< when the fingers were there, client ms (any base); absent from the first clients' packets
} SS_TRACKPAD_PACKET, *PSS_TRACKPAD_PACKET;

  #pragma pack(pop)

  /// The first clients' packets end before `timestamp`
  #define SS_TRACKPAD_PACKET_UNTIMED_SIZE (sizeof(SS_TRACKPAD_PACKET) - sizeof(uint32_t))
#endif
