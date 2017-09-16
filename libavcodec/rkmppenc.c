/*
 * https://users.cs.cf.ac.uk/Dave.Marshall/C/node10.html
 */

#include "avcodec.h"
#include "internal.h"
#include "rockchip/rk_type.h"
#include "rockchip/rk_mpi.h"
#include "rockchip/rk_mpi_cmd.h"
#include "rockchip/mpp_buffer.h"
#include "rockchip/mpp_frame.h"
#include "rockchip/mpp_packet.h"
#include "rockchip/mpp_mem.h"
#include "rockchip/mpp_common.h"
#include "rockchip/mpp_log.h"
#include "rockchip/mpp_err.h"
#include "libavutil/log.h"

//allocate mem --> mpi init --> mpp init --> configure some params --> encode

const enum AVPixelFormat ff_rkmpp_pix_fmts[] = {
    AV_PIX_FMT_NV21,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE //must have this to terminate
};

#define MPI_ENC_IO_COUNT            (1)//one frame each time
#define MAX_FILE_NAME_LENGTH        256

#define MPI_ENC_TEST_SET_IDR_FRAME  0
#define MPI_ENC_TEST_SET_OSD        0

typedef struct {
    // global flow control flag
    RK_U32 frm_eos;
    RK_U32 pkt_eos;
    RK_U32 frame_count;
    RK_U64 stream_size;

    // src and dst
    FILE *fp_input;
    FILE *fp_output;

    // base flow context
    MppCtx ctx;
    MppApi *mpi;
    MppEncPrepCfg prep_cfg;
    MppEncRcCfg rc_cfg;
    MppEncCodecCfg codec_cfg;

    // input / output
    MppBufferGroup frm_grp;
    MppBufferGroup pkt_grp;
    MppFrame  frame;
    MppPacket packet;
    MppBuffer frm_buf[MPI_ENC_IO_COUNT];
    MppBuffer pkt_buf[MPI_ENC_IO_COUNT];
    MppBuffer md_buf[MPI_ENC_IO_COUNT];
    MppBuffer osd_idx_buf[MPI_ENC_IO_COUNT];
    MppEncOSDPlt osd_plt;
    MppEncSeiMode sei_mode;

    // paramter for resource malloc
    RK_U32 width;
    RK_U32 height;
    RK_U32 hor_stride;
    RK_U32 ver_stride;
    MppFrameFormat fmt;
    MppCodingType type;
    RK_U32 num_frames;

    // resources
    size_t frame_size;
    /* NOTE: packet buffer may overflow */
    size_t packet_size;
    /* 32bits for each 16x16 block */
    size_t mdinfo_size;
    /* osd idx size range from 16x16 bytes(pixels) to hor_stride*ver_stride(bytes). for general use, 1/8 Y buffer is enough. */
    size_t osd_idx_size;
    RK_U32 plt_table[8];

    // rate control runtime parameter
    RK_S32 gop;
    RK_S32 fps;
    RK_S32 bps;
    RK_S32 qp_min;
    RK_S32 qp_max;
    RK_S32 qp_step;
    RK_S32 qp_init;
} MpiEncData;

static MppCodingType ffrkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:  return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:  return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_VP8:   return MPP_VIDEO_CodingVP8;
    default:                return MPP_VIDEO_CodingUnused;
    }
}

static MppFrameFormat get_frame_format(AVCodecContext *avctx){
    switch(*(avctx->codec->pix_fmts)){
        case AV_PIX_FMT_NV21:
            return MPP_FMT_YUV420SP;
        case AV_PIX_FMT_YUV420P:
            return MPP_FMT_YUV420P;
        default:
            return 0;
    }
}

static MPP_RET mpp_deinit(MpiEncData *p)
{
    if (p->ctx) {
        mpp_destroy(p->ctx);
        p->ctx = NULL;
    }

    return MPP_OK;
}

static MPP_RET res_deinit(MpiEncData *p)
{
    RK_U32 i;

    mpp_assert(p);

    for (i = 0; i < MPI_ENC_IO_COUNT; i++) {
        if (p->frm_buf[i]) {
            mpp_buffer_put(p->frm_buf[i]);
            p->frm_buf[i] = NULL;
        }

        if (p->pkt_buf[i]) {
            mpp_buffer_put(p->pkt_buf[i]);
            p->pkt_buf[i] = NULL;
        }

        if (p->md_buf[i]) {
            mpp_buffer_put(p->md_buf[i]);
            p->md_buf[i] = NULL;
        }

        if (p->osd_idx_buf[i]) {
            mpp_buffer_put(p->osd_idx_buf[i]);
            p->osd_idx_buf[i] = NULL;
        }
    }

    if (p->frm_grp) {
        mpp_buffer_group_put(p->frm_grp);
        p->frm_grp = NULL;
    }

    if (p->pkt_grp) {
        mpp_buffer_group_put(p->pkt_grp);
        p->pkt_grp = NULL;
    }

    return MPP_OK;
}

