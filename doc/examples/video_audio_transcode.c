/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   video_audio_transcode.c
 * Author: ducla
 *
 * Created on November 4, 2019, 10:19 AM
 */

#include <stdio.h>
#include <stdlib.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avassert.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"

static void logging(const char *fmt, ...);
static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context);
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context);
static void init_packet(AVPacket *packet);
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size);
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size);
static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present);
static int convert_samples(const uint8_t **input_data,
                           uint8_t **converted_data, const int frame_size,
                           SwrContext *resample_context);
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size);
static int init_input_frame(AVFrame **frame);
static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished);

/*
 * 
 */
int main(int argc, char** argv) {
    const char      *in_filename, *out_filename;
    AVFormatContext *i_avfctx = NULL;
    AVFormatContext *o_avfctx = NULL;
    //for input decode
    AVCodecContext  *ia_avctx = NULL;
    AVCodecContext  *iv_avctx = NULL;
    //for output encode
    AVCodecContext  *oa_avctx = NULL;
    AVCodecContext  *ov_avctx = NULL;
    
    AVCodec         *ia_avc;
    AVCodec         *iv_avc;
    AVCodec         *oa_avc;    
    AVCodec         *ov_avc;
        
    
    int             *streams_list = NULL;
    int             number_of_streams = 0;
    
    int             i;
    int             video_stream_index = -1;
    int             audio_stream_index = -1;
    int             stream_index = 0;
    int             ret;
    
    AVPacket        in_packet;
        
    
    if (argc < 3) {
        printf("You need to pass at least two parameters.\n");
        return -1;
    }
    
    in_filename = argv[1];
    out_filename = argv[2];    
    ret = avformat_open_input(&i_avfctx, in_filename, NULL, NULL);
    if(ret < 0){
        logging("Can not open input format");
        goto end;
    }
    ret = avformat_find_stream_info(i_avfctx, NULL);
    if(ret < 0){
        logging("Can not find input stream info.");
        goto end;
    }
    ret = avformat_alloc_output_context2(&o_avfctx, NULL, NULL, out_filename);
    if(ret < 0){
        logging("Can not allocate output format context %s", av_err2str(ret));
        goto end;
    }
    number_of_streams = i_avfctx->nb_streams;
    streams_list = av_mallocz_array(number_of_streams, sizeof (*streams_list));
    if (!streams_list) {
        ret = AVERROR(ENOMEM);
        logging("Can not allocate streamlist");
        goto end;
    }
    for(i = 0; i < i_avfctx->nb_streams; i++){
        AVStream *out_stream;
        AVStream *in_stream = i_avfctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;
        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {     
            streams_list[i] = -1;
            continue;
        }
        streams_list[i] = stream_index++;
        out_stream = avformat_new_stream(o_avfctx, NULL);
        if (!out_stream) {
            logging("Failed allocating output stream");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        if (in_codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
            if (audio_stream_index == -1) {
                audio_stream_index = i;
            }
             //prepare for sound processing 
            //input audio
            ia_avc = avcodec_find_decoder(in_codecpar->codec_id);
            if(!ia_avc){
                logging("Can not find input audio decoder");
                goto end;
            }
            ia_avctx = avcodec_alloc_context3(ia_avc);
            if(!ia_avctx){
                logging("Can not allocate input audio codec");
                goto end;
            }
            ret = avcodec_parameters_to_context(ia_avctx, in_codecpar);
            if(ret < 0){
                logging("Can not set params for context");
                goto end;
            }
            ret = avcodec_open2(ia_avctx, ia_avc, NULL);
            if(ret < 0){
                logging("Can not open input audio context");
                goto end;
            }
            //output audio
            oa_avc = avcodec_find_encoder(AV_CODEC_ID_AAC);
            if(!oa_avc){
                logging("Can not find output audio encoder");
                goto end;
            }
            oa_avctx = avcodec_alloc_context3(oa_avc);
            if(!oa_avctx){
                logging("Can not allocate for output audio context");
                goto end;
            }                        
            oa_avctx->channel_layout = in_codecpar->channel_layout;
            oa_avctx->channels = av_get_channel_layout_nb_channels(oa_avctx->channels);
            oa_avctx->sample_rate    = ia_avctx->sample_rate;
            oa_avctx->sample_fmt     = oa_avc->sample_fmts[0];                   
//            oa_avctx->time_base = (AVRational){1, 24};
            oa_avctx->time_base.den = ia_avctx->time_base.den;       
            oa_avctx->time_base.num = ia_avctx->time_base.num;
            ret = avcodec_open2(oa_avctx, oa_avc, NULL);
            if(ret < 0){
                logging("Can not open output audio codec context");
                goto end;
            }
            ret = avcodec_parameters_from_context(out_stream->codecpar, oa_avctx);
            if(ret < 0){
                logging("Could not initialize stream parameters");
                goto end;
            }            
        }else if(in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
            if (video_stream_index == -1) {
                video_stream_index = i;
            }
            //video input codec context
            iv_avc = avcodec_find_decoder(in_codecpar->codec_id);
            if(!iv_avc){
                logging("Can not find video decoder");
                goto end;
            }
            iv_avctx = avcodec_alloc_context3(iv_avc);
            if(!iv_avctx){
                logging("Can not allocate input video context");
                goto end;
            }
            ret = avcodec_open2(iv_avctx, iv_avc, NULL);
            if(ret < 0){
                logging("Can not open input video codec");
                goto end;
            }
            
            //video output codec context
            ov_avc = avcodec_find_encoder(AV_CODEC_ID_H264);
            if(!ov_avc){
                logging("Can not find output video encoder");
                goto end;
            }            
            ov_avctx = avcodec_alloc_context3(ov_avc);
            if(!ov_avctx){
                logging("Can not allocate output video context");
                goto end;
            }
            ov_avctx->bit_rate = in_codecpar->bit_rate;
            ov_avctx->width = in_codecpar->width;
            ov_avctx->height = in_codecpar->height;            
            ov_avctx->time_base = (AVRational){1, 24};       
            ov_avctx->gop_size = 10;
            ov_avctx->max_b_frames = 1;
            ov_avctx->pix_fmt = AV_PIX_FMT_YUV420P;
            ret = av_opt_set(ov_avctx->priv_data, "preset", "slow", 0); 
            ret = avcodec_open2(ov_avctx, ov_avc, NULL);
            if(ret < 0){
                logging("Can not open output video codec context");
                goto end;
            }
            //add info for output stream
            ret = avcodec_parameters_from_context(out_stream->codecpar, ov_avctx);
            if(ret < 0){
                logging("Can not create param for output video");
                goto end;
            }
        }
    }
    av_dump_format(i_avfctx, 0, in_filename, 0);

    av_dump_format(o_avfctx, 0, out_filename, 1);
    if (!(o_avfctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&o_avfctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            logging("Could not open output file '%s'", out_filename);
            goto end;
        }
    }
    AVOutputFormat *avof;
    //we get ff_hls_muxer;
    avof = av_guess_format("hls", NULL, NULL);
    if (!avof) {
        logging("No output format\n");
        ret = AVERROR_MUXER_NOT_FOUND;
        return ret;
    }
    o_avfctx->oformat = avof;
    // https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga18b7b10bb5b94c4842de18166bc677cb      
    //this option can be found in this link https://ffmpeg.org/ffmpeg-formats.html#Options-5
    ret = av_opt_set(o_avfctx->priv_data, "hls_segment_type", "fmp4", 0);            
    
    ret = avformat_write_header(o_avfctx, NULL);
    if(ret < 0){
        logging("Can not write header");
        goto end;
    }    
    AVFrame         *i_frame;
    i_frame = av_frame_alloc();
    if(!i_frame){
        logging("Can not allocate frame");
        goto end;
    }  
    AVPacket        *out_packet;
    out_packet = av_packet_alloc();
    if(!out_packet){
        logging("Can not allocate packet");
        goto end;
    }    
    //process data
    while (1) {
        AVStream        *in_stream, *out_stream;                
//        const int       output_frame_size = oa_avctx->frame_size;
        
                      
        ret = av_read_frame(i_avfctx, &in_packet);
        if(ret != 0){
            logging("Can not read more frame");
            break;
        } 
        
        in_stream = i_avfctx->streams[in_packet.stream_index];
        if (in_packet.stream_index >= number_of_streams || streams_list[in_packet.stream_index] < 0) {                        
        }
        in_packet.stream_index = streams_list[in_packet.stream_index];
        out_stream = o_avfctx->streams[in_packet.stream_index];
        if (in_packet.stream_index == video_stream_index) {    
//            av_packet_rescale_ts(&in_packet, in_stream->time_base, iv_avctx->time_base); 
            ret = avcodec_send_packet(iv_avctx, &in_packet);
            if(ret < 0){                
                goto end;
            }else{
                ret = avcodec_receive_frame(iv_avctx, i_frame);
                if(ret < 0){
                    goto end;
                }else{                                        
                    ret = avcodec_send_frame(ov_avctx, i_frame);                    
                    if(ret < 0){
                        goto end;
                    }else{                        
                        ret = avcodec_receive_packet(ov_avctx, out_packet); 
                        if(ret < 0){
                            if(ret == AVERROR(EAGAIN)){
//                                av_packet_unref(out_packet);
                                continue;
                            }else{
//                                av_packet_unref(out_packet);
                                goto end;
                            }
                        }else{                                                      
                            av_packet_rescale_ts(out_packet, in_stream->time_base, out_stream->time_base);    
                                                        
                            logging("v%d", out_packet->dts);
                            out_packet->stream_index = video_stream_index;//this packet is belong to what stream?
                            ret = av_interleaved_write_frame(o_avfctx, out_packet);
                            if(ret < 0){
                                logging("293-%s", av_err2str(ret));
                                av_packet_unref(out_packet);
                                break;
                            }
                            av_packet_unref(out_packet);
                        }
                    }
                }
            }
        }else if(in_packet.stream_index == audio_stream_index){         
//            av_packet_rescale_ts(&in_packet, in_stream->time_base, ia_avctx->time_base);
            ret = avcodec_send_packet(ia_avctx, &in_packet);
            if(ret < 0){                
                goto clean_up;
            }else{
                //because of one audio packet maybe contains multi audio frames
                while(ret >= 0){
                    ret = avcodec_receive_frame(ia_avctx, i_frame);
                    if(ret < 0){                    
                        if(ret == AVERROR(EAGAIN)){
                            break;
                        }else{
                            goto end;
                        }                    
                    }else{                    
                        ret = avcodec_send_frame(oa_avctx, i_frame);                    
                        if(ret < 0){
                            logging("315-%s", av_err2str(ret));
                            goto end;
                        }else{                        
                            ret = avcodec_receive_packet(oa_avctx, out_packet);         
                            if(ret < 0){
                                if(ret == AVERROR(EAGAIN)){
    //                                av_packet_unref(out_packet);
                                    break;
                                }else{
                                    logging("320-%s", av_err2str(ret));
                                    goto end;
                                }                            
                            }else{
    //                            logging("Write audio packet %s", av_err2str(ret));
                                logging("a1-%d-%d-%d", in_packet.dts, in_stream->time_base.num, in_stream->time_base.den);                            
                                av_packet_rescale_ts(out_packet, in_stream->time_base, out_stream->time_base);            
                                logging("a2-%d-%d-%d", out_packet->dts, out_stream->time_base.num, out_stream->time_base.den);
    //                            logging("a%d", out_packet->dts);
                                out_packet->stream_index = audio_stream_index;
                                out_packet->pos = -1;
                                ret = av_interleaved_write_frame(o_avfctx, out_packet);
                                if(ret < 0){
                                    logging("325-%s", av_err2str(ret));
                                    av_packet_unref(out_packet);
                                    break;
                                }
                                av_packet_unref(out_packet);
                            }
                        }
                    }
                }                
            }
        }      
        clean_up:
        in_packet.pos = -1;
//        av_frame_unref(i_frame);
        av_packet_unref(&in_packet);
//        av_packet_unref(&out_packet);
    }
    av_write_trailer(o_avfctx);   
    
    end:
    if(!i_avfctx){
        avformat_free_context(i_avfctx);
    }
    if(!o_avfctx){
        avformat_free_context(o_avfctx);
    }
    if(!oa_avctx){
        avcodec_free_context(&oa_avctx);
    }
    if(!ia_avctx){
        avcodec_free_context(&ia_avctx);
    }
    if(!iv_avctx){
        avcodec_free_context(&iv_avctx);
    }
    if(!ov_avctx){
        avcodec_free_context(&ov_avctx);
    }
    avformat_close_input(&i_avfctx);    
    if(!i_frame){
        av_frame_free(&i_frame);
    }
    if(!out_packet){
        av_packet_free(&out_packet);
    }
    if (o_avfctx && !(o_avfctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&o_avfctx->pb);
    }
    return ret;
}

static void logging(const char *fmt, ...) {
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context)
{
        int error;

        /*
         * Create a resampler context for the conversion.
         * Set the conversion parameters.
         * Default channel layouts based on the number of channels
         * are assumed for simplicity (they are sometimes not detected
         * properly by the demuxer and/or decoder).
         */
        *resample_context = swr_alloc_set_opts(NULL,
                                              av_get_default_channel_layout(output_codec_context->channels),
                                              output_codec_context->sample_fmt,
                                              output_codec_context->sample_rate,
                                              av_get_default_channel_layout(input_codec_context->channels),
                                              input_codec_context->sample_fmt,
                                              input_codec_context->sample_rate,
                                              0, NULL);
        if (!*resample_context) {
            fprintf(stderr, "Could not allocate resample context\n");
            return AVERROR(ENOMEM);
        }
        /*
        * Perform a sanity check so that the number of converted samples is
        * not greater than the number of samples to be converted.
        * If the sample rates differ, this case has to be handled differently
        */
        av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);

        /* Open the resampler with the specified parameters. */
        if ((error = swr_init(*resample_context)) < 0) {
            fprintf(stderr, "Could not open resample context\n");
            swr_free(resample_context);
            return error;
        }
    return 0;
}

