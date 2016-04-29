//
//  video_filter.c
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/23.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#include "video_filter.h"

int configure_input_video_filter(FilterGraph *fg, AVFilterInOut *in) {
    int ret = 0;
    AVFilter *buffer = avfilter_get_by_name("buffer");
    if (!buffer) {
        return AVERROR(EINVAL);
    }
    AVBPrint args;
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    InputFilter *ifilter = fg->input;
    const int width = ifilter->ist->dec_ctx->width;
    const int height = ifilter->ist->dec_ctx->height;
    const enum AVPixelFormat formt = ifilter->ist->dec_ctx->pix_fmt;
    const struct AVRational time_base = ifilter->ist->dec_ctx->time_base;
    const struct AVRational sample_aspect_ratio = ifilter->ist->dec_ctx->sample_aspect_ratio;
    av_bprintf(&args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", width, height, formt,
               time_base.num, time_base.den, sample_aspect_ratio.num, sample_aspect_ratio.den);
    char name[255];
    snprintf(name, sizeof(name), "video graph input stream %d", ifilter->ist->st->index);
    ret = avfilter_graph_create_filter(&fg->input->filter, buffer, name, args.str, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }
    ret = avfilter_link(ifilter->filter, 0, in->filter_ctx, 0);
    if (ret < 0) {
        return ret;
    }
    return ret;
}

int configure_input_audio_filter(FilterGraph *fg, AVFilterInOut *in) {
    int ret = 0;
    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        return AVERROR(EINVAL);
    }
    AVBPrint args;
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s", 1, fg->input->ist->dec_ctx->sample_rate, fg->input->ist->dec_ctx->sample_rate,
               av_get_sample_fmt_name(fg->input->ist->dec_ctx->sample_fmt));
    if (fg->input->ist->dec_ctx->channel_layout) {
        av_bprintf(&args, ":channel_layout=0x%"PRIx64, fg->input->ist->dec_ctx->channel_layout);
    } else {
        av_bprintf(&args, ":channels=%d", fg->input->ist->dec_ctx->channels);
    }
    char name[255];
    snprintf(name, sizeof(name), "audio graph input stream %d", fg->input->ist->st->index);
    ret = avfilter_graph_create_filter(&fg->input->filter, abuffer, name, args.str, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }
    ret = avfilter_link(fg->input->filter, 0, in->filter_ctx, 0);
    if (ret < 0) {
        return ret;
    }
    return ret;
}

int configure_output_video_filter(FilterGraph *fg, AVFilterInOut *out) {
    int ret = 0;
    AVFilter *buffersink = avfilter_get_by_name("buffersink");
    char name[255];
    snprintf(name, sizeof(name), "video graph output stream %d", fg->output->ost->st->index);
    ret = avfilter_graph_create_filter(&fg->output->filter, buffersink, name, NULL, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }

    AVFilterContext *format_context;
    AVFilter *format = avfilter_get_by_name("format");
    AVIOContext *s;
    if (avio_open_dyn_buf(&s) < 0) {
        return ret;
    }
    const enum AVPixelFormat *p = fg->output->ost->enc->pix_fmts;
    for (; *p != AV_PIX_FMT_NONE; p++) {
        avio_printf(s, "%s|", av_get_pix_fmt_name(*p));
    }
    uint8_t *tmp;
    int len;
    len = avio_close_dyn_buf(s, &tmp);
    tmp[len - 1] = 0;
    ret = avfilter_graph_create_filter(&format_context, format, "format", (char *) tmp, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }
    ret = avfilter_link(out->filter_ctx, 0, format_context, 0);
    if (ret < 0) {
        return ret;
    }
    av_free(tmp);
    AVFilterContext *scale_context;
    AVFilter *scale = avfilter_get_by_name("scale");
    char scale_name[255];
    snprintf(scale_name, sizeof(scale_name), "video scale output stream %d", fg->output->ost->st->index);

    AVBPrint scale_args;
    av_bprint_init(&scale_args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&scale_args, "%d:%d", fg->output->ost->enc_ctx->width, fg->output->ost->enc_ctx->height);
    ret = avfilter_graph_create_filter(&scale_context, scale, scale_name, scale_args.str, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }
    av_bprint_clear(&scale_args);
    ret = avfilter_link(format_context, 0, scale_context, 0);
    if (ret < 0) {
        return ret;
    }
    ret = avfilter_link(scale_context, 0, fg->output->filter, 0);
    if (ret < 0) {
        return ret;
    }
    return ret;
}

#define DEF_CHOOSE_FORMAT(type, var, supported_list, none, get_name)    \
char *choose_ ## var ## s(OutputStream *ost) {                          \
    if (ost->enc_ctx->var != none) {                                    \
        get_name(ost->enc_ctx->var);                                    \
        return av_strdup(name);                                         \
    } else if (ost->enc && ost->enc->supported_list) {                  \
        const type *p;                                                  \
        AVIOContext *s = NULL;                                          \
        uint8_t *ret;                                                   \
        int len;                                                        \
        if (avio_open_dyn_buf(&s) < 0) {                                \
            return NULL;                                                \
        }                                                               \
        for (p = ost->enc->supported_list; *p != none; p++) {           \
            get_name(*p);                                               \
            avio_printf(s, "%s|", name);                                \
        }                                                               \
        len = avio_close_dyn_buf(s, &ret);                              \
        ret[len - 1] = 0;                                               \
        return (char *) ret;                                            \
    } else {                                                            \
        return NULL;                                                    \
    }                                                                   \
}                                                                       \

