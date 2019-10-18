/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   1_remux.c
 * Author: ducla
 *
 * Created on October 17, 2019, 11:47 PM
 */

#include <stdio.h>
#include <stdlib.h>

#include "libavformat/avformat.h"

/*
 * 
 */
int main(int argc, char** argv) {
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
    AVPacket packet;
    const char *in_filename, *out_filename;
    int ret, i;
    int stream_index = 0;
    int *stream_list = NULL;
    int number_of_streams = 0;
    int fragmented_mp4_options = 0;
    if(argc < 3){
        printf("You need to pass at least two parameters.\n");
        return -1;
    }
    in_filename = argv[1];
    out_filename = argv[2];
    if((ret = avformat_open_input(&input_format_context, in_filename, NULL, NULL)) < 0){
        fprintf(stderr, "Could not open input file '%s'", in_filename);
        goto end;
    }
    if((ret = avformat_find_stream_info(input_format_context, NULL)) < 0){
        fprintf(stderr, "Failed to retrieve input stream information.\n");
        goto end;
    }
    avformat_alloc_output_context2(&output_format_context, NULL, NULL, out_filename);
    if(!output_format_context){
        fprintf(stderr, "Could not create output context\n");
        goto end;
    }
    number_of_streams = input_format_context->nb_streams;
    stream_list = av_mallocz_array(number_of_streams, sizeof(*stream_list));
    if(!stream_list){
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for(i = 0; i < input_format_context->nb_streams; i++){
        AVStream *out_stream;
        AVStream *in_stream = input_format_context->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;
        if(in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
           in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE
            ){
            stream_list[i] = -1;
            continue;
        }
        stream_list[i] = stream_index++;
        out_stream = avformat_new_stream(output_format_context, NULL);
        if(!out_stream){
            fprintf(stderr, "Failed allocating output stream.\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if(ret < 0){
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
    }
    av_dump_format(output_format_context, 0, out_filename, 1);
    if(!(output_format_context->oformat->flags && AVFMT_NOFILE)){
        ret = avio_open(&output_format_context->pb, out_filename, AVIO_FLAG_WRITE);
        if(ret < 0){
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
        }
    }
    AVDictionary *opts = NULL;
    if(fragmented_mp4_options){
        av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    }
    ret = avformat_write_header(output_format_context, &opts);
    if(ret < 0){
        fprintf(stderr, "Error occured when opening output file.\n");
        goto end;
    }
    while(1){
        AVStream *in_stream, *out_stream;
        ret = av_read_frame(input_format_context, &packet);
        if(ret < 0){
            break;
        }
        in_stream = input_format_context->streams[packet.stream_index];
        if(packet.stream_index >= number_of_streams || stream_list[packet.stream_index] < 0){
            av_packet_unref(&packet);
            continue;
        }
        packet.stream_index = stream_list[packet.stream_index];
        out_stream = output_format_context->streams[packet.stream_index];
        /* copy packet */
        packet.pts = av_rescale_q_rnd(packet.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        packet.dts = av_rescale_q_rnd(packet.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        packet.duration = av_rescale_q(packet.duration, in_stream->time_base, out_stream->time_base);
        packet.pos = -1;
        ret = av_interleaved_write_frame(output_format_context, &packet);
        if(ret < 0){
            fprintf(stderr, "Error muxing packet.\n");
            break;
        }
        av_packet_unref(&packet);
    }
    av_write_trailer(output_format_context);
    end:
    avformat_close_input(&input_format_context);
    if(output_format_context && !(output_format_context->oformat->flags & AVFMT_NOFILE)){
        avio_closep(&output_format_context->pb);        
    }
    avformat_free_context(output_format_context);
    av_freep(&stream_list);
    if(ret < 0 && ret != AVERROR_EOF){
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }
    return (EXIT_SUCCESS);
}