static MPP_RET mpi_enc_gen_osd_data(MppEncOSDData *osd_data, MppBuffer osd_buf, RK_U32 frame_cnt)
{
    RK_U32 k = 0, buf_size = 0;
    RK_U8 data = 0;

    osd_data->num_region = 8;
    osd_data->buf = osd_buf;
    for (k = 0; k < osd_data->num_region; k++) {
        osd_data->region[k].enable = 1;
        osd_data->region[k].inverse = frame_cnt & 1;
        osd_data->region[k].start_mb_x = k * 3;
        osd_data->region[k].start_mb_y = k * 2;
        osd_data->region[k].num_mb_x = 2;
        osd_data->region[k].num_mb_y = 2;

        buf_size = osd_data->region[k].num_mb_x * osd_data->region[k].num_mb_y * 256;
        osd_data->region[k].buf_offset = k * buf_size;

        data = k;
        memset((RK_U8 *)mpp_buffer_get_ptr(osd_data->buf) + osd_data->region[k].buf_offset, data, buf_size);
    }

    return MPP_OK;
}

static MPP_RET res_init(AVCodecContext *avctx){
    int i;
    MPP_RET ret = MPP_NOK;
    MpiEncData *p = avctx->priv_data;
    mpp_assert(p);
    av_log(avctx, AV_LOG_INFO, "Get buffer for frm_grp %d\n", p->num_frames);
    ret = mpp_buffer_group_get_internal(&p->frm_grp, MPP_BUFFER_TYPE_ION);//<-- error in here
    av_log(avctx, AV_LOG_INFO, "Can not get buffer for frm_grp (code = %d)\n", ret);
    av_log(avctx, AV_LOG_INFO, "Get buffer for pkt_grp \n");
    mpp_buffer_group_get_internal(&p->pkt_grp, MPP_BUFFER_TYPE_ION);
    av_log(avctx, AV_LOG_INFO, "Get buffer for other components \n");
    for (i = 0; i < MPI_ENC_IO_COUNT; i++) {
        //link frm_buff to frm_grp buffer
        ret = mpp_buffer_get(p->frm_grp, &p->frm_buf[i], p->frame_size);
        if (ret) {
            return ret;
        }

        ret = mpp_buffer_get(p->frm_grp, &p->osd_idx_buf[i], p->osd_idx_size);
        av_log(avctx, AV_LOG_INFO, "failed to get buffer for input frame ret %d\n", ret);
        if (ret) {
            return ret;
        }

        ret = mpp_buffer_get(p->pkt_grp, &p->pkt_buf[i], p->packet_size);
        if (ret) {
            return ret;
        }

        ret = mpp_buffer_get(p->pkt_grp, &p->md_buf[i], p->mdinfo_size);
        if (ret) {
            return ret;
        }
    }
    return MPP_OK;
}

static MPP_RET mpi_enc_gen_osd_plt(MppEncOSDPlt *osd_plt, RK_U32 *table)
{
    RK_U32 k = 0;
    if (osd_plt->buf) {
        for (k = 0; k < 256; k++)
            osd_plt->buf[k] = table[k % 8];
    }
    return MPP_OK;
}

/**
 * 
 * @param avctx
 * @return 
 */
