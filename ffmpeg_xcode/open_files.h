//
//  open_files.h
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/23.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#ifndef open_files_h
#define open_files_h

#include <stdio.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/parseutils.h"
#include "libavfilter/avfilter.h"

enum ERROR {
    NONE,
    NOT_OPEN_INPUT_FILE,
    NOT_OPEN_OUTPUT_FILE
};

typedef struct InputFile {
    AVFormatContext *ic;
} InputFile;

typedef struct OutputFile {
    AVFormatContext *oc;
} OutputFile;

typedef struct InputStream {
    AVCodecContext *dec_ctx;
    AVCodec *dec;
    AVStream *st;
    int resample_width;
    int resample_height;
    int resample_pix_fmt;
    int resample_channels;
    int resample_channel_layout;
    int resample_sample_fmt;
    int resample_sample_rate;
    struct InputFilter *filter;
} InputStream;

typedef struct OutputStream {
    AVCodecContext *enc_ctx;
    AVCodec *enc;
    AVStream *st;
    char *avfilter;
    struct OutputFilter *filter;
    int finished;
    uint64_t sync_opts;
    int index;
    AVFrame *filtered_frame;
} OutputStream;

typedef struct InputFilter {
    AVFilterContext *filter;
    struct InputStream *ist;
    struct FilterGraph *graph;
} InputFilter;

typedef struct OutputFilter {
    AVFilterContext *filter;
    struct OutputStream *ost;
    struct FilterGraph *graph;
} OutputFilter;

typedef struct FilterGraph {
    InputFilter *input;
    OutputFilter *output;
    AVFilterGraph *graph;
} FilterGraph;

extern InputFile *input_file;
extern InputStream **input_streams;
extern int nb_input_streams;

extern OutputFile *output_file;
extern OutputStream **output_streams;
extern int nb_output_streams;

void *grow_array(void *array, int elem_size, int *size, int new_size);

#define GROW_ARRAY(array, nb_elems)\
    array = grow_array(array, sizeof(*array), &nb_elems, nb_elems + 1);

enum ERROR open_files(const char *input_filename, const char *output_filename, int width, int height);
#endif /* open_files_h */