#define GET_SAMPLE_FMT_NAME(sample_fmt)                                 \
    const char *name = av_get_sample_fmt_name(sample_fmt);              \

#define GET_SAMPLE_RATE_NAME(rate)                                      \
    char name[255];                                                     \
    snprintf(name, sizeof(name), "%d", rate);                           \

#define GET_CHANNEL_LAYOUT_NAME(channel_layout)                         \
    char name[255];                                                     \
    snprintf(name, sizeof(name), "0x%"PRIx64, channel_layout);          \

DEF_CHOOSE_FORMAT(enum AVSampleFormat, sample_fmt, sample_fmts, AV_SAMPLE_FMT_NONE, GET_SAMPLE_FMT_NAME);

DEF_CHOOSE_FORMAT(int, sample_rate, supported_samplerates, 0, GET_SAMPLE_RATE_NAME);

DEF_CHOOSE_FORMAT(uint64_t, channel_layout, channel_layouts, 0, GET_CHANNEL_LAYOUT_NAME);

int configure_output_audio_filter(FilterGraph *fg, AVFilterInOut *out) {
    int ret = 0;
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    char name[255];
    snprintf(name, sizeof(name), "audio graph output stream %d", fg->output->ost->st->index);
    ret = avfilter_graph_create_filter(&fg->output->filter, abuffersink, name, NULL, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }
    const char *fmts = choose_sample_fmts(fg->output->ost);
    const char *rates = choose_sample_rates(fg->output->ost);
    const char *channel_layouts = choose_channel_layouts(fg->output->ost);
    char args[255];
    args[0] = 0;
    if (fmts) {
        av_strlcatf(args, sizeof(args), "sample_fmts=%s:", fmts);
    }
    if (rates) {
        av_strlcatf(args, sizeof(args), "sample_rates=%s:", rates);
    }
    if (channel_layouts) {
        av_strlcatf(args, sizeof(args), "channel_layouts=%s:", channel_layouts);
    }
    av_freep(&fmts);
    av_freep(&rates);
    av_freep(&channel_layouts);
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilterContext *aformat_context;
    ret = avfilter_graph_create_filter(&aformat_context, aformat, "aformat", args, NULL, fg->graph);
    if (ret < 0) {
        return ret;
    }
    ret = avfilter_link(out->filter_ctx, 0, aformat_context, 0);
    if (ret < 0) {
        return ret;
    }
    ret = avfilter_link(aformat_context, 0, fg->output->filter, 0);
    if (ret < 0) {
        return 0;
    }
    return ret;
}

int configure_filtergraph(FilterGraph *fg) {
    int ret = 0;
    if (!(fg->graph = avfilter_graph_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc filter graph.\n");
        return AVERROR(ENOMEM);
    }
    avfilter_graph_free(&fg->graph);
    AVFilterInOut *in, *out, *cur;
    fg->graph = avfilter_graph_alloc();
    ret = avfilter_graph_parse2(fg->graph, fg->output->ost->avfilter, &in, &out);
    if (ret < 0) {
        return AVERROR(EINVAL);
    }
    if (!in || in->next || !out || out->next) {
        return AVERROR(EINVAL);
    }
    int i;
    for (i = 0, cur = in; cur; cur = in->next, i++) {
        switch (avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx)) {
            case AVMEDIA_TYPE_VIDEO:
                if ((ret = configure_input_video_filter(fg, cur)) < 0) {
                    avfilter_inout_free(&in);
                    avfilter_inout_free(&out);
                    return ret;
                }
                break;

            case AVMEDIA_TYPE_AUDIO:
                if ((ret = configure_input_audio_filter(fg, cur)) < 0) {
                    avfilter_inout_free(&in);
                    avfilter_inout_free(&out);
                    return ret;
                }
                break;
            default:
                break;
        }
    }
    avfilter_inout_free(&in);
    for (i = 0, cur = out; cur; cur = out->next, i++) {
        switch (avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx)) {
            case AVMEDIA_TYPE_VIDEO:
                if ((ret = configure_output_video_filter(fg, out)) < 0) {
                    avfilter_inout_free(&out);
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                if ((ret = configure_output_audio_filter(fg, out)) < 0) {
                    avfilter_inout_free(&out);
                }
                break;
            default:
                break;
        }
    }
    avfilter_inout_free(&out);
    ret = avfilter_graph_config(fg->graph, NULL);
    if (ret < 0) {
        return AVERROR(EINVAL);
    }
    return ret;
}

FilterGraph* init_filtergraph(InputStream *ist, OutputStream *ost) {
    FilterGraph *graph = av_mallocz(sizeof(*graph));
    graph->input = av_mallocz(sizeof(*graph->input));
    graph->output = av_mallocz(sizeof(*graph->output));
    graph->input->ist = ist;
    graph->input->graph = graph;
    graph->output->ost = ost;
    graph->output->graph = graph;
    ost->filter = graph->output;
    ist->filter = graph->input;
    return graph;
}