static MPP_RET init_mpp(AVCodecContext *avctx){
    MpiEncData *p = avctx->priv_data;
    MppApi *mpi;
    MppCtx ctx;
    MppEncCodecCfg *codec_cfg;
    MppEncPrepCfg *prep_cfg;
    MppEncRcCfg *rc_cfg;
    MPP_RET ret = MPP_NOK;
    mpi = p->mpi;
    ctx = p->ctx;
    codec_cfg = &p->codec_cfg;
    prep_cfg = &p->prep_cfg;
    rc_cfg = &p->rc_cfg;
    p->fps = 30;
    p->gop = 60;
    p->bps = p->width * p->height / 8 * p->fps;
    p->qp_init  = (p->type == MPP_VIDEO_CodingMJPEG) ? (10) : (26);

    prep_cfg->change        = MPP_ENC_PREP_CFG_CHANGE_INPUT |
                              MPP_ENC_PREP_CFG_CHANGE_ROTATION |
                              MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    prep_cfg->width         = p->width;
    prep_cfg->height        = p->height;
    prep_cfg->hor_stride    = p->hor_stride;
    prep_cfg->ver_stride    = p->ver_stride;
    prep_cfg->format        = p->fmt;
    prep_cfg->rotation      = MPP_ENC_ROT_0;
    ret = mpi->control(ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
    if (ret) {
        return ret;
    }
    rc_cfg->change  = MPP_ENC_RC_CFG_CHANGE_ALL;
    rc_cfg->rc_mode = MPP_ENC_RC_MODE_CBR;
    rc_cfg->quality = MPP_ENC_RC_QUALITY_MEDIUM;
    if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
        /* constant bitrate has very small bps range of 1/16 bps */
        rc_cfg->bps_target   = p->bps;
        rc_cfg->bps_max      = p->bps * 17 / 16;
        rc_cfg->bps_min      = p->bps * 15 / 16;
    } else if (rc_cfg->rc_mode ==  MPP_ENC_RC_MODE_VBR) {
        if (rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
            /* constant QP does not have bps */
            rc_cfg->bps_target   = -1;
            rc_cfg->bps_max      = -1;
            rc_cfg->bps_min      = -1;
        } else {
            /* variable bitrate has large bps range */
            rc_cfg->bps_target   = p->bps;
            rc_cfg->bps_max      = p->bps * 17 / 16;
            rc_cfg->bps_min      = p->bps * 1 / 16;
        }
    }

    /* fix input / output frame rate */
    rc_cfg->fps_in_flex      = 0;
    rc_cfg->fps_in_num       = p->fps;
    rc_cfg->fps_in_denorm    = 1;
    rc_cfg->fps_out_flex     = 0;
    rc_cfg->fps_out_num      = p->fps;
    rc_cfg->fps_out_denorm   = 1;

    rc_cfg->gop              = p->gop;
    rc_cfg->skip_cnt         = 0;
    ret = mpi->control(ctx, MPP_ENC_SET_RC_CFG, rc_cfg);
    if (ret) {
        return ret;
    }
    codec_cfg->coding = p->type;
    switch (codec_cfg->coding) {
    case MPP_VIDEO_CodingAVC : {
        codec_cfg->h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE |
                                 MPP_ENC_H264_CFG_CHANGE_ENTROPY |
                                 MPP_ENC_H264_CFG_CHANGE_TRANS_8x8 |
                                 MPP_ENC_H264_CFG_CHANGE_QP_LIMIT;
        /*
         * H.264 profile_idc parameter
         * 66  - Baseline profile
         * 77  - Main profile
         * 100 - High profile
         */
        codec_cfg->h264.profile  = 100;
        /*
         * H.264 level_idc parameter
         * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
         * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
         * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
         * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
         * 50 / 51 / 52         - 4K@30fps
         */
        codec_cfg->h264.level    = 40;
        codec_cfg->h264.entropy_coding_mode  = 1;
        codec_cfg->h264.cabac_init_idc  = 0;
        codec_cfg->h264.transform8x8_mode = 1;

        if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_CBR) {
            /* constant bitrate do not limit qp range */
            p->qp_max   = 48;
            p->qp_min   = 4;
            p->qp_step  = 16;
            p->qp_init  = 0;
        } else if (rc_cfg->rc_mode == MPP_ENC_RC_MODE_VBR) {
            if (rc_cfg->quality == MPP_ENC_RC_QUALITY_CQP) {
                /* constant QP mode qp is fixed */
                p->qp_max   = p->qp_init;
                p->qp_min   = p->qp_init;
                p->qp_step  = 0;
            } else {
                /* variable bitrate has qp min limit */
                p->qp_max   = 40;
                p->qp_min   = 12;
                p->qp_step  = 8;
                p->qp_init  = 0;
            }
        }

        codec_cfg->h264.qp_max      = p->qp_max;
        codec_cfg->h264.qp_min      = p->qp_min;
        codec_cfg->h264.qp_max_step = p->qp_step;
        codec_cfg->h264.qp_init     = p->qp_init;
    } break;
    case MPP_VIDEO_CodingMJPEG : {
        codec_cfg->jpeg.change  = MPP_ENC_JPEG_CFG_CHANGE_QP;
        codec_cfg->jpeg.quant   = p->qp_init;
    } break;
    case MPP_VIDEO_CodingVP8 :
    case MPP_VIDEO_CodingHEVC :
    default : {
        mpp_err_f("support encoder coding type %d\n", codec_cfg->coding);
    } break;
    }
    ret = mpi->control(ctx, MPP_ENC_SET_CODEC_CFG, codec_cfg);
    if (ret) {
        return ret;
    }

    /* optional */
    p->sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
    ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &p->sei_mode);
    if (ret) {
        return ret;
    }

    /* gen and cfg osd plt */
    mpi_enc_gen_osd_plt(&p->osd_plt, p->plt_table);
    return MPP_OK;
}

