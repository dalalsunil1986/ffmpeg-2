//
// Created by wlanjie on 16/4/26.
//

#ifndef FFMPEG_COMPRESS_H
#define FFMPEG_COMPRESS_H

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/parseutils.h"
#include "libavfilter/avfilter.h"
#include "libavutil/bprint.h"
#include "libavutil/pixdesc.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"

typedef struct InputFile {
    AVFormatContext *ic;
} InputFile;

typedef struct InputStream {
    AVStream *st;
    AVCodecContext *dec_ctx;
    struct AVCodec *dec;
    struct InputFilter *filter;
} InputStream;

typedef struct OutputFile {
    AVFormatContext *oc;
} OutputFile;

typedef struct OutputStream {
    AVStream *st;
    AVCodecContext *enc_ctx;
    struct AVCodec *enc;
    int source_index;
    char *avfilter;
    struct OutputFilter *filter;
    int finished;
    uint64_t sync_opts;
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
    AVFilterGraph *graph;
    InputFilter *input;
    OutputFilter *output;
} FilterGraph;

extern InputFile *input_file;
extern InputStream **input_streams;
extern int nb_input_streams;
extern OutputFile *output_file;
extern OutputStream **output_streams;
extern int nb_output_streams;

int open_files(const char *input_file, const char *output_file, int new_width, int new_height);
int transcode();
void release();
#endif //FFMPEG_COMPRESS_H