static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /* Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        fprintf(stderr, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resampler_context,
                                         int *finished)
{
    /* Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    /* Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int data_present = 0;
    int ret = AVERROR_EXIT;

    /* Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame))
        goto cleanup;
    /* Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, input_format_context,
                           input_codec_context, &data_present, finished))
        goto cleanup;
    /* If we are at the end of the file and there are no more samples
     * in the decoder which are delayed, we are actually finished.
     * This must not be treated as an error. */
    if (*finished) {
        ret = 0;
        goto cleanup;
    }
    /* If there is decoded data, convert and store it. */
    if (data_present) {
        /* Initialize the temporary storage for the converted input samples. */
        if (init_converted_samples(&converted_input_samples, output_codec_context,
                                   input_frame->nb_samples))
            goto cleanup;

        /* Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples. */
        if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,
                            input_frame->nb_samples, resampler_context))
            goto cleanup;

        /* Add the converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, converted_input_samples,
                                input_frame->nb_samples))
            goto cleanup;
        ret = 0;
    }
    ret = 0;

cleanup:
    if (converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }
    av_frame_free(&input_frame);

    return ret;
}

static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate input frame\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished)
{
    /* Packet used for temporary storage. */
    AVPacket input_packet;
    int error;
    init_packet(&input_packet);

    /* Read one audio frame from the input file into a temporary packet. */
    if ((error = av_read_frame(input_format_context, &input_packet)) < 0) {
        /* If we are at the end of the file, flush the decoder below. */
        if (error == AVERROR_EOF)
            *finished = 1;
        else {
            fprintf(stderr, "Could not read frame (error '%s')\n",
                    av_err2str(error));
            return error;
        }
    }

    /* Send the audio frame stored in the temporary packet to the decoder.
     * The input audio stream decoder is used to do this. */
    if ((error = avcodec_send_packet(input_codec_context, &input_packet)) < 0) {
        fprintf(stderr, "Could not send packet for decoding (error '%s')\n",
                av_err2str(error));
        return error;
    }

    /* Receive one frame from the decoder. */
    error = avcodec_receive_frame(input_codec_context, frame);
    /* If the decoder asks for more data to be able to decode a frame,
     * return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
    /* If the end of the input file is reached, stop decoding. */
    } else if (error == AVERROR_EOF) {
        *finished = 1;
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not decode frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    /* Default case: Return decoded data. */
    } else {
        *data_present = 1;
        goto cleanup;
    }