static MPP_RET mpi_init(AVCodecContext *avctx){
    MPP_RET ret = MPP_NOK;
    MpiEncData *p = avctx->priv_data;
    ret = mpp_create(&p->ctx, &p->mpi);
    if (ret) {
        return ret;
    }

    ret = mpp_init(p->ctx, MPP_CTX_ENC, p->type);
    if (ret){
        return ret;
    }
    return MPP_OK;
}


/**
 * ignore this, because FFMPEG helps us create file' headers
 * @param avctx
 * @return 
 */
static int prepare(AVCodecContext *avctx){
    MPP_RET ret;
    MppApi *mpi;
    MppCtx ctx;
    MpiEncData *p = avctx->priv_data;
    MppPacket packet = NULL;
    mpi = p->mpi;
    ctx = p->ctx;
    ret = mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &packet);
    if (ret) {
        return ret;
    }
    if (packet) {
        void *ptr   = mpp_packet_get_pos(packet); //get position of packet
        size_t len  = mpp_packet_get_length(packet);//get valid data length of packet

        if (p->fp_output)
            //write data is pointed by ptr, has size is 1 byte, we have len item, to file
            //we write global info to file
            //see more: https://stackoverflow.com/questions/17541153/how-to-find-sps-and-pps-string-in-h264-codec-from-mp4
            fwrite(ptr, 1, len, p->fp_output);

        packet = NULL;
    }
}

/**
 * Init something before starting
 * @param avctx
 * @return 
 */
static av_cold int encode_init(AVCodecContext *avctx){
    MpiEncData *p = avctx->priv_data;//hold data for global using
    MPP_RET ret = MPP_NOK;
    p = mpp_calloc(MpiEncData, 1);
    if(!p){
        return ret;
    }
    av_log(avctx, AV_LOG_INFO, "Start initing rockchip\n");
    p->width = avctx->width;
    p->height = avctx->height;
    p->hor_stride   = MPP_ALIGN(avctx->width, 16);
    p->ver_stride   = MPP_ALIGN(avctx->height, 16);
    p->fmt          = get_frame_format(avctx);
    p->type         = ffrkmpp_get_codingtype(avctx);
    p->num_frames   = avctx->frame_number;
    p->frame_size   = p->hor_stride * p->ver_stride * 3 / 2;
    p->packet_size  = p->width * p->height;
    p->mdinfo_size  = (((p->hor_stride + 255) & (~255)) / 16) * (p->ver_stride / 16) * 4;
    /*
     * osd idx size range from 16x16 bytes(pixels) to hor_stride*ver_stride(bytes).
     * for general use, 1/8 Y buffer is enough.
     */
    p->osd_idx_size  = p->hor_stride * p->ver_stride / 8;
    p->plt_table[0] = MPP_ENC_OSD_PLT_WHITE;
    p->plt_table[1] = MPP_ENC_OSD_PLT_YELLOW;
    p->plt_table[2] = MPP_ENC_OSD_PLT_CYAN;
    p->plt_table[3] = MPP_ENC_OSD_PLT_GREEN;
    p->plt_table[4] = MPP_ENC_OSD_PLT_TRANS;
    p->plt_table[5] = MPP_ENC_OSD_PLT_RED;
    p->plt_table[6] = MPP_ENC_OSD_PLT_BLUE;
    p->plt_table[7] = MPP_ENC_OSD_PLT_BLACK;
    av_log(avctx, AV_LOG_INFO, "Start initing rockchip's resources %d\n", p->num_frames);
    mpp_assert(p);
    res_init(avctx);
    av_log(avctx, AV_LOG_INFO, "Finish initing rockchip's resources \n");
    av_log(avctx, AV_LOG_INFO, "Start initing rockchip's mpi \n");    
    mpi_init(avctx);
    av_log(avctx, AV_LOG_INFO, "Finish initing rockchip's mpi \n");
    av_log(avctx, AV_LOG_INFO, "Start initing rockchip's mpp \n");
    init_mpp(avctx);
    
    av_log(avctx, AV_LOG_INFO, "Finish initing rockchip\n");
    return 0;
}

