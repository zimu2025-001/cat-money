#pragma once

#include <stddef.h>
#include <stdint.h>

/* ZMC2 matches esp32-proxy.py's !4sIIIHHH wire format. Keep the entire UDP
 * datagram below a normal Ethernet MTU, including the 22-byte header. */
#define CAM_UDP_HEADER_BYTES 22U
#define CAM_UDP_DATAGRAM_BYTES 1400U
#define CAM_UDP_PAYLOAD_BYTES (CAM_UDP_DATAGRAM_BYTES - CAM_UDP_HEADER_BYTES)
#define CAM_UDP_MAX_FRAME_BYTES (1024U * 1024U)

uint32_t cam_udp_crc32(const uint8_t *data, size_t len);
uint16_t cam_udp_chunk_count(size_t frame_len);

/* Return the encoded length, or zero for invalid inputs. No allocation. */
size_t cam_udp_encode_chunk(uint8_t *packet, size_t capacity,
                            const uint8_t *frame, size_t frame_len,
                            uint32_t frame_id, uint32_t frame_crc,
                            uint16_t chunk_index);