cleanup:
    av_packet_unref(&input_packet);
    return error;
}

static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    /* Set the packet data and size so that it is recognized as being empty. */
    packet->data = NULL;
    packet->size = 0;
}

static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    int error;

    /* Allocate as many pointers as there are audio channels.
     * Each pointer will later point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     */
    if (!(*converted_input_samples = calloc(output_codec_context->channels,
                                            sizeof(**converted_input_samples)))) {
        fprintf(stderr, "Could not allocate converted input sample pointers\n");
        return AVERROR(ENOMEM);
    }

    /* Allocate memory for the samples of all channels in one consecutive
     * block for convenience. */
    if ((error = av_samples_alloc(*converted_input_samples, NULL,
                                  output_codec_context->channels,
                                  frame_size,
                                  output_codec_context->sample_fmt, 0)) < 0) {
        fprintf(stderr,
                "Could not allocate converted input samples (error '%s')\n",
                av_err2str(error));
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return error;
    }
    return 0;
}

static int convert_samples(const uint8_t **input_data,
                           uint8_t **converted_data, const int frame_size,
                           SwrContext *resample_context)
{
    int error;

    /* Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, frame_size,
                             input_data    , frame_size)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                av_err2str(error));
        return error;
    }

    return 0;
}

static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /* Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples. */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO\n");
        return error;
    }

    /* Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    /* Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /* Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame size. */
    const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                 output_codec_context->frame_size);
    int data_written;

    /* Initialize temporary storage for one output frame. */
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
        return AVERROR_EXIT;

    /* Read as many samples from the FIFO buffer as required to fill the frame.
     * The samples are stored in the frame temporarily. */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        fprintf(stderr, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /* Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    /* Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
                av_err2str(error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}
static int64_t pts = 0;

static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    /* Packet used for temporary storage. */
    AVPacket output_packet;
    int error;
    init_packet(&output_packet);

    /* Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }

    /* Send the audio frame stored in the temporary packet to the encoder.
     * The output audio stream encoder is used to do this. */
    error = avcodec_send_frame(output_codec_context, frame);
    /* The encoder signals that it has nothing more to encode. */
    if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not send packet for encoding (error '%s')\n",
                av_err2str(error));
        return error;
    }

    /* Receive one encoded frame from the encoder. */
    error = avcodec_receive_packet(output_codec_context, &output_packet);
    /* If the encoder asks for more data to be able to provide an
     * encoded frame, return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
    /* If the last frame has been encoded, stop encoding. */
    } else if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not encode frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    /* Default case: Return encoded data. */
    } else {
        *data_present = 1;
    }

    /* Write one audio frame from the temporary packet to the output file. */
    if (*data_present &&
        (error = av_write_frame(output_format_context, &output_packet)) < 0) {
        fprintf(stderr, "Could not write frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

cleanup:
    av_packet_unref(&output_packet);
    return error;
}