static av_cold int encode_close(AVCodecContext *avctx){
    MPP_RET ret = MPP_NOK;
    MpiEncData *p = avctx->priv_data;
    if (p->frame) {
        mpp_frame_deinit(&p->frame);
        p->frame = NULL;
    }
    ret = p->mpi->reset(p->ctx);
    if (ret) {        
        goto MPP_TEST_OUT;
    }
    return 0;
    MPP_TEST_OUT:
        mpp_deinit(p);
        res_deinit(p);
        return ret;
}




/**
 * Encode data
 * @param avctx
 * @param pkt output encode packet
 * @param frame input frame to encode
 * @param got_packet 1 - we got packet, 0 - no packet to get
 * @return 
 */
static int encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                          const AVFrame *frame, int *got_packet){
    MPP_RET ret;
    MppPacket packet = NULL;
    MppApi *mpi;
    MppCtx ctx;
    //encode packet
    MppTask task = NULL;
    MpiEncData *p = avctx->priv_data;
    mpi = p->mpi;
    ctx = p->ctx;
    //get frame
    ret = mpp_frame_init(&p->frame);
    if (ret) {
        return ret;
    }
    mpp_frame_set_width(p->frame, p->width);
    mpp_frame_set_height(p->frame, p->height);
    mpp_frame_set_hor_stride(p->frame, p->hor_stride);
    mpp_frame_set_ver_stride(p->frame, p->ver_stride);
    mpp_frame_set_fmt(p->frame, p->fmt);
    
    
    MppBuffer frm_buf_in  = p->frm_buf[0];
    MppBuffer pkt_buf_out = p->pkt_buf[0];
    MppBuffer md_info_buf = p->md_buf[0];
    MppBuffer osd_data_buf = p->osd_idx_buf[0];
    MppEncOSDData osd_data;
    void *buf = mpp_buffer_get_ptr(frm_buf_in);
    buf = frame->data;//get frame data
    mpp_frame_set_buffer(p->frame, frm_buf_in);
    mpp_frame_set_eos(p->frame, p->frm_eos);
    av_log(avctx, AV_LOG_INFO, "Init packet \n");//<-- stop here
    mpp_assert(pkt_buf_out);
    mpp_packet_init_with_buffer(&packet, pkt_buf_out);
    ret = mpi->poll(ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret) {
        return ret;
    }
    ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &task);
    if (ret || NULL == task) {
        return ret;
    }
    mpp_task_meta_set_frame (task, KEY_INPUT_FRAME,  p->frame);
    mpp_task_meta_set_packet(task, KEY_OUTPUT_PACKET, packet);
    mpp_task_meta_set_buffer(task, KEY_MOTION_INFO, md_info_buf);
    mpi_enc_gen_osd_data(&osd_data, osd_data_buf, p->frame_count);
    ret = mpi->enqueue(ctx, MPP_PORT_INPUT, task);
    if (ret) {
        return ret;
    }
    ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret) {
        return ret;
    }
    ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &task);
    if (ret || NULL == task) {
        return ret;
    }
    if (task) {
        MppFrame packet_out = NULL;
        mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &packet_out);
        mpp_assert(packet_out == packet);
        if (packet) {
            void *ptr   = mpp_packet_get_pos(packet);
            void *ptr_cpy;
            size_t len  = mpp_packet_get_length(packet);
            //we must copy data to onther mem before release it
            memcpy(ptr_cpy, ptr, len);
            pkt->data = ptr_cpy;
            p->pkt_eos = mpp_packet_get_eos(packet);
            mpp_packet_deinit(&packet);
            
            //get packet
            *got_packet = 1;
        }else{
            *got_packet = 0;
        }
        ret = mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
        if (ret) {
            return ret;
        }
    }
    return 0;
}


AVCodec ff_h264_rkmpp_encoder = {
    .name = "h264_rkmpp", // <-- nothing
    .long_name = "RKMPP chiprock encoder",
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .pix_fmts = ff_rkmpp_pix_fmts,
    .init = encode_init,
    .close = encode_close,
    .encode2 = encode_frame,
    .priv_data_size = sizeof(MpiEncData),//size of private data, if not use we can not cast priv_data to our pointer
};
