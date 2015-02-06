#include <linux/string.h>

#include "fm_typedef.h"
#include "fm_rds.h"
#include "fm_dbg.h"
#include "fm_err.h"
#include "fm_stdlib.h"

//static rds_ps_state_machine_t ps_state_machine = RDS_PS_START;
//static rds_rt_state_machine_t rt_state_machine = RDS_RT_START;
struct fm_state_machine {
    fm_s32 state;
    fm_s32(*state_get)(struct fm_state_machine *thiz);
    fm_s32(*state_set)(struct fm_state_machine *thiz, fm_s32 new_state);
};

static fm_s32 fm_state_get(struct fm_state_machine *thiz)
{
    return thiz->state;
}

static fm_s32 fm_state_set(struct fm_state_machine *thiz, fm_s32 new_state)
{
    return thiz->state = new_state;
}

#define STATE_SET(a, s)        \
{                           \
    if((a)->state_set){          \
        (a)->state_set((a), (s));    \
    }                       \
}

#define STATE_GET(a)         \
({                             \
    fm_s32 __ret = 0;              \
    if((a)->state_get){          \
        __ret = (a)->state_get((a));    \
    }                       \
    __ret;                  \
})

static fm_u16(*rds_get_freq)(void)  = NULL;

//RDS spec related handle flow
static fm_s32 rds_cnt_get(struct rds_rx_t *rds_raw, fm_s32 raw_size, fm_s32 *cnt)
{
    fm_s32 gap = sizeof(rds_raw->cos) + sizeof(rds_raw->sin);

    FMR_ASSERT(rds_raw);
    FMR_ASSERT(cnt);
    *cnt = (raw_size - gap) / sizeof(rds_packet_t);
    WCN_DBG(FM_INF | RDSC, "group cnt=%d\n", *cnt);

    return 0;
}

static fm_s32 rds_grp_get(fm_u16 *dst, struct rds_rx_t *raw, fm_s32 idx)
{
    FMR_ASSERT(dst);
    FMR_ASSERT(raw);

    if (idx > (MAX_RDS_RX_GROUP_CNT - 1)) {
        return -FM_EPARA;
    }

    dst[0] = raw->data[idx].blkA;
    dst[1] = raw->data[idx].blkB;
    dst[2] = raw->data[idx].blkC;
    dst[3] = raw->data[idx].blkD;
    dst[4] = raw->data[idx].crc;
    dst[5] = raw->data[idx].cbc;

    WCN_DBG(FM_INF | RDSC, "BLOCK:%04x %04x %04x %04x, CRC:%04x\n", dst[0], dst[1], dst[2], dst[3], dst[4]);

    return 0;
}

static fm_s32 rds_checksum_check(fm_u16 crc, fm_s32 mask, fm_bool *valid)
{
    FMR_ASSERT(valid);

    if ((crc & mask) == mask) {
        *valid = fm_true;
    } else {
        *valid = fm_false;
    }

    return 0;
}

static fm_s32 rds_cbc_get(fm_u16 cbc, enum rds_blk_t blk)
{
    int ret = 0;

    switch (blk) {
    case RDS_BLK_A:
        ret = (cbc & 0xF000) >> 12;
        break;
    case RDS_BLK_B:
        ret = (cbc & 0x0F00) >> 8;
        break;
    case RDS_BLK_C:
        ret = (cbc & 0x00F0) >> 4;
        break;
    case RDS_BLK_D:
        ret = (cbc & 0x000F) >> 0;
        break;
    default:
        break;
    }

    WCN_DBG(FM_INF | RDSC, "group cbc=0x%04x\n", cbc);
    return ret;
}

static fm_s32 rds_event_set(fm_u16 *events, fm_s32 event_mask)
{
    FMR_ASSERT(events);
    *events |= event_mask;

    return 0;
}

static fm_s32 rds_flag_set(fm_u32 *flags, fm_s32 flag_mask)
{
    FMR_ASSERT(flags);
    *flags |= flag_mask;

    return 0;
}

static fm_s32 rds_grp_type_get(fm_u16 crc, fm_u16 blk, fm_u8 *type, fm_u8 *subtype)
{
    fm_bool valid = fm_false;

    FMR_ASSERT(type);
    FMR_ASSERT(subtype);
    //to get the group type from block B
    rds_checksum_check(crc, FM_RDS_GDBK_IND_B, &valid);

    if (valid == fm_true) {
        *type = (blk & 0xF000) >> 12; //Group type(4bits)
        *subtype = (blk & 0x0800) >> 11; //version code(1bit), 0=vesionA, 1=versionB
    } else {
        WCN_DBG(FM_WAR | RDSC, "Block1 CRC err\n");
        return -FM_ECRC;
    }

    WCN_DBG(FM_INF | RDSC, "Type=%d, subtype:%s\n", (fm_s32)*type, *subtype ? "version B" : "version A");
    return 0;
}

static fm_s32 rds_grp_counter_add(fm_u8 type, fm_u8 subtype, struct rds_group_cnt_t *gc)
{
    FMR_ASSERT(gc);

    if (type > 15) {
        return -FM_EPARA;
    }

    switch (subtype) {
    case RDS_GRP_VER_A:
        gc->groupA[type]++;
        break;
    case RDS_GRP_VER_B:
        gc->groupB[type]++;
        break;
    default:
        return -FM_EPARA;
        break;
    }

    gc->total++;
    WCN_DBG(FM_INF | RDSC, "group counter:%d\n", (fm_s32)gc->total);
    return 0;
}

extern fm_s32 rds_grp_counter_get(struct rds_group_cnt_t *dst, struct rds_group_cnt_t *src)
{
    FMR_ASSERT(dst);
    FMR_ASSERT(src);
    fm_memcpy(dst, src, sizeof(struct rds_group_cnt_t));
    WCN_DBG(FM_DBG | RDSC, "rds gc get[total=%d]\n", (fm_s32)dst->total);
    return 0;
}

extern fm_s32 rds_grp_counter_reset(struct rds_group_cnt_t *gc)
{
    FMR_ASSERT(gc);
    fm_memset(gc, 0, sizeof(struct rds_group_cnt_t));
    return 0;
}

extern fm_s32 rds_log_in(struct rds_log_t *thiz, struct rds_rx_t *new_log, fm_s32 new_len)
{
    FMR_ASSERT(new_log);

    if (thiz->len < thiz->size) {
        new_len = (new_len < sizeof(struct rds_rx_t)) ? new_len : sizeof(struct rds_rx_t);
        fm_memcpy(&(thiz->rds_log[thiz->in]), new_log, new_len);
        thiz->log_len[thiz->in] = new_len;
        thiz->in = (thiz->in + 1) % thiz->size;
        thiz->len++;
        WCN_DBG(FM_DBG | RDSC, "add a new log[len=%d]\n", thiz->len);
    } else {
        WCN_DBG(FM_WAR | RDSC, "rds log buf is full\n");
        return -FM_ENOMEM;
    }

    return 0;
}

