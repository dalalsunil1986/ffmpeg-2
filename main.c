//
//  main.c
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/2/25.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#include <stdio.h>
#include <pthread.h>
#include "ffmpeg.h"

#define INPUT_FILE_NAME "/Users/wlanjie/Desktop/sintel.mp4"
#define OUTPUT_FILE_NAME "/Users/wlanjie/Desktop/ffmpeg.mp4"

InputStream **input_streams = NULL;
int        nb_input_streams = 0;
InputFile   **input_files   = NULL;
int        nb_input_files   = 0;

OutputStream **output_streams = NULL;
int         nb_output_streams = 0;
OutputFile   **output_files   = NULL;
int         nb_output_files   = 0;

FilterGraph **filtergraphs = NULL;
int        nb_filtergraphs = 0;

void *grow_array(void *array, int elem_size, int *size, int new_size)
{
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
        return NULL;
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(array, new_size, elem_size);
        if (!tmp) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc buffer.\n");
            return NULL;
        }
        memset(tmp + *size*elem_size, 0, (new_size-*size) * elem_size);
        *size = new_size;
        return tmp;
    }
    return array;
}

static int add_input_streams(AVFormatContext *ic) {
    int ret = 0;
    for (int i =0 ; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        AVCodecContext *dec = st->codec;
        InputStream *ist = av_mallocz(sizeof(*ist));
        if (!ist) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc input stream.\n");
            return AVERROR(ENOMEM);
        }
        GROW_ARRAY(input_streams, nb_input_streams);
        input_streams[nb_input_streams - 1] = ist;
        ist->st = st;
        ist->file_index = nb_input_files;
        ist->dec = avcodec_find_decoder(st->codec->codec_id);
        ist->dec_ctx = avcodec_alloc_context3(ist->dec);
        ret = avcodec_copy_context(ist->dec_ctx, dec);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
//        ist->user_set_discard = AVDISCARD_NONE;
        if (!ist->dec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc input AVCodecContext.\n");
            av_free(ist);
            avcodec_close(ist->dec_ctx);
            return AVERROR(ENOMEM);
        }
        switch (dec->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (!ist->dec) {
                    ist->dec = avcodec_find_decoder(dec->codec_id);
                }
                ist->resample_height = ist->dec_ctx->height;
                ist->resample_width= ist->dec_ctx->width;
                ist->resample_pix_fmt = ist->dec_ctx->pix_fmt;
                break;

            case AVMEDIA_TYPE_AUDIO:
                ist->resample_sample_fmt = ist->dec_ctx->sample_fmt;
                ist->resample_sample_rate = ist->dec_ctx->sample_rate;
                ist->resample_channels = ist->dec_ctx->channels;
                ist->resample_channel_layout = ist->dec_ctx->channel_layout;
                break;
            default:
                break;
        }
    }
    return ret;
}

static int decode_interrupt_cb(void *ctx) {
    return received_nb_signals > transcode_init_done;
}

const AVIOInterruptCB int_cb_1 = { decode_interrupt_cb, NULL };

static int open_input_file(const char *filename) {
    int ret;
    AVInputFormat *file_iformat = NULL;
    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc input format context.\n");
        return AVERROR(ENOMEM);
    }
    ic->flags |= AVFMT_FLAG_NONBLOCK;
    ic->interrupt_callback = int_cb_1;

    ret = avformat_open_input(&ic, filename, file_iformat, NULL);
    if (ret < 0) {
        av_err2str(ret);
        if (ic->nb_streams == 0) {
            avformat_close_input(&ic);
        }
        avformat_free_context(ic);
        return ret;
    }

    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) {
        av_err2str(ret);
        if (ic->nb_streams == 0) {
            avformat_close_input(&ic);
            avformat_free_context(ic);
            return ret;
        }
    }

    add_input_streams(ic);

    av_dump_format(ic, nb_input_files, filename, 0);
    GROW_ARRAY(input_files, nb_input_files);
    InputFile *f = av_mallocz(sizeof(*f));
    if (!f) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc InputFile.\n");
        avformat_close_input(&ic);
        avformat_free_context(ic);
        return AVERROR(ENOMEM);
    }
    input_files[nb_input_files - 1] = f;
    f->ctx = ic;
