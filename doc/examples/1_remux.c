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
    return (EXIT_SUCCESS);
}