extern fm_s32 rds_log_out(struct rds_log_t *thiz, struct rds_rx_t *dst, fm_s32 *dst_len)
{
    FMR_ASSERT(dst);
    FMR_ASSERT(dst_len);

    if (thiz->len > 0) {
        *dst_len = thiz->log_len[thiz->out];
        *dst_len = (*dst_len < sizeof(struct rds_rx_t)) ? *dst_len : sizeof(struct rds_rx_t);
        fm_memcpy(dst, &(thiz->rds_log[thiz->out]), *dst_len);
        thiz->out = (thiz->out + 1) % thiz->size;
        thiz->len--;
        WCN_DBG(FM_DBG | RDSC, "del a new log[len=%d]\n", thiz->len);
    } else {
        *dst_len = 0;
        WCN_DBG(FM_WAR | RDSC, "rds log buf is empty\n");
    }

    return 0;
}

static fm_s32 rds_grp_pi_get(fm_u16 crc, fm_u16 blk, fm_u16 *pi, fm_bool *dirty)
{
    fm_s32 ret = 0;
    fm_bool valid = fm_false;

    FMR_ASSERT(pi);
    FMR_ASSERT(dirty);

    //to get the group pi code from block A
    ret = rds_checksum_check(crc, FM_RDS_GDBK_IND_A, &valid);

    if (valid == fm_true) {
        if (*pi != blk) {
            //PI=program Identication
            *pi = blk;
            *dirty = fm_true; // yes, we got new PI code
        } else {
            *dirty = fm_false; // PI is the same as last one
        }
    } else {
        WCN_DBG(FM_WAR | RDSC, "Block0 CRC err\n");
        return -FM_ECRC;
    }

    WCN_DBG(FM_INF | RDSC, "PI=0x%04x, %s\n", *pi, *dirty ? "new" : "old");
    return ret;
}

static fm_s32 rds_grp_pty_get(fm_u16 crc, fm_u16 blk, fm_u8 *pty, fm_bool *dirty)
{
    fm_s32 ret = 0;
    fm_bool valid = fm_false;

    FMR_ASSERT(pty);
    FMR_ASSERT(dirty);

    //to get PTY code from block B
    ret = rds_checksum_check(crc, FM_RDS_GDBK_IND_B, &valid);

    if (valid == fm_false) {
        WCN_DBG(FM_WAR | RDSC, "Block1 CRC err\n");
        return -FM_ECRC;
    }

    if (*pty != ((blk & 0x03E0) >> 5)) {
        //PTY=Program Type Code
        *pty = (blk & 0x03E0) >> 5;
        *dirty = fm_true; // yes, we got new PTY code
    } else {
        *dirty = fm_false; // PTY is the same as last one
    }

    WCN_DBG(FM_INF | RDSC, "PTY=%d, %s\n", (fm_s32)*pty, *dirty ? "new" : "old");
    return ret;
}

static fm_s32 rds_grp_tp_get(fm_u16 crc, fm_u16 blk, fm_u8 *tp, fm_bool *dirty)
{
    fm_s32 ret = 0;
    fm_bool valid = fm_false;

    FMR_ASSERT(tp);
    FMR_ASSERT(dirty);

    //to get TP code from block B
    ret = rds_checksum_check(crc, FM_RDS_GDBK_IND_B, &valid);

    if (valid == fm_false) {
        WCN_DBG(FM_WAR | RDSC, "Block1 CRC err\n");
        return -FM_ECRC;
    }

    if (*tp != ((blk&0x0400) >> 10)) {
        //Tranfic Program Identification
        *tp = (blk & 0x0400) >> 10;
        *dirty = fm_true; // yes, we got new TP code
    } else {
        *dirty = fm_false; // TP is the same as last one
    }

    WCN_DBG(FM_INF | RDSC, "TP=%d, %s\n", (fm_s32)*tp, *dirty ? "new" : "old");
    return ret;
}

static fm_s32 rds_g0_ta_get(fm_u16 blk, fm_u8 *ta, fm_bool *dirty)
{
    fm_s32 ret = 0;

    FMR_ASSERT(ta);
    FMR_ASSERT(dirty);

    //TA=Traffic Announcement code
    if (*ta != ((blk & 0x0010) >> 4)) {
        *ta = (blk & 0x0010) >> 4;
        *dirty = fm_true; // yes, we got new TA code
    } else {
        *dirty = fm_false; // TA is the same as last one
    }

    WCN_DBG(FM_INF | RDSC, "TA=%d, %s\n", (fm_s32)*ta, *dirty ? "new" : "old");
    return ret;
}

static fm_s32 rds_g0_music_get(fm_u16 blk, fm_u8 *music, fm_bool *dirty)
{
    fm_s32 ret = 0;

    FMR_ASSERT(music);
    FMR_ASSERT(dirty);

    //M/S=music speech switch code
    if (*music != ((blk & 0x0008) >> 3)) {
        *music = (blk & 0x0008) >> 3;
        *dirty = fm_true; // yes, we got new music code
    } else {
        *dirty = fm_false; // music  is the same as last one
    }

    WCN_DBG(FM_INF | RDSC, "Music=%d, %s\n", (fm_s32)*music, *dirty ? "new" : "old");
    return ret;
}

static fm_s32 rds_g0_ps_addr_get(fm_u16 blkB, fm_u8 *addr)
{
    FMR_ASSERT(addr);
    *addr = (fm_u8)blkB & 0x03;

    WCN_DBG(FM_INF | RDSC, "addr=0x%02x\n", *addr);
    return 0;
}

static fm_s32 rds_g0_di_flag_get(fm_u16 blkB, fm_u8 *flag)
{
    FMR_ASSERT(flag);
    *flag = (fm_u8)((blkB & 0x0004) >> 2);

    WCN_DBG(FM_INF | RDSC, "flag=0x%02x\n", *flag);
    return 0;
}

static fm_s32 rds_g0_ps_get(fm_u16 crc, fm_u16 blkD, fm_u8 addr, fm_u8 *buf)
{
    fm_bool valid = fm_false;
    fm_s32 idx = 0;

    FMR_ASSERT(buf);

    //ps segment addr rang 0~3
    if (addr > 0x03) {
        WCN_DBG(FM_ERR | RDSC, "addr invalid(0x%02x)\n", addr);
        return -FM_EPARA;
    } else {
        idx = 2 * addr;
    }

    rds_checksum_check(crc, FM_RDS_GDBK_IND_D, &valid);

    if (valid == fm_true) {
        buf[idx] = blkD >> 8;
        buf[idx+1] = blkD & 0xFF;
    } else {
        WCN_DBG(FM_ERR | RDSC, "ps crc check err\n");
        return -FM_ECRC;
    }

    WCN_DBG(FM_INF | RDSC, "addr[%02x]:0x%02x 0x%02x\n", addr, buf[idx], buf[idx+1]);
    return 0;
}

static fm_s32 rds_g0_ps_cmp(fm_u8 addr, fm_u16 cbc, fm_u8 *fresh,
                            fm_u8 *once, fm_u8 *twice, fm_bool *valid)
{
    fm_s32 ret = 0;
    fm_s32 i = 0;
    fm_s32 j = 0;
    fm_s32 cnt = 0;

    FMR_ASSERT(fresh);
    FMR_ASSERT(once);
    FMR_ASSERT(twice);
    FMR_ASSERT(valid);

    j = 2; // PS segment width

    if (rds_cbc_get(cbc, RDS_BLK_D) == 0) {
        once[j*addr+0] = fresh[j*addr+0];
        once[j*addr+1] = fresh[j*addr+1];
    }

    for (i = 0; i < j; i++) {
        if (fresh[j*addr+i] == once[j*addr+i]) {
            twice[j*addr+i] = once[j*addr+i]; //get the same byte 2 times
            cnt++;
        } else {
            once[j*addr+i] = fresh[j*addr+i]; //use new val
        }
    }

    //check if we got a valid segment
    if (cnt == j) {
        *valid = fm_true;
    } else {
        *valid = fm_false;
    }

    WCN_DBG(FM_INF | RDSC, "PS seg=%s\n", *valid == fm_true ? "fm_true" : "fm_false");
    return ret;
}

