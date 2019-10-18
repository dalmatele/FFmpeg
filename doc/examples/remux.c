// based on https://ffmpeg.org/doxygen/trunk/remuxing_8c-example.html
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>

#include "libavutil/opt.h"

static void logging(const char *fmt, ...);

int main(int argc, char **argv)
{
  AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
  AVPacket in_packet;
  AVPacket *out_packet;
  const char *in_filename, *out_filename;
  int ret, i;
  int stream_index = 0;
  int *streams_list = NULL;
  int number_of_streams = 0;
  int fragmented_mp4_options = 0;
  AVCodec *output_codec;
  AVCodec *input_codec;
  AVCodecContext *output_codec_context;
  AVCodecContext *input_codec_context;
  AVFrame *frame;
  int video_stream_index = -1;

  if (argc < 3) {
    printf("You need to pass at least two parameters.\n");
    return -1;
  } else if (argc == 4) {
    fragmented_mp4_options = 1;
  }

  in_filename  = argv[1];
  out_filename = argv[2];

  if ((ret = avformat_open_input(&input_format_context, in_filename, NULL, NULL)) < 0) {
    logging("Could not open input file '%s'", in_filename);
    goto end;
  }
  if ((ret = avformat_find_stream_info(input_format_context, NULL)) < 0) {
    logging("Failed to retrieve input stream information");
    goto end;
  }

  avformat_alloc_output_context2(&output_format_context, NULL, NULL, out_filename);
  if (!output_format_context) {
    logging("Could not create output context\n");
    ret = AVERROR_UNKNOWN;
    goto end;
  }

  number_of_streams = input_format_context->nb_streams;
  streams_list = av_mallocz_array(number_of_streams, sizeof(*streams_list));

  if (!streams_list) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  output_codec = avcodec_find_encoder(AV_CODEC_ID_H264);  
  if(!output_codec){
      logging("Failed to find output codec\n");
      ret = AVERROR_ENCODER_NOT_FOUND;
      goto end;
  }
  output_codec_context = avcodec_alloc_context3(output_codec);
  if(!output_codec_context){
      logging("Failed to find output codec context\n");
      ret = AVERROR_UNKNOWN;
      goto end;
  }  
  for (i = 0; i < input_format_context->nb_streams; i++) {
    AVStream *out_stream;
    AVStream *in_stream = input_format_context->streams[i];
    AVCodecParameters *in_codecpar = in_stream->codecpar;
    if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
        in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
        in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
      streams_list[i] = -1;
      continue;
    }
    streams_list[i] = stream_index++;
    out_stream = avformat_new_stream(output_format_context, NULL);
    if (!out_stream) {
      logging("Failed allocating output stream\n");
      ret = AVERROR_UNKNOWN;
      goto end;
    }        
    if(in_codecpar->codec_type == AVMEDIA_TYPE_AUDIO || in_codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE){          
        in_codecpar->frame_size = 1024;
        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            logging("Failed to copy codec parameters\n");
            goto end;
        }
    }else{
        if(video_stream_index == -1){
            video_stream_index = i;
        } 
        //we open input codec context
        input_codec = avcodec_find_decoder(in_codecpar->codec_id);
        if(!input_codec){
            logging("Failed to find input codec: %d\n", in_codecpar->codec_id);
            ret = AVERROR_DECODER_NOT_FOUND;
            goto end;
        }
        input_codec_context = avcodec_alloc_context3(input_codec);
        if(!input_codec_context){
            logging("Failed to find output codec context\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
        
        output_codec_context->bit_rate = 40000;
        output_codec_context->width = in_codecpar->width;
        output_codec_context->height = in_codecpar->height;
        output_codec_context->time_base = (AVRational){1, 24};
//        output_codec_context->framerate = (AVRational){25, 1};
        output_codec_context->gop_size = 10;
        output_codec_context->max_b_frames = 1;
        output_codec_context->pix_fmt = AV_PIX_FMT_YUV420P;      
        av_opt_set(output_codec_context->priv_data, "preset", "slow", 0);        
//        av_opt_set(output_codec_context->priv_data, "preset", "slow", 0);
        if(avcodec_open2(output_codec_context, output_codec, NULL) < 0){
            logging("Could not open codec");
            goto end;
        }
        if(avcodec_open2(input_codec_context, input_codec, NULL) < 0){
            logging("Could not open codec");
            goto end;
        }
        ret = avcodec_parameters_from_context(out_stream->codecpar, output_codec_context);  
        if (ret < 0) {
            logging("Failed to copy codec parameters\n");
            goto end;
        }
    }
//    ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
//    //set params for output codec context  
//        if (ret < 0) {
//            fprintf(stderr, "Failed to copy codec parameters\n");
//            goto end;
//        }    
  }
  // https://ffmpeg.org/doxygen/trunk/group__lavf__misc.html#gae2645941f2dc779c307eb6314fd39f10
  av_dump_format(input_format_context, 0, in_filename, 0);
  
  av_dump_format(output_format_context, 0, out_filename, 1);

  // unless it's a no file (we'll talk later about that) write to the disk (FLAG_WRITE)
  // but basically it's a way to save the file to a buffer so you can store it
  // wherever you want.
  if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&output_format_context->pb, out_filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      logging("Could not open output file '%s'", out_filename);
      goto end;
    }
  }
  AVDictionary* opts = NULL;

  if (fragmented_mp4_options) {
    // https://developer.mozilla.org/en-US/docs/Web/API/Media_Source_Extensions_API/Transcoding_assets_for_MSE
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
  }
  // https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga18b7b10bb5b94c4842de18166bc677cb
  ret = avformat_write_header(output_format_context, &opts);
  if (ret < 0) {
    logging("Error occurred when opening output file\n");
    goto end;
  }
  out_packet = av_packet_alloc();
  if(!out_packet){
    logging("failed to allocated memory for AVPacket\n");
    goto end;
  }
  frame = av_frame_alloc();
  if(!frame){
    logging("failed to allocated memory for AVFrame\n");
    goto end;
  }
  while (1) {
    AVStream *in_stream, *out_stream;
    ret = av_read_frame(input_format_context, &in_packet);
    if (ret < 0)
      break;
    in_stream  = input_format_context->streams[in_packet.stream_index];
    if (in_packet.stream_index >= number_of_streams || streams_list[in_packet.stream_index] < 0) {
      av_packet_unref(&in_packet);
      continue;
    }
    in_packet.stream_index = streams_list[in_packet.stream_index];
    out_stream = output_format_context->streams[in_packet.stream_index]; 
    //only process video's packets
    if(in_packet.stream_index == video_stream_index){
        ret = avcodec_send_packet(input_codec_context, &in_packet);
        if(ret < 0){
            logging("Error while sending a packet to the decoder: %s", av_err2str(ret));
            goto end;
        } 
        while(ret >= 0){
            ret = avcodec_receive_frame(input_codec_context, frame);        
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                goto end;           
            }else if(ret < 0){            
                logging("Error while receiving a frame from the decoder: %s", av_err2str(ret));
                goto end;
            }
            if(ret >= 0){
                ret = avcodec_send_frame(output_codec_context, frame);                   
                if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                    goto end;           
                }else if(ret < 0){
                    logging("Error while sending a frame to the decoder: %s", av_err2str(ret));
                    goto end;
                }
                while(ret >= 0){
                    ret = avcodec_receive_packet(output_codec_context, out_packet);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    else if (ret < 0) {
                        logging("Error during encoding\n");
                        av_packet_unref(out_packet);
                        break;
                    }
                    //must write this to reduce high fps after transcoding
                    out_packet->pts = av_rescale_q(out_packet->pts, in_stream->time_base, out_stream->time_base);
                    out_packet->dts = av_rescale_q(out_packet->dts, in_stream->time_base, out_stream->time_base);
                    out_packet->duration = av_rescale_q(out_packet->duration, in_stream->time_base, out_stream->time_base);
                    ret = av_interleaved_write_frame(output_format_context, out_packet);
                    if (ret < 0) {
                        logging("Error muxing packet\n");
                        av_packet_unref(out_packet);
                        break;
                    }
                    av_packet_unref(out_packet);               
                }            
            }
            av_frame_unref(frame);
        }
    }else{
        //write audio data
//        https://stackoverflow.com/questions/13565062/fps-too-high-when-saving-video-in-mp4-container
        in_packet.pts = av_rescale_q_rnd(in_packet.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        in_packet.dts = av_rescale_q_rnd(in_packet.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        in_packet.duration = av_rescale_q(in_packet.duration, in_stream->time_base, out_stream->time_base);
        ret = av_interleaved_write_frame(output_format_context, &in_packet);
        if (ret < 0) {
            logging("Error muxing packet\n");
            break;
        }
    }              
    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html#ab5793d8195cf4789dfb3913b7a693903
    in_packet.pos = -1;

    //https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga37352ed2c63493c38219d935e71db6c1
//    ret = av_interleaved_write_frame(output_format_context, &in_packet);
//    if (ret < 0) {
//      logging("Error muxing packet\n");
//      break;
//    }
    av_packet_unref(&in_packet);
  }
  //https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga7f14007e7dc8f481f054b21614dfec13
  av_write_trailer(output_format_context);
end:
  avformat_close_input(&input_format_context);
  /* close output */
  if (output_format_context && !(output_format_context->oformat->flags & AVFMT_NOFILE)){
      avio_closep(&output_format_context->pb);
  }    
  avformat_free_context(output_format_context);
  av_freep(&streams_list);
  av_packet_free(&out_packet);
  av_frame_free(&frame);
  if (ret < 0 && ret != AVERROR_EOF) {
    logging("Error occurred: %s\n", av_err2str(ret));
    return 1;
  }
  return 0;
}
static void logging(const char *fmt, ...){
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}