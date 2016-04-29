//
//  open_files.c
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/23.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#include "open_files.h"

InputFile *input_file = NULL;
InputStream **input_streams = NULL;
int nb_input_streams = 0;

OutputFile *output_file = NULL;
OutputStream **output_streams = NULL;
int nb_output_streams = 0;

void *grow_array(void *array, int elem_size, int *size, int new_size) {
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array to big.\n");
        return NULL;
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(array, elem_size, new_size);
        memset(tmp + *size * elem_size, 0, (new_size - *size) * elem_size);
        *size = new_size;
        return tmp;
    }
    return array;
}

OutputStream *new_output_stream(AVFormatContext *oc, enum AVMediaType type, const char *codec_name, int source_index) {
    AVStream *st = avformat_new_stream(oc, NULL);
    if (!st) {
        return NULL;
    }
    OutputStream *ost = av_mallocz(sizeof(*ost));
    GROW_ARRAY(output_streams, nb_output_streams);
    ost->index = source_index;
    ost->st = st;
    ost->enc = avcodec_find_encoder_by_name(codec_name);
    ost->enc_ctx = avcodec_alloc_context3(ost->enc);
    ost->enc_ctx->codec_type = type;
    ost->st->codec->codec_type = type;
    ost->enc_ctx->codec_type = type;
    output_streams[nb_output_streams - 1] = ost;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        ost->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    return ost;
}

int open_output_file(const char *output_filename, int new_width, int new_height) {
    int ret = 0;
    AVFormatContext *oc;
    ret = avformat_alloc_output_context2(&oc, NULL, NULL, output_filename);
    if (ret < 0) {
        return ret;
    }
    output_file = av_mallocz(sizeof(*output_file));
    output_file->oc = oc;
    for (int i = 0; i < nb_input_streams; i++) {
        InputStream *ist = input_streams[i];
        switch (ist->st->codec->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (av_guess_codec(oc->oformat, NULL, output_filename, NULL, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) {
                    OutputStream *ost = new_output_stream(oc, AVMEDIA_TYPE_VIDEO, "libx264", i);
                    if (!ost) {
                        avformat_free_context(oc);
                    }
                    char new_size[10];
                    snprintf(new_size, sizeof(new_size), "%dx%d", new_width, new_height);
                    ret = av_parse_video_size(&ost->enc_ctx->width, &ost->enc_ctx->height, new_size);
                    if (ret < 0) {
                        return AVERROR(ENOMEM);
                    }
                    ost->st->sample_aspect_ratio = ost->enc_ctx->sample_aspect_ratio;
                    ost->avfilter = "null";
                }
                break;

            case AVMEDIA_TYPE_AUDIO:
                if (av_guess_codec(oc->oformat, NULL, output_filename, NULL, AVMEDIA_TYPE_AUDIO) != AV_CODEC_ID_NONE) {
                    OutputStream *ost = new_output_stream(oc, AVMEDIA_TYPE_AUDIO, "aac", i);
                    if (!ost) {
                        avformat_free_context(oc);
                    }
                    ost->avfilter = "anull";
                }
                break;
            default:
                break;
        }
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&oc->pb, output_filename, AVIO_FLAG_WRITE, NULL, NULL);
        if (ret < 0) {
            return ret;
        }
    }
    return ret;
}

int open_input_file(const char *input_filename) {
    int ret = 0;
    AVFormatContext *ic = avformat_alloc_context();
    ret = avformat_open_input(&ic, input_filename, NULL, NULL);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) {
        if (ic->streams == 0) {
            avformat_close_input(&ic);
        }
        return ret;
    }

    for (int i = 0; i < ic->nb_streams; i++) {
        GROW_ARRAY(input_streams, nb_input_streams);
        AVStream *st = ic->streams[i];
        InputStream *ist = av_mallocz(sizeof(*ist));
        ist->st = st;
        ist->dec = avcodec_find_decoder(st->codec->codec_id);
        ist->dec_ctx = avcodec_alloc_context3(ist->dec);
        ret = avcodec_copy_context(ist->dec_ctx, st->codec);
        if (ret < 0) {
            return AVERROR(ENXIO);
        }
        input_streams[nb_input_streams - 1] = ist;
    }
    av_dump_format(ic, 0, input_filename, 0);
    input_file = av_mallocz(sizeof(*input_file));
    input_file->ic = ic;
    return ret;
}

enum ERROR open_files(const char *input_filename, const char *output_filename, int width, int height) {
    av_register_all();
    avcodec_register_all();
    avfilter_register_all();
    if (open_input_file(input_filename) < 0) {
        return NOT_OPEN_INPUT_FILE;
    }
    if (open_output_file(output_filename, width, height) < 0) {
        return NOT_OPEN_OUTPUT_FILE;
    }
    return NONE;
}