struct rds_bitmap {
    fm_u16 bm;
    fm_s32 cnt;
    fm_s32 max_addr;
    fm_u16(*bm_get)(struct rds_bitmap *thiz);
    fm_s32(*bm_cnt_get)(struct rds_bitmap *thiz);
    fm_s32(*bm_get_pos)(struct rds_bitmap *thiz);
    fm_s32(*bm_clr)(struct rds_bitmap *thiz);
    fm_s32(*bm_cmp)(struct rds_bitmap *thiz, struct rds_bitmap *that);
    fm_s32(*bm_set)(struct rds_bitmap *thiz, fm_u8 addr);
};

static fm_u16 rds_bm_get(struct rds_bitmap *thiz)
{
    return thiz->bm;
}

static fm_s32 rds_bm_cnt_get(struct rds_bitmap *thiz)
{
    return thiz->cnt;
}

#define FM_RDS_USE_SOLUTION_B

static fm_s32 rds_bm_get_pos(struct rds_bitmap *thiz)
{
    fm_s32 i = thiz->max_addr;
    fm_s32 j;

    j = 0;

    while (!(thiz->bm & (1 << i)) && (i > -1)) {
        i--;
    }

#ifdef FM_RDS_USE_SOLUTION_B
    for (j = i; j >= 0; j--) {
        if (!(thiz->bm & (1 << j))) {
            WCN_DBG(FM_NTC | RDSC, "uncomplete msg 0x%04x, delete it\n", thiz->bm);
            return -1;
        }
    }
#endif

    return i;
}

static fm_s32 rds_bm_clr(struct rds_bitmap *thiz)
{
    thiz->bm = 0x0000;
    thiz->cnt = 0;
    return 0;
}

static fm_s32 rds_bm_cmp(struct rds_bitmap *bitmap1, struct rds_bitmap *bitmap2)
{
    return (fm_s32)(bitmap1->bm - bitmap2->bm);
}

static fm_s32 rds_bm_set(struct rds_bitmap *thiz, fm_u8 addr)
{
    struct rds_bitmap bm_old;

    //text segment addr rang
    if (addr > thiz->max_addr) {
        WCN_DBG(FM_ERR | RDSC, "addr invalid(0x%02x)\n", addr);
        return -FM_EPARA;
    }

    bm_old.bm = thiz->bm;
    thiz->bm |= (1 << addr); //set bitmap

    if (!rds_bm_cmp(&bm_old, thiz)) {
        thiz->cnt++;  // multi get a segment
    } else if (thiz->cnt > 0) {
        thiz->cnt--;
    }

    WCN_DBG(FM_NTC | RDSC, "bitmap=0x%04x, bmcnt=%d\n", thiz->bm, thiz->cnt);
    return 0;
}


static fm_s32 rds_g2_rt_addr_get(fm_u16 blkB, fm_u8 *addr)
{
    fm_s32 ret = 0;

    FMR_ASSERT(addr);
    *addr = (fm_u8)blkB & 0x0F;

    WCN_DBG(FM_INF | RDSC, "addr=0x%02x\n", *addr);
    return ret;
}

static fm_s32 rds_g2_txtAB_get(fm_u16 blk, fm_u8 *txtAB, fm_bool *dirty)
{
    fm_s32 ret = 0;

    FMR_ASSERT(txtAB);
    FMR_ASSERT(dirty);

    if (*txtAB != ((blk&0x0010) >> 4)) {
        *txtAB = (blk & 0x0010) >> 4;
        *dirty = fm_true; // yes, we got new txtAB code
    } else {
        *dirty = fm_false; // txtAB is the same as last one
    }

    WCN_DBG(FM_INF | RDSC, "txtAB=%d, %s\n", *txtAB, *dirty ? "new" : "old");
    return ret;
}

static fm_s32 rds_g2_rt_get(fm_u16 crc, fm_u8 subtype, fm_u16 blkC, fm_u16 blkD, fm_u8 addr, fm_u8 *buf)
{
    fm_s32 ret = 0;
    fm_bool valid = fm_false;
    fm_s32 idx = 0;

    FMR_ASSERT(buf);

    //text segment addr rang 0~15
    if (addr > 0x0F) {
        WCN_DBG(FM_ERR | RDSC, "addr invalid(0x%02x)\n", addr);
        ret = -FM_EPARA;
        return ret;
    }

    switch (subtype) {
    case RDS_GRP_VER_A:
        idx = 4 * addr;
        ret = rds_checksum_check(crc, FM_RDS_GDBK_IND_C | FM_RDS_GDBK_IND_D, &valid);

        if (valid == fm_true) {
            buf[idx] = blkC >> 8;
            buf[idx+1] = blkC & 0xFF;
            buf[idx+2] = blkD >> 8;
            buf[idx+3] = blkD & 0xFF;
        } else {
            WCN_DBG(FM_ERR | RDSC, "rt crc check err\n");
            ret = -FM_ECRC;
        }

        break;
    case RDS_GRP_VER_B:
        idx = 2 * addr;
        ret = rds_checksum_check(crc, FM_RDS_GDBK_IND_D, &valid);

        if (valid == fm_true) {
            buf[idx] = blkD >> 8;
            buf[idx+1] = blkD & 0xFF;
        } else {
            WCN_DBG(FM_ERR | RDSC, "rt crc check err\n");
            ret = -FM_ECRC;
        }

        break;
    default:
        break;
    }

    WCN_DBG(FM_INF | RDSC, "addr[%02x]:0x%02x 0x%02x 0x%02x 0x%02x\n", addr, buf[idx], buf[idx+1], buf[idx+2], buf[idx+3]);
    return ret;
}

static fm_s32 rds_g2_rt_get_len(fm_u8 subtype, fm_s32 pos, fm_s32 *len)
{
    fm_s32 ret = 0;

    FMR_ASSERT(len);

    if (subtype == RDS_GRP_VER_A) {
        *len = 4 * (pos + 1);
    } else {
        *len = 2 * (pos + 1);
    }

    return ret;
}

