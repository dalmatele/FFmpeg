/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   hello_world.c
 * Author: ducla
 *
 * Created on October 17, 2019, 4:02 PM
 */

#include <stdio.h>
#include <stdlib.h>

#include "libavformat/avformat.h"

static void logging(const char *fmt, ...);
static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame);
static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename);

/*
 * 
 */
int main(int argc, char** argv) {
    if(argc < 2){
        printf("You need to specify a media file.\n");
        return -1;
    }
    AVFormatContext *pFormatContext = avformat_alloc_context();
    if(!pFormatContext){
        logging("Error could not allocate memory for Format Context");
        return -1;
    }
    logging("Initializing all the containers, codecs and protocols.");
    if(avformat_open_input(&pFormatContext, argv[1], NULL, NULL) != 0){
        logging("Error could not open the file");
        return -1;
    }
    logging("Format %s, duration %lld us, bit_rate %lld", pFormatContext->iformat->name, pFormatContext->duration, pFormatContext->bit_rate);
    logging("finding stream info from format");
    if(avformat_find_stream_info(pFormatContext, NULL) < 0){
        logging("Error could not get the stream info");
        return -1;
    }
    AVCodec *pCodec = NULL;
    AVCodecParameters *pCodecParameters = NULL;
    int video_stream_index = -1;
    for(int i = 0; i < pFormatContext->nb_streams; i++){
        AVCodecParameters *pLocalCodecParameters = NULL;
        pLocalCodecParameters = pFormatContext->streams[i]->codecpar;
        logging("AVStream->time_base before open coded %d/%d", pFormatContext->streams[i]->time_base.num, pFormatContext->streams[i]->time_base.den);
        logging("AVStream->r_frame_rate before open coded %d/%d", pFormatContext->streams[i]->r_frame_rate.num, pFormatContext->streams[i]->r_frame_rate.den);
        logging("AVStream->start_time %" PRId64, pFormatContext->streams[i]->start_time);
        logging("AVStream->duration %" PRId64, pFormatContext->streams[i]->duration);
        AVCodec *pLocalCodec = NULL;
        pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);
        if(pLocalCodec == NULL){
            logging("Error unsupported codec!");
            return -1;
        }
        if(pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO){
            if(video_stream_index == -1){
                video_stream_index = i;
                pCodec = pLocalCodec;
                pCodecParameters = pLocalCodecParameters;
            }
            logging("Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
        }else if(pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO){
            logging("Audio Codec: %d channels, sample rate %d", pLocalCodecParameters->channels, pLocalCodecParameters->sample_rate);
        }
         logging("\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pCodecParameters->bit_rate);
    }
    AVCodecContext *pCodecContext = avcodec_alloc_context3(pCodec);
    if(!pCodecContext){
        logging("failed to allocated memory for AVCodecContext");
        return -1;
    }
    if(avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0){
        logging("failed to copy codec params to codec context");
        return -1;
    }
    
    if(avcodec_open2(pCodecContext, pCodec, NULL) < 0){
        logging("failed to open codec through avcodec_open2");
        return -1;
    }
    AVFrame *pFrame = av_frame_alloc();
    if(!pFrame){
        logging("failed to allocated memory for AVFrame");
        return -1;
    }
    AVPacket *pPacket = av_packet_alloc();
    if(!pPacket){
        logging("failed to allocated memory for AVPacket");
        return -1;
    }
    int response = 0;
    int how_many_packes_to_process = 8;
    while(av_read_frame(pFormatContext, pPacket) >= 0){
        if(pPacket->stream_index == video_stream_index){
            logging("AVPacket->pts %s" PRId64, pPacket->pts);
            response = decode_packet(pPacket, pCodecContext, pFrame);            
            if(response < 0){
                break;
            }
            if(--how_many_packes_to_process <= 0){
                break;
            }
        }
        av_packet_unref(pPacket);
    }    
    avformat_close_input(&pFormatContext);
    avformat_free_context(pFormatContext);
    av_packet_free(&pPacket);
    av_frame_free(&pFrame);
    avcodec_free_context(&pCodecContext);
    return 0;
}

//https://www.techonthenet.com/c_language/standard_library_functions/stdarg_h/va_arg.php
//https://vietjack.com/thu-vien-c/stdarg-h-trong-c.jsp
static void logging(const char *fmt, ...){
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static int decode_packet(AVPacket* pPacket, AVCodecContext* pCodecContext, AVFrame* pFrame){    
    int response = avcodec_send_packet(pCodecContext, pPacket);
    if(response < 0){
        logging("Error while sending a packet to the decoder: %s", av_err2str(response));
        return response;
    }    
    while(response >= 0){
        response = avcodec_receive_frame(pCodecContext, pFrame);         
        if(response == AVERROR(EAGAIN) || response == AVERROR_EOF){
            return response;            
        }else if(response < 0){            
            logging("Error while receiving a frame from the decoder: %s", av_err2str(response));
            return response;
        }        
        if(response >= 0){
            logging(
                    "Frame %d (type=%c, size=%d bytes) pts %d key_frame %d [DTS %d]",
                    pCodecContext->frame_number,
                    av_get_picture_type_char(pFrame->pict_type),
                    pFrame->pkt_size,
                    pFrame->pts,
                    pFrame->key_frame,
                    pFrame->coded_picture_number
                    );
            char frame_filename[1024];
            snprintf(frame_filename, sizeof(frame_filename), "%s-%d.pgm", "frame", pCodecContext->frame_number);
            save_gray_frame(pFrame->data[0], pFrame->linesize[0], pFrame->width, pFrame->height, frame_filename);
        }
    }
    return 0;
}

static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename){
    FILE *f;
    int i;
    f = fopen(filename, "w");
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for(i = 0; i < ysize; i++){
        fwrite(buf + i * wrap, 1, xsize, f);
    }
    fclose(f);
}