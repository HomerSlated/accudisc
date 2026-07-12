#include <string.h>

#include <accudisc/accudisc.h>

#include "cdb.h"

void adsc_cdb_inquiry(uint8_t cdb[6], uint8_t alloc)
{
    memset(cdb, 0, 6);
    cdb[0] = ADSC_OP_INQUIRY;
    cdb[4] = alloc;
}

void adsc_cdb_start_stop(uint8_t cdb[6], unsigned start, unsigned loej)
{
    memset(cdb, 0, 6);
    cdb[0] = ADSC_OP_START_STOP;
    cdb[4] = (uint8_t)(((loej & 1) << 1) | (start & 1));
}

void adsc_cdb_read_toc(uint8_t cdb[10], unsigned format, unsigned time_bit,
                       unsigned track, uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_READ_TOC;
    cdb[1] = (uint8_t)((time_bit & 1) << 1);
    cdb[2] = (uint8_t)(format & 0x0f);
    cdb[6] = (uint8_t)track;
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_read_disc_info(uint8_t cdb[10], uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_READ_DISC_INFO;
    cdb[1] = 0x00; /* data type: standard disc information */
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_get_configuration(uint8_t cdb[10], unsigned rt,
                                uint16_t feature, uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_GET_CONFIG;
    cdb[1] = (uint8_t)(rt & 0x03);
    cdb[2] = (uint8_t)(feature >> 8);
    cdb[3] = (uint8_t)(feature & 0xff);
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_mode_sense10(uint8_t cdb[10], unsigned page, uint16_t alloc)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_MODE_SENSE10;
    cdb[2] = (uint8_t)(page & 0x3f);
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)(alloc & 0xff);
}

void adsc_cdb_mode_select10(uint8_t cdb[10], uint16_t param_len)
{
    memset(cdb, 0, 10);
    cdb[0] = ADSC_OP_MODE_SELECT10;
    cdb[1] = 0x10; /* PF = SPC-format pages */
    cdb[7] = (uint8_t)(param_len >> 8);
    cdb[8] = (uint8_t)(param_len & 0xff);
}

void adsc_cdb_read_cd(uint8_t cdb[12], uint32_t lba, uint32_t nsec,
                      unsigned sector_type, unsigned c2, unsigned sub)
{
    memset(cdb, 0, 12);
    cdb[0] = ADSC_OP_READ_CD;
    cdb[1] = (uint8_t)((sector_type & 0x07) << 2);
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[6] = (uint8_t)(nsec >> 16);
    cdb[7] = (uint8_t)(nsec >> 8);
    cdb[8] = (uint8_t)nsec;
    cdb[9] = (uint8_t)(0x10 | ((c2 & 0x03) << 1)); /* user data | error flags */
    cdb[10] = (uint8_t)(sub & 0x07);
}

uint32_t adsc_read_cd_sector_len(unsigned c2, unsigned sub)
{
    uint32_t len = ACCUDISC_BYTES_AUDIO;

    if (c2 == ADSC_C2_294)
        len += ACCUDISC_BYTES_C2;
    else if (c2 == ADSC_C2_296)
        len += ACCUDISC_BYTES_C2_BEB;
    if (sub == ADSC_SUB_RAW)
        len += ACCUDISC_BYTES_SUB_RAW;
    else if (sub == ADSC_SUB_Q)
        len += ACCUDISC_BYTES_SUB_Q;
    return len;
}