static fm_s32 rds_g2_rt_cmp(fm_u8 addr, fm_u16 cbc, fm_u8 subtype, fm_u8 *fresh,
                            fm_u8 *once, fm_u8 *twice, fm_bool *valid, fm_bool *end, fm_s32 *len)
{
    fm_s32 ret = 0;
    fm_s32 i = 0;
    fm_s32 j = 0;
    fm_s32 cnt = 0;

    FMR_ASSERT(fresh);
    FMR_ASSERT(once);
    FMR_ASSERT(twice);
    FMR_ASSERT(valid);
    FMR_ASSERT(end);

    j = (subtype == RDS_GRP_VER_A) ? 4 : 2; // RT segment width

    if (subtype == RDS_GRP_VER_A) {
        if (rds_cbc_get(cbc, RDS_BLK_C) == 0) {
            once[j*addr+0] = fresh[j*addr+0];
            once[j*addr+1] = fresh[j*addr+1];
        }

        if (rds_cbc_get(cbc, RDS_BLK_D) == 0) {
            once[j*addr+2] = fresh[j*addr+2];
            once[j*addr+3] = fresh[j*addr+3];
        }
    } else if (subtype == RDS_GRP_VER_B) {
        if (rds_cbc_get(cbc, RDS_BLK_D) == 0) {
            once[j*addr+0] = fresh[j*addr+0];
            once[j*addr+1] = fresh[j*addr+1];
        }
    }

    for (i = 0; i < j; i++) {
        if (fresh[j*addr+i] == once[j*addr+i]) {
            twice[j*addr+i] = once[j*addr+i]; //get the same byte 2 times
            cnt++;
        } else {
            once[j*addr+i] = fresh[j*addr+i]; //use new val
        }

        //if we got 0x0D twice, it means a RT end
        if (twice[j*addr+i] == 0x0D) {
            *end = fm_true;
            *len = j * addr + i + 1; //record the length of RT
        }
    }

    //check if we got a valid segment 4bytes for typeA, 2bytes for typeB
    if (cnt == j) {
        *valid = fm_true;
    } else {
        *valid = fm_false;
    }

    WCN_DBG(FM_INF | RDSC, "RT seg=%s\n", *valid == fm_true ? "fm_true" : "fm_false");
    WCN_DBG(FM_INF | RDSC, "RT end=%s\n", *end == fm_true ? "fm_true" : "fm_false");
    WCN_DBG(FM_INF | RDSC, "RT len=%d\n", *len);
    return ret;
}

static fm_s32 rds_retrieve_g0_af(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    static fm_s16 preAF_Num = 0;
    fm_u8 indx, indx2, AF_H, AF_L, num;
    fm_s32 ret = 0;
    fm_bool valid = fm_false;
    fm_bool dirty = fm_false;
    fm_u16 *event = &pstRDSData->event_status;
    fm_u32 *flag = &pstRDSData->RDSFlag.flag_status;

    ret = rds_checksum_check(block_data[4], FM_RDS_GDBK_IND_D, &valid);

    if (valid == fm_false) {
        WCN_DBG(FM_WAR | RDSC, "Group0 BlockD crc err\n");
        return -FM_ECRC;
    }

    ret = rds_g0_ta_get(block_data[1], &pstRDSData->RDSFlag.TA, &dirty);

    if (ret) {
        WCN_DBG(FM_WAR | RDSC, "get ta failed[ret=%d]\n", ret);
    } else if (dirty == fm_true) {
        ret = rds_event_set(event, RDS_EVENT_FLAGS); // yes, we got new TA code
        ret = rds_flag_set(flag, RDS_FLAG_IS_TA);
    }

    ret = rds_g0_music_get(block_data[1], &pstRDSData->RDSFlag.Music, &dirty);

    if (ret) {
        WCN_DBG(FM_WAR | RDSC, "get music failed[ret=%d]\n", ret);
    } else if (dirty == fm_true) {
        ret = rds_event_set(event, RDS_EVENT_FLAGS); // yes, we got new MUSIC code
        ret = rds_flag_set(flag, RDS_FLAG_IS_MUSIC);
    }

    if ((pstRDSData->Switch_TP) && (pstRDSData->RDSFlag.TP) && !(pstRDSData->RDSFlag.TA)) {
        ret = rds_event_set(event, RDS_EVENT_TAON_OFF);
    }

    if (!SubType) {
        //Type A
        ret = rds_checksum_check(block_data[4], FM_RDS_GDBK_IND_C, &valid);

        if (valid == fm_false) {
            WCN_DBG(FM_WAR | RDSC, "Group0 BlockC crc err\n");
            return -FM_ECRC;
        } else {
            AF_H = (block_data[2] & 0xFF00) >> 8;
            AF_L = block_data[2] & 0x00FF;

            if ((AF_H > 224) && (AF_H < 250)) {
                //Followed AF Number, see RDS spec Table 11, valid(224-249)
                WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 AF_H:%d, AF_L:%d\n", AF_H, AF_L);
                pstRDSData->AF_Data.isAFNum_Get = 0;
                preAF_Num = AF_H - 224; //AF Number

                if (preAF_Num != pstRDSData->AF_Data.AF_Num) {
                    pstRDSData->AF_Data.AF_Num = preAF_Num;
                } else {
                    //Get the same AFNum two times
                    pstRDSData->AF_Data.isAFNum_Get = 1;
                }

                if ((AF_L < 205) && (AF_L > 0)) {
                    //See RDS Spec table 10, valid VHF
                    pstRDSData->AF_Data.AF[0][0] = AF_L + 875; //convert to 100KHz
#ifdef MTK_FM_50KHZ_SUPPORT
                    pstRDSData->AF_Data.AF[0][0] *= 10;
#endif
                    WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 AF[0][0]:%d\n", pstRDSData->AF_Data.AF[0][0]);

                    if ((pstRDSData->AF_Data.AF[0][0]) != (pstRDSData->AF_Data.AF[1][0])) {
                        pstRDSData->AF_Data.AF[1][0] = pstRDSData->AF_Data.AF[0][0];
                    } else {
                        if (pstRDSData->AF_Data.AF[1][0] !=  rds_get_freq())
                            pstRDSData->AF_Data.isMethod_A = 1;
                        else
                            pstRDSData->AF_Data.isMethod_A = 0;
                    }

                    WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 isAFNum_Get:%d, isMethod_A:%d\n", pstRDSData->AF_Data.isAFNum_Get, pstRDSData->AF_Data.isMethod_A);

                    //only one AF handle
                    if ((pstRDSData->AF_Data.isAFNum_Get) && (pstRDSData->AF_Data.AF_Num == 1)) {
                        pstRDSData->AF_Data.Addr_Cnt = 0xFF;
                        pstRDSData->event_status |= RDS_EVENT_AF_LIST;
                        WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 RDS_EVENT_AF_LIST update\n");
                    }
                }
            } else if ((pstRDSData->AF_Data.isAFNum_Get) && (pstRDSData->AF_Data.Addr_Cnt != 0xFF)) {
                //AF Num correct
                num = pstRDSData->AF_Data.AF_Num;
                num = num >> 1;
                WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 +num:%d\n", num);

                //Put AF freq fm_s32o buffer and check if AF freq is repeat again
                for (indx = 1; indx < (num + 1); indx++) {
                    if ((AF_H == (pstRDSData->AF_Data.AF[0][2*num-1])) && (AF_L == (pstRDSData->AF_Data.AF[0][2*indx]))) {
                        WCN_DBG(FM_ERR | RDSC, "RetrieveGroup0 AF same as indx:%d\n", indx);
                        break;
                    } else if (!(pstRDSData->AF_Data.AF[0][2*indx-1])) {
                        //null buffer
                        pstRDSData->AF_Data.AF[0][2*indx-1] = AF_H + 875; //convert to 100KHz
                        pstRDSData->AF_Data.AF[0][2*indx] = AF_L + 875;
                        
#ifdef MTK_FM_50KHZ_SUPPORT
                        pstRDSData->AF_Data.AF[0][2*indx-1] *= 10;
                        pstRDSData->AF_Data.AF[0][2*indx] *= 10;
#endif
                        WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 AF[0][%d]:%d, AF[0][%d]:%d\n",
                                2*indx - 1, pstRDSData->AF_Data.AF[0][2*indx-1], 2*indx, pstRDSData->AF_Data.AF[0][2*indx]);
                        break;
                    }
                }

                num = pstRDSData->AF_Data.AF_Num;
                WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 ++num:%d\n", num);

                if (num > 0) {
                    if ((pstRDSData->AF_Data.AF[0][num-1]) != 0) {
                        num = num >> 1;
                        WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 +++num:%d\n", num);

                        //arrange frequency from low to high:start
                        for (indx = 1; indx < num; indx++) {
                            for (indx2 = indx + 1; indx2 < (num + 1); indx2++) {
                                AF_H = pstRDSData->AF_Data.AF[0][2*indx-1];
                                AF_L = pstRDSData->AF_Data.AF[0][2*indx];

                                if (AF_H > (pstRDSData->AF_Data.AF[0][2*indx2-1])) {
                                    pstRDSData->AF_Data.AF[0][2*indx-1] = pstRDSData->AF_Data.AF[0][2*indx2-1];
                                    pstRDSData->AF_Data.AF[0][2*indx] = pstRDSData->AF_Data.AF[0][2*indx2];
                                    pstRDSData->AF_Data.AF[0][2*indx2-1] = AF_H;
                                    pstRDSData->AF_Data.AF[0][2*indx2] = AF_L;
                                } else if (AF_H == (pstRDSData->AF_Data.AF[0][2*indx2-1])) {
                                    if (AF_L > (pstRDSData->AF_Data.AF[0][2*indx2])) {
                                        pstRDSData->AF_Data.AF[0][2*indx-1] = pstRDSData->AF_Data.AF[0][2*indx2-1];
                                        pstRDSData->AF_Data.AF[0][2*indx] = pstRDSData->AF_Data.AF[0][2*indx2];
                                        pstRDSData->AF_Data.AF[0][2*indx2-1] = AF_H;
                                        pstRDSData->AF_Data.AF[0][2*indx2] = AF_L;
                                    }
                                }
                            }
                        }

                        //arrange frequency from low to high:end
                        //compare AF buff0 and buff1 data:start
                        num = pstRDSData->AF_Data.AF_Num;
                        indx2 = 0;

                        for (indx = 0; indx < num; indx++) {
                            if ((pstRDSData->AF_Data.AF[1][indx]) == (pstRDSData->AF_Data.AF[0][indx])) {
                                if (pstRDSData->AF_Data.AF[1][indx] != 0)
                                    indx2++;
                            } else
                                pstRDSData->AF_Data.AF[1][indx] = pstRDSData->AF_Data.AF[0][indx];
                        }

                        WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 indx2:%d, num:%d\n", indx2, num);

                        //compare AF buff0 and buff1 data:end
                        if (indx2 == num) {
                            pstRDSData->AF_Data.Addr_Cnt = 0xFF;
                            pstRDSData->event_status |= RDS_EVENT_AF_LIST;
                            WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 AF_Num:%d\n", pstRDSData->AF_Data.AF_Num);

                            for (indx = 0; indx < num; indx++) {
                                if ((pstRDSData->AF_Data.AF[1][indx]) == 0) {
                                    pstRDSData->AF_Data.Addr_Cnt = 0x0F;
                                    pstRDSData->event_status &= (~RDS_EVENT_AF_LIST);
                                }
                            }
                        } else
                            pstRDSData->AF_Data.Addr_Cnt = 0x0F;
                    }
                }
            }
        }
    }

    return ret;
}

