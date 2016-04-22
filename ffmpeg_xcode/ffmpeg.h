//
//  ffmpeg.h
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/7.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#ifndef ffmpeg_h
#define ffmpeg_h

#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/samplefmt.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/error.h"
#include "libavutil/parseutils.h"
#include "libavutil/time.h"
#include "libavutil/threadmessage.h"
#include "libavfilter/avfilter.h"
#include "libavutil/bprint.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavcodec/mathops.h"

typedef enum {
    ENCODER_FINISHED = 1,
    MUXER_FINISHED = 2,
} OSTFinished ;

typedef struct InputFilter {
    AVFilterContext    *filter;
    struct InputStream *ist;
    struct FilterGraph *graph;
    uint8_t            *name;
} InputFilter;

typedef struct OutputFilter {
    AVFilterContext     *filter;
    struct OutputStream *ost;
    struct FilterGraph  *graph;
    uint8_t             *name;
    
    /* temporary storage until stream maps are processed */
    AVFilterInOut       *out_tmp;
    enum AVMediaType     type;
} OutputFilter;

typedef struct FilterGraph {
    int index;
    const char *graph_desc;
    InputFilter **inputs;
    int nb_inputs;
    OutputFilter **outputs;
    int nb_outputs;
    AVFilterGraph *graph;
} FilterGraph;

typedef struct InputFiles {
    AVFormatContext *ctx;
    int ist_index;
    int nb_streams;
    AVRational time_base;
    int duration;
    int ts_offset;
    int thread_queue_size;
} InputFile;

typedef struct InputStream {
    AVStream *st;
    int file_index;
    int min_pts;
    int max_pts;
    AVCodec *dec;
    AVCodecContext *dec_ctx;
    int resample_height;
    int resample_width;
    enum AVPixelFormat resample_pix_fmt;
    enum AVSampleFormat resample_sample_fmt;
    int resample_sample_rate;
    int resample_channels;
    uint64_t resample_channel_layout;
    int user_set_discard;
    int discard;
    
    InputFilter **filters;
    int nb_filters;
    
#define DECODING_FOR_OST    1
#define DECODING_FOR_FILTER 2
    int decoding_needed;
    
    AVDictionary *decoder_opts;
    int64_t next_pts;
    int64_t dts;
    int64_t next_dts;
    int64_t pts;
    uint64_t data_size;
    uint64_t nb_packets;
    AVFrame *decoded_frame;
    AVFrame *filter_frame;
} InputStream;

typedef struct OutputFiles {
    AVFormatContext *ctx;
    int ost_index;
    int64_t recording_time;
    int64_t start_time;
    uint64_t limit_filesize;
    int shortest;
    AVDictionary *opts;
} OutputFile;

typedef struct OutputStream {
    int file_index;
    int index;
    AVStream *st;
    AVCodecContext *enc_ctx;
    AVCodec *enc;
    AVDictionary *encoder_opts;
    int64_t max_frames;
    int frame_number;
    InputStream *sync_list;
    int source_index;
    int64_t last_mux_dts;
    char *avfilter;
    OutputFilter *filter;
    int encoding_needed;
    AVRational frame_rate;
    
    int finished;
    AVFrame *filtered_frame;
    
    int64_t sync_opts;
    int frame_encoded;
    uint64_t data_size;
    AVBitStreamFilterContext *bitstream_filters;
        int last_nb0_frames[3];
        int is_cfr;
        int last_dropped;
} OutputStream;

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;
static volatile int transcode_init_done = 0;

extern InputStream **input_streams;
extern int nb_input_streams;
extern InputFile **input_files;
extern int nb_input_files;

extern OutputStream **output_streams;
extern int nb_output_streams;
extern OutputFile **output_files;
extern int nb_output_files;

extern FilterGraph **filtergraphs;
extern int nb_filtergraphs;

void *grow_array(void *array, int elem_size, int *size, int new_size);

#define GROW_ARRAY(array, nb_elems)\
    array = grow_array(array, sizeof(*array), &nb_elems, nb_elems + 1)

#endif /* ffmpeg_h */
