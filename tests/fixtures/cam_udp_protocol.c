#include "cam_udp_protocol.h"

#include <string.h>

static void write_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

uint32_t cam_udp_crc32(const uint8_t *data, size_t len)
{
    /* CRC-32/ISO-HDLC, the same initial/final XOR as Python zlib.crc32.
     * A small nibble table avoids both per-bit work and a large lookup table. */
    static const uint32_t table[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
    };
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 0x0fU];
        crc = (crc >> 4) ^ table[crc & 0x0fU];
    }
    return crc ^ UINT32_MAX;
}

uint16_t cam_udp_chunk_count(size_t frame_len)
{
    if (frame_len == 0 || frame_len > CAM_UDP_MAX_FRAME_BYTES) {
        return 0;
    }
    return (uint16_t)((frame_len + CAM_UDP_PAYLOAD_BYTES - 1U) / CAM_UDP_PAYLOAD_BYTES);
}

size_t cam_udp_encode_chunk(uint8_t *packet, size_t capacity,
                            const uint8_t *frame, size_t frame_len,
                            uint32_t frame_id, uint32_t frame_crc,
                            uint16_t chunk_index)
{
    uint16_t chunk_count = cam_udp_chunk_count(frame_len);
    if (packet == NULL || frame == NULL || chunk_index >= chunk_count) {
        return 0;
    }

    size_t offset = (size_t)chunk_index * CAM_UDP_PAYLOAD_BYTES;
    size_t payload_len = frame_len - offset;
    if (payload_len > CAM_UDP_PAYLOAD_BYTES) {
        payload_len = CAM_UDP_PAYLOAD_BYTES;
    }
    size_t packet_len = CAM_UDP_HEADER_BYTES + payload_len;
    if (capacity < packet_len) {
        return 0;
    }

    memcpy(packet, "ZMC2", 4);
    write_u32_be(packet + 4, frame_id);
    write_u32_be(packet + 8, (uint32_t)frame_len);
    write_u32_be(packet + 12, frame_crc);
    write_u16_be(packet + 16, chunk_index);
    write_u16_be(packet + 18, chunk_count);
    write_u16_be(packet + 20, (uint16_t)payload_len);
    memcpy(packet + CAM_UDP_HEADER_BYTES, frame + offset, payload_len);
    return packet_len;
}