static fm_s32 rds_retrieve_g0_di(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    fm_u8 DI_Code, DI_Flag;
    fm_s32 ret = 0;
    fm_bool valid = fm_false;

    fm_u16 *event = &pstRDSData->event_status;
    fm_u32 *flag = &pstRDSData->RDSFlag.flag_status;

    //parsing Program service name segment (in BlockD)
    ret = rds_checksum_check(block_data[4], FM_RDS_GDBK_IND_D, &valid);

    if (valid == fm_false) {
        WCN_DBG(FM_WAR | RDSC, "Group0 BlockD crc err\n");
        return -FM_ECRC;
    }

    rds_g0_ps_addr_get(block_data[1], &DI_Code);
    rds_g0_di_flag_get(block_data[1], &DI_Flag);

    switch (DI_Code) {
    case 3:

        if (pstRDSData->RDSFlag.Stereo != DI_Flag) {
            pstRDSData->RDSFlag.Stereo = DI_Flag;
            ret = rds_event_set(event, RDS_EVENT_FLAGS);
            ret = rds_flag_set(flag, RDS_FLAG_IS_STEREO);
        }

        break;
    case 2:

        if (pstRDSData->RDSFlag.Artificial_Head != DI_Flag) {
            pstRDSData->RDSFlag.Artificial_Head = DI_Flag;
            ret = rds_event_set(event, RDS_EVENT_FLAGS);
            ret = rds_flag_set(flag, RDS_FLAG_IS_ARTIFICIAL_HEAD);
        }

        break;
    case 1:

        if (pstRDSData->RDSFlag.Compressed != DI_Flag) {
            pstRDSData->RDSFlag.Compressed = DI_Flag;
            ret = rds_event_set(event, RDS_EVENT_FLAGS);
            ret = rds_flag_set(flag, RDS_FLAG_IS_COMPRESSED);
        }

        break;
    case 0:

        if (pstRDSData->RDSFlag.Dynamic_PTY != DI_Flag) {
            pstRDSData->RDSFlag.Dynamic_PTY = DI_Flag;
            ret = rds_event_set(event, RDS_EVENT_FLAGS);
            ret = rds_flag_set(flag, RDS_FLAG_IS_DYNAMIC_PTY);
        }

        break;
    default:
        break;
    }

    return ret;
}