//    f->ist_index = nb_input_streams - ic->nb_streams;
//    f->ts_offset = 0;
//    f->duration = 0;
    f->nb_streams = ic->nb_streams;
    f->thread_queue_size = 8;
    f->time_base = (AVRational) { 1, 1 };
    return ret;
}

static OutputStream *new_output_stream(AVFormatContext *oc, enum AVMediaType type, char *codec_name, int source_index) {
    OutputStream *ost;
    AVStream *st = avformat_new_stream(oc, NULL);
    int idx = oc->nb_streams - 1;
    if (!st) {
        av_log(NULL, AV_LOG_ERROR, "Could not new Output Stream.\n");
        return NULL;
    }
    GROW_ARRAY(output_streams, nb_output_streams);
    if (!(ost = av_mallocz(sizeof(*ost)))) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc OutputStream.\n");
        return NULL;
    }
    output_streams[nb_output_streams - 1] = ost;
    ost->file_index = nb_output_files - 1;
    ost->index = idx;
    ost->st = st;
    st->codec->codec_type = type;
    ost->enc = avcodec_find_encoder_by_name(codec_name);
    if (!ost->enc) {
        av_log(NULL, AV_LOG_ERROR, "Could not find encoder by name %s.\n", codec_name);
        return NULL;
    }
    ost->enc_ctx = avcodec_alloc_context3(ost->enc);
    if (!ost->enc_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc Output AVCodecContext.\n");
        return NULL;
    }
    ost->enc_ctx->codec_type = type;
    int ret = av_dict_set(&ost->encoder_opts, "strict", "-2", 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not dict set strict value -2.\n");
        return NULL;
    }
    ost->max_frames = INT64_MAX;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        ost->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ost->source_index = source_index;
    if (source_index >= 0) {
        ost->sync_list = input_streams[source_index];
        input_streams[source_index]->discard = 0;
        input_streams[source_index]->st->discard = input_streams[source_index]->user_set_discard;
    }
    ost->last_mux_dts = AV_NOPTS_VALUE;
    return ost;
}

static int new_video_stream(AVFormatContext *oc, int source_index) {
    int ret;
    OutputStream *ost = new_output_stream(oc, AVMEDIA_TYPE_VIDEO, "libx264", source_index);
    if (ost == NULL) {
        return AVERROR(ENOMEM);
    }

    AVStream *st = ost->st;
    AVCodecContext *video_enc = ost->enc_ctx;
//    if (av_parse_video_size(&video_enc->width, &video_enc->height, "640x320") < 0) {
//        av_log(NULL, AV_LOG_ERROR, "Could not parse video size %s.\n", "640x320");
//        av_err2str(ret);
//        return ret;
//    }
    st->sample_aspect_ratio = video_enc->sample_aspect_ratio;
    ost->avfilter = st->codec->codec_type == AVMEDIA_TYPE_VIDEO ? "null" : "anull";
    return ret;
}

static int new_audio_stream(AVFormatContext *oc, int source_index) {
    int ret;
    OutputStream *ost = new_output_stream(oc, AVMEDIA_TYPE_AUDIO, "aac", source_index);
    if (ost == NULL) {
        return AVERROR(ENOMEM);
    }
    AVStream *st = ost->st;
    AVCodecContext *audio_enc = ost->enc_ctx;
    audio_enc->codec_type = AVMEDIA_TYPE_AUDIO;
    ost->avfilter = st->codec->codec_type == AVMEDIA_TYPE_VIDEO ? "null" : "anull";
    return ret;
}