static fm_s32 rds_retrieve_g0_ps(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    fm_u8 ps_addr;
    fm_s32 ret = 0;
    fm_bool valid = fm_false;
    fm_s32 pos = 0;
    static struct fm_state_machine ps_sm = {
        .state = RDS_PS_START,
        .state_get = fm_state_get,
        .state_set = fm_state_set,
    };
    static struct rds_bitmap ps_bm = {
        .bm = 0,
        .cnt = 0,
        .max_addr = 0x03,
        .bm_get = rds_bm_get,
        .bm_cnt_get = rds_bm_cnt_get,
        .bm_set = rds_bm_set,
        .bm_get_pos = rds_bm_get_pos,
        .bm_clr = rds_bm_clr,
        .bm_cmp = rds_bm_cmp,
    };

    fm_u16 *event = &pstRDSData->event_status;

    //parsing Program service name segment (in BlockD)
    ret = rds_checksum_check(block_data[4], FM_RDS_GDBK_IND_D, &valid);

    if (valid == fm_false) {
        WCN_DBG(FM_WAR | RDSC, "Group0 BlockD crc err\n");
        return -FM_ECRC;
    }

    rds_g0_ps_addr_get(block_data[1], &ps_addr);

	//PR-367250-fred.wang begin
	pos=ps_bm.bm_get_pos(&ps_bm);
    if(!((pos==ps_addr)||(pos+1==ps_addr)))return ret;
	//PR-367250-fred.wang end
	
    //PS parsing state machine run
    while (1) {
        switch (STATE_GET(&ps_sm)) {
        case RDS_PS_START:

            if (rds_g0_ps_get(block_data[4], block_data[3], ps_addr, pstRDSData->PS_Data.PS[0])) {
                STATE_SET(&ps_sm, RDS_PS_FINISH); //if CRC error, we should not do parsing
                break;
            }

            rds_g0_ps_cmp(ps_addr, block_data[5], pstRDSData->PS_Data.PS[0],
                          pstRDSData->PS_Data.PS[1], pstRDSData->PS_Data.PS[2], &valid);

            if (valid == fm_true) {
                ps_bm.bm_set(&ps_bm, ps_addr);
            }

            STATE_SET(&ps_sm, RDS_PS_DECISION);
            break;
        case RDS_PS_DECISION:

            if (ps_bm.bm_get(&ps_bm) == 0x000F //get max  8 chars
                    || (ps_bm.bm_cnt_get(&ps_bm) > RDS_RT_MULTI_REV_TH)) { //repeate many times, but no end char get
                pos = ps_bm.bm_get_pos(&ps_bm);
                STATE_SET(&ps_sm, RDS_PS_GETLEN);
            } else {
                STATE_SET(&ps_sm, RDS_PS_FINISH);
            }

            break;
        case RDS_PS_GETLEN:
            
            if (pos == ps_bm.max_addr) {
                fm_memcpy(pstRDSData->PS_Data.PS[3], pstRDSData->PS_Data.PS[2], 8);
                rds_event_set(event, RDS_EVENT_PROGRAMNAME); //yes we got a new PS
                WCN_DBG(FM_NTC | RDSC, "Yes, get an PS!\n");
            }

            ps_bm.bm_clr(&ps_bm);
            //clear buf
            fm_memset(pstRDSData->PS_Data.PS[0], 0x20, 8);
            fm_memset(pstRDSData->PS_Data.PS[1], 0x20, 8);
            fm_memset(pstRDSData->PS_Data.PS[2], 0x20, 8);
            STATE_SET(&ps_sm, RDS_PS_FINISH);
            break;
        case RDS_PS_FINISH:
            STATE_SET(&ps_sm, RDS_PS_START);
            goto out;
            break;
        default:
            break;
        }
    }

out:
    return ret;
}

static fm_s32 rds_retrieve_g0(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    fm_s32 ret = 0;

    ret = rds_retrieve_g0_af(block_data, SubType, pstRDSData);

    if (ret) {
        return ret;
    }

    ret = rds_retrieve_g0_di(block_data, SubType, pstRDSData);

    if (ret) {
        return ret;
    }

    ret = rds_retrieve_g0_ps(block_data, SubType, pstRDSData);

    if (ret) {
        return ret;
    }

    return ret;
}

static fm_s32 rds_retrieve_g1(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    fm_u8 variant_code = (block_data[2] & 0x7000) >> 12;
    fm_s32 ret = 0;

    if (variant_code == 0) {
        pstRDSData->Extend_Country_Code = (fm_u8)block_data[2] & 0xFF;
        WCN_DBG(FM_DBG | RDSC, "Extend_Country_Code:%d\n", pstRDSData->Extend_Country_Code);
    } else if (variant_code == 3) {
        pstRDSData->Language_Code = block_data[2] & 0xFFF;
        WCN_DBG(FM_DBG | RDSC, "Language_Code:%d\n", pstRDSData->Language_Code);
    }

    pstRDSData->Radio_Page_Code = block_data[1] & 0x001F;
    pstRDSData->Program_Item_Number_Code = block_data[3];

    return ret;
}

static fm_s32 rds_retrieve_g2(fm_u16 *source, fm_u8 subtype, rds_t *target)
{
    fm_s32 ret = 0;
    fm_u16 crc, cbc;
    fm_u16 blkA, blkB, blkC, blkD;
    fm_u8 *fresh, *once, *twice, *display;
    fm_u16 *event;
    fm_u32 *flag;
    static struct fm_state_machine rt_sm = {
        .state = RDS_RT_START,
        .state_get = fm_state_get,
        .state_set = fm_state_set,
    };
    static struct rds_bitmap rt_bm = {
        .bm = 0,
        .cnt = 0,
        .max_addr = 0x15,
        .bm_get = rds_bm_get,
        .bm_cnt_get = rds_bm_cnt_get,
        .bm_set = rds_bm_set,
        .bm_get_pos = rds_bm_get_pos,
        .bm_clr = rds_bm_clr,
        .bm_cmp = rds_bm_cmp,
    };
    fm_u8 rt_addr = 0;
    fm_bool txtAB_change = fm_false;  //text AB flag 0 --> 1 or 1-->0 meas new RT incoming
    fm_bool txt_end = fm_false;       //0x0D means text end
    fm_bool seg_ok = 0;
    fm_s32 pos = 0;
    fm_s32 rt_len = 0;
    fm_s32 bufsize = 0;

    FMR_ASSERT(source);
    FMR_ASSERT(target);
    //source
    blkA = source[0];
    blkB = source[1];
    blkC = source[2];
    blkD = source[3];
    crc = source[4];
    cbc = source[5];
    //target
    fresh = target->RT_Data.TextData[0];
    once = target->RT_Data.TextData[1];
    twice = target->RT_Data.TextData[2];
    display = target->RT_Data.TextData[3];
    event = &target->event_status;
    flag = &target->RDSFlag.flag_status;
    bufsize = sizeof(target->RT_Data.TextData[0]);

    //get basic info: addr, txtAB
    if (rds_g2_rt_addr_get(blkB, &rt_addr))
        return ret;

    if (rds_g2_txtAB_get(blkB, &target->RDSFlag.Text_AB, &txtAB_change))
        return ret;

    //RT parsing state machine run
    while (1) {
        switch (STATE_GET(&rt_sm)) {
        case RDS_RT_START:

            if (txtAB_change == fm_true) {
                STATE_SET(&rt_sm, RDS_RT_DECISION);
                break;
            } else {
                if (rds_g2_rt_get(crc, subtype, blkC, blkD, rt_addr, fresh)) {
                    STATE_SET(&rt_sm, RDS_RT_FINISH); //if CRC error, we should not do parsing
                    break;
                }

                rds_g2_rt_cmp(rt_addr, cbc, subtype, fresh, once, twice,
                              &seg_ok, &txt_end, &rt_len);

                if (seg_ok == fm_true) {
                    rt_bm.bm_set(&rt_bm, rt_addr);
                }

                STATE_SET(&rt_sm, RDS_RT_DECISION);
                break;
            }

        case RDS_RT_DECISION:

            if ((txt_end == fm_true) //find 0x0D, and the lenth has been recorded when do rds_g2_rt_cmp()
                        || (rt_bm.bm_get(&rt_bm) == 0xFFFF) //get max  64 chars
                       || (txtAB_change == fm_true)  //text AB changed,
                       || (rt_bm.bm_cnt_get(&rt_bm) > RDS_RT_MULTI_REV_TH)) { //repeate many times, but no end char get
                pos = rt_bm.bm_get_pos(&rt_bm);
                rds_g2_rt_get_len(subtype, pos, &rt_len);
                STATE_SET(&rt_sm, RDS_RT_GETLEN);
            } else {
                STATE_SET(&rt_sm, RDS_RT_FINISH);
            }

            break;
        case RDS_RT_GETLEN:
            
            fm_memcpy(display, twice, bufsize);
            target->RT_Data.TextLength = rt_len;
            if (rt_len > 0 && ((txt_end == fm_true) || (rt_bm.bm_get(&rt_bm) == 0xFFFF))) {
                rds_event_set(event, RDS_EVENT_LAST_RADIOTEXT); //yes we got a new RT
                WCN_DBG(FM_NTC | RDSC, "Yes, get an RT! [len=%d]\n", rt_len);
            }

            rt_bm.bm_clr(&rt_bm);
            //clear buf
            fm_memset(fresh, 0x20, bufsize);
            fm_memset(once, 0x20, bufsize);
            fm_memset(twice, 0x20, bufsize);

            if (txtAB_change == fm_true) {
                txtAB_change = fm_false;
                //we need get new RT after show the old RT to the display
                STATE_SET(&rt_sm, RDS_RT_START);
            } else {
                STATE_SET(&rt_sm, RDS_RT_FINISH);
            }

            break;
        case RDS_RT_FINISH:
            STATE_SET(&rt_sm, RDS_RT_START);
            goto out;
            break;
        default:
            break;
        }
    }

out:
    return ret;
}

static fm_s32 rds_retrieve_g4(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    fm_u16 year, month, k = 0, D2, minute;
    fm_u32 MJD, D1;
    fm_s32 ret = 0;
    WCN_DBG(FM_DBG | RDSC, "RetrieveGroup4 %d\n", SubType);

    if (!SubType) {
        //Type A
        if ((block_data[4]&FM_RDS_GDBK_IND_C) && (block_data[4]&FM_RDS_GDBK_IND_D)) {
            MJD = (fm_u32)(((block_data[1] & 0x0003) << 15) + ((block_data[2] & 0xFFFE) >> 1));
            year = (MJD * 100 - 1507820) / 36525;
            month = (MJD * 10000 - 149561000 - 3652500 * year) / 306001;

            if ((month == 14) || (month == 15))
                k = 1;

            D1 = (fm_u32)((36525 * year) / 100);
            D2 = (fm_u16)((306001 * month) / 10000);
            pstRDSData->CT.Year = 1900 + year + k;
            pstRDSData->CT.Month = month - 1 - k * 12;
            pstRDSData->CT.Day = (fm_u16)(MJD - 14956 - D1 - D2);
            pstRDSData->CT.Hour = ((block_data[2] & 0x0001) << 4) + ((block_data[3] & 0xF000) >> 12);
            minute = (block_data[3] & 0x0FC0) >> 6;

            if (block_data[3]&0x0020) {
                pstRDSData->CT.Local_Time_offset_signbit = 1; //0=+, 1=-
            }

            pstRDSData->CT.Local_Time_offset_half_hour = block_data[3] & 0x001F;

            if (pstRDSData->CT.Minute != minute) {
                pstRDSData->CT.Minute = (block_data[3] & 0x0FC0) >> 6;
                pstRDSData->event_status |= RDS_EVENT_UTCDATETIME;
            }
        }
    }

    return ret;
}

static fm_s32 rds_retrieve_g14(fm_u16 *block_data, fm_u8 SubType, rds_t *pstRDSData)
{
    static fm_s16 preAFON_Num = 0;
    fm_u8 TP_ON, TA_ON, PI_ON, PS_Num, AF_H, AF_L, indx, indx2, num;
    fm_s32 ret = 0;
    WCN_DBG(FM_DBG | RDSC, "RetrieveGroup14 %d\n", SubType);
    //SubType = (*(block_data+1)&0x0800)>>11;
    PI_ON = block_data[3];
    TP_ON = block_data[1] & 0x0010;

    if ((!SubType) && (block_data[4]&FM_RDS_GDBK_IND_C)) {
        //Type A
        PS_Num = block_data[1] & 0x000F;

        if (PS_Num < 4) {
            for (indx = 0; indx < 2; indx++) {
                pstRDSData->PS_ON[2*PS_Num] = block_data[2] >> 8;
                pstRDSData->PS_ON[2*PS_Num+1] = block_data[2] & 0xFF;
            }
        } else if (PS_Num == 4) {
            AF_H = (block_data[2] & 0xFF00) >> 8;
            AF_L = block_data[2] & 0x00FF;

            if ((AF_H > 223) && (AF_H < 250)) {
                //Followed AF Number
                pstRDSData->AFON_Data.isAFNum_Get = 0;
                preAFON_Num = AF_H - 224;

                if (pstRDSData->AFON_Data.AF_Num != preAFON_Num) {
                    pstRDSData->AFON_Data.AF_Num = preAFON_Num;
                } else
                    pstRDSData->AFON_Data.isAFNum_Get = 1;

                if (AF_L < 205) {
                    pstRDSData->AFON_Data.AF[0][0] = AF_L + 875;

                    if ((pstRDSData->AFON_Data.AF[0][0]) != (pstRDSData->AFON_Data.AF[1][0])) {
                        pstRDSData->AFON_Data.AF[1][0] = pstRDSData->AFON_Data.AF[0][0];
                    } else {
                        pstRDSData->AFON_Data.isMethod_A = 1;
                    }
                }
            } else if ((pstRDSData->AFON_Data.isAFNum_Get) && ((pstRDSData->AFON_Data.Addr_Cnt) != 0xFF)) {
                //AF Num correct
                num = pstRDSData->AFON_Data.AF_Num;
                num = num >> 1;

                //Put AF freq fm_s32o buffer and check if AF freq is repeat again
                for (indx = 1; indx < (num + 1); indx++) {
                    if ((AF_H == (pstRDSData->AFON_Data.AF[0][2*indx-1])) && (AF_L == (pstRDSData->AFON_Data.AF[0][2*indx]))) {
                        WCN_DBG(FM_NTC | RDSC, "RetrieveGroup14 AFON same as indx:%d\n", indx);
                        break;
                    } else if (!(pstRDSData->AFON_Data.AF[0][2*indx-1])) {
                        //null buffer
                        pstRDSData->AFON_Data.AF[0][2*indx-1] = AF_H + 875;
                        pstRDSData->AFON_Data.AF[0][2*indx] = AF_L + 875;
                        break;
                    }
                }

                num = pstRDSData->AFON_Data.AF_Num;

                if (num > 0) {
                    if ((pstRDSData->AFON_Data.AF[0][num-1]) != 0) {
                        num = num >> 1;

                        //arrange frequency from low to high:start
                        for (indx = 1; indx < num; indx++) {
                            for (indx2 = indx + 1; indx2 < (num + 1); indx2++) {
                                AF_H = pstRDSData->AFON_Data.AF[0][2*indx-1];
                                AF_L = pstRDSData->AFON_Data.AF[0][2*indx];

                                if (AF_H > (pstRDSData->AFON_Data.AF[0][2*indx2-1])) {
                                    pstRDSData->AFON_Data.AF[0][2*indx-1] = pstRDSData->AFON_Data.AF[0][2*indx2-1];
                                    pstRDSData->AFON_Data.AF[0][2*indx] = pstRDSData->AFON_Data.AF[0][2*indx2];
                                    pstRDSData->AFON_Data.AF[0][2*indx2-1] = AF_H;
                                    pstRDSData->AFON_Data.AF[0][2*indx2] = AF_L;
                                } else if (AF_H == (pstRDSData->AFON_Data.AF[0][2*indx2-1])) {
                                    if (AF_L > (pstRDSData->AFON_Data.AF[0][2*indx2])) {
                                        pstRDSData->AFON_Data.AF[0][2*indx-1] = pstRDSData->AFON_Data.AF[0][2*indx2-1];
                                        pstRDSData->AFON_Data.AF[0][2*indx] = pstRDSData->AFON_Data.AF[0][2*indx2];
                                        pstRDSData->AFON_Data.AF[0][2*indx2-1] = AF_H;
                                        pstRDSData->AFON_Data.AF[0][2*indx2] = AF_L;
                                    }
                                }
                            }
                        }

                        //arrange frequency from low to high:end
                        //compare AF buff0 and buff1 data:start
                        num = pstRDSData->AFON_Data.AF_Num;
                        indx2 = 0;

                        for (indx = 0; indx < num; indx++) {
                            if ((pstRDSData->AFON_Data.AF[1][indx]) == (pstRDSData->AFON_Data.AF[0][indx])) {
                                if (pstRDSData->AFON_Data.AF[1][indx] != 0)
                                    indx2++;
                            } else
                                pstRDSData->AFON_Data.AF[1][indx] = pstRDSData->AFON_Data.AF[0][indx];
                        }

                        //compare AF buff0 and buff1 data:end
                        if (indx2 == num) {
                            pstRDSData->AFON_Data.Addr_Cnt = 0xFF;
                            pstRDSData->event_status |= RDS_EVENT_AFON_LIST;

                            for (indx = 0; indx < num; indx++) {
                                if ((pstRDSData->AFON_Data.AF[1][indx]) == 0) {
                                    pstRDSData->AFON_Data.Addr_Cnt = 0x0F;
                                    pstRDSData->event_status &= (~RDS_EVENT_AFON_LIST);
                                }
                            }
                        } else
                            pstRDSData->AFON_Data.Addr_Cnt = 0x0F;
                    }
                }
            }
        }
    } else {
        //Type B
        TA_ON = block_data[1] & 0x0008;
        WCN_DBG(FM_DBG | RDSC, "TA g14 typeB pstRDSData->RDSFlag.TP=%d pstRDSData->RDSFlag.TA=%d TP_ON=%d TA_ON=%d\n", pstRDSData->RDSFlag.TP, pstRDSData->RDSFlag.TA, TP_ON, TA_ON);

        if ((!pstRDSData->RDSFlag.TP) && (pstRDSData->RDSFlag.TA) && TP_ON && TA_ON) {
            fm_s32 TA_num = 0;

            for (num = 0; num < 25; num++) {
                if (pstRDSData->AFON_Data.AF[1][num] != 0) {
                    TA_num++;
                } else {
                    break;
                }
            }

            WCN_DBG(FM_NTC | RDSC, "TA set RDS_EVENT_TAON");

            if (TA_num == pstRDSData->AFON_Data.AF_Num) {
                pstRDSData->event_status |= RDS_EVENT_TAON;
            }
        }
    }

    return ret;
}