static int open_output_file(const char *filename) {
    int ret;
    GROW_ARRAY(output_files, nb_output_files);
    OutputFile *of = av_mallocz(sizeof(*of));
    if (!of) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc OutputFile.\n");
        return AVERROR(ENOMEM);
    }
    of->ost_index = nb_output_streams;
    of->recording_time = INT64_MAX;
    of->start_time = INT64_MIN;
    of->limit_filesize = UINT64_MAX;
    of->shortest = 0;
    ret = av_dict_set(&of->opts, "strict", "-2", 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not set dict.\n");
        av_free(of);
        return AVERROR(ENOMEM);
    }
    AVFormatContext *oc = avformat_alloc_context();
    ret = avformat_alloc_output_context2(&oc, NULL, NULL, filename);
    if (ret < 0) {
        av_err2str(ret);
        avformat_free_context(oc);
        return ret;
    }
    of->ctx = oc;
    output_files[nb_output_files - 1] = of;
    InputStream *ist;
    AVOutputFormat *file_oformat = oc->oformat;
    if (av_guess_codec(file_oformat, NULL, filename, NULL, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) {
        int area = 0, idx = -1;
        int qcr = avformat_query_codec(oc->oformat, oc->oformat->video_codec, 0);
        for (int i = 0; i < nb_input_streams; i++) {
            int new_area;
            ist = input_streams[i];
            new_area = ist->st->codec->width * ist->st->codec->height + 100000000*!!ist->st->codec_info_nb_frames;
            if((qcr!=MKTAG('A', 'P', 'I', 'C')) && (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                new_area = 1;
            if (ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
                new_area > area) {
                if((qcr==MKTAG('A', 'P', 'I', 'C')) && !(ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                    continue;
                area = new_area;
                idx = i;
            }
        }
        if (idx >= 0)
            new_video_stream(oc, idx);
    }
    if (av_guess_codec(file_oformat, NULL, filename, NULL, AVMEDIA_TYPE_AUDIO) != AV_CODEC_ID_NONE) {
        int best_score = 0, idx = -1;
        for (int i = 0; i < nb_input_streams; i++) {
            int score;
            ist = input_streams[i];
            score = ist->st->codec->channels + 100000000*!!ist->st->codec_info_nb_frames;
            if (ist->st->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
                score > best_score) {
                best_score = score;
                idx = i;
            }
        }
        if (idx >= 0)
            new_audio_stream(oc, idx);
    }
    for (int i = of->ost_index; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        ost->encoding_needed = 1;
        InputStream *ist = input_streams[ost->source_index];
        ist->decoding_needed |= DECODING_FOR_OST;
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open2(&oc->pb, filename, AVIO_FLAG_WRITE, &oc->interrupt_callback, &of->opts)) < 0) {
            av_err2str(ret);
            return ret;
        }
    }
//    oc->max_delay = (int) (0.7 * AV_TIME_BASE);
    if (nb_input_files) {
        av_dict_copy(&oc->metadata, input_files[0]->ctx->metadata, AV_DICT_DONT_OVERWRITE);
        av_dict_set(&oc->metadata, "creation_time", NULL, 0);
    }

    for (int i = of->ost_index; i < nb_output_streams; i++) {
        InputStream *ist;
        if (output_streams[i]->source_index < 0) {
            continue;
        }
        ist = input_streams[output_streams[i]->source_index];
        av_dict_copy(&output_streams[i]->st->metadata, ist->st->metadata, AV_DICT_DONT_OVERWRITE);
        av_dict_set(&output_streams[i]->st->metadata, "encoder", NULL, 0);
    }
    return ret;
}

static int open_files(const char *filename, int (*open_file)(const char*)) {
    return open_file(filename);
}

int main(int argc, char **argv) {
    av_register_all();
    avcodec_register_all();
    avfilter_register_all();
    int ret;
    ret = open_files(INPUT_FILE_NAME, open_input_file);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input file.\n");
        return ret;
    }
    ret = open_files(OUTPUT_FILE_NAME, open_output_file);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open output file.\n");
        return ret;
    }
//    ret = transcode();
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not transcode.\n");
        return ret;
    }
    return 0;
}