fm_s32 rds_parser(rds_t *rds_dst, struct rds_rx_t *rds_raw, fm_s32 rds_size, fm_u16(*getfreq)(void))
{
    fm_s32 ret = 0;
    //block_data[0] = blockA,   block_data[1] = blockB, block_data[2] = blockC,   block_data[3] = blockD,
    //block_data[4] = CRC,      block_data[5] = CBC
    fm_u16 block_data[6];
    fm_u8 GroupType, SubType = 0;
    fm_s32 rds_cnt = 0;
    fm_s32 i = 0;
    fm_bool dirty = fm_false;
    //target buf to fill the result in
    fm_u16 *event = &rds_dst->event_status;
    fm_u32 *flag = &rds_dst->RDSFlag.flag_status;

    FMR_ASSERT(getfreq);
    rds_get_freq = getfreq;

    ret = rds_cnt_get(rds_raw, rds_size, &rds_cnt);

    if (ret) {
        WCN_DBG(FM_WAR | RDSC, "get cnt err[ret=%d]\n", ret);
        return ret;
    }

    while (rds_cnt > 0) {
        ret = rds_grp_get(&block_data[0], rds_raw, i);

        if (ret) {
            WCN_DBG(FM_WAR | RDSC, "get group err[ret=%d]\n", ret);
            goto do_next;
        }

        ret = rds_grp_type_get(block_data[4], block_data[1], &GroupType, &SubType);

        if (ret) {
            WCN_DBG(FM_WAR | RDSC, "get group type err[ret=%d]\n", ret);
            goto do_next;
        }

        ret = rds_grp_counter_add(GroupType, SubType, &rds_dst->gc);

        ret = rds_grp_pi_get(block_data[4], block_data[0], &rds_dst->PI, &dirty);

        if (ret) {
            WCN_DBG(FM_WAR | RDSC, "get group pi err[ret=%d]\n", ret);
            goto do_next;
        } else if (dirty == fm_true) {
            ret = rds_event_set(event, RDS_EVENT_PI_CODE); //yes, we got new PI code
        }

        ret = rds_grp_pty_get(block_data[4], block_data[1], &rds_dst->PTY, &dirty);

        if (ret) {
            WCN_DBG(FM_WAR | RDSC, "get group pty err[ret=%d]\n", ret);
            goto do_next;
        } else if (dirty == fm_true) {
            ret = rds_event_set(event, RDS_EVENT_PTY_CODE); // yes, we got new PTY code
        }

        ret = rds_grp_tp_get(block_data[4], block_data[1], &rds_dst->RDSFlag.TP, &dirty);

        if (ret) {
            WCN_DBG(FM_WAR | RDSC, "get group tp err[ret=%d]\n", ret);
            goto do_next;
        } else if (dirty == fm_true) {
            ret = rds_event_set(event, RDS_EVENT_FLAGS); // yes, we got new TP code
            ret = rds_flag_set(flag, RDS_FLAG_IS_TP);
        }

        switch (GroupType) {
        case 0:

            if ((ret = rds_retrieve_g0(&block_data[0], SubType, rds_dst)))
                goto do_next;

            break;
        case 1:

            if ((ret = rds_retrieve_g1(&block_data[0], SubType, rds_dst)))
                goto do_next;

            break;
        case 2:

            if ((ret = rds_retrieve_g2(&block_data[0], SubType, rds_dst)))
                goto do_next;

            break;
        case 4:

            if ((ret = rds_retrieve_g4(&block_data[0], SubType, rds_dst)))
                goto do_next;

            break;
        case 14:

            if ((ret = rds_retrieve_g14(&block_data[0], SubType, rds_dst)))
                goto do_next;

            break;
        default:
            break;
        }

do_next:

        if (ret && (ret != -FM_ECRC)) {
            WCN_DBG(FM_ERR | RDSC, "parsing err[ret=%d]\n", ret);
            return ret;
        }

        rds_cnt--;
        i++;
    }

    return ret;
}


