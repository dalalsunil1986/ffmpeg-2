//
// Created by wlanjie on 16/4/26.
//

#include "compress_.h"

InputFile *input_file = NULL;
InputStream **input_streams = NULL;
int nb_input_streams = 0;
OutputFile *output_file = NULL;
OutputStream **output_streams = NULL;
int nb_output_streams = 0;

void *grow_array(void *array, int elem_size, int *size, int new_size) {
    if (new_size > INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array to big.\n");
        return NULL;
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(array, (size_t) elem_size, (size_t) new_size);
        memset(tmp + *size * elem_size, 0, (size_t) ((new_size - *size) * elem_size));
        *size = new_size;
        return tmp;
    }
    return array;
}

#define GROW_ARRAY(array, nb_elems) \
    array = grow_array(array, sizeof(*array), &nb_elems, nb_elems + 1);

int open_input_file(const char *input_path) {
    int ret = 0;
    AVFormatContext *ic = avformat_alloc_context();
    ret = avformat_open_input(&ic, input_path, NULL, NULL);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    for (int i = 0; i < ic->nb_streams; ++i) {
        AVStream *st = ic->streams[i];
        GROW_ARRAY(input_streams, nb_input_streams);
        InputStream *ist = av_mallocz(sizeof(*ist));
        ist->st = st;
        ist->dec = avcodec_find_decoder(st->codec->codec_id);
        ist->dec_ctx = avcodec_alloc_context3(ist->dec);
        ret = avcodec_copy_context(ist->dec_ctx, st->codec);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
        input_streams[nb_input_streams - 1] = ist;
    }
    input_file = av_mallocz(sizeof(*input_file));
    input_file->ic = ic;
    return ret;
}

OutputStream *new_output_stream(AVFormatContext *oc, enum AVMediaType type, const char *codec_name, int source_index) {
    AVStream *st = avformat_new_stream(oc, NULL);
    if (!st) {
        return NULL;
    }
    OutputStream *ost = av_mallocz(sizeof(*ost));
    GROW_ARRAY(output_streams, nb_output_streams);
    ost->source_index = source_index;
    AVCodec *enc = avcodec_find_encoder_by_name(codec_name);
    AVCodecContext *enc_ctx = avcodec_alloc_context3(enc);
    ost->enc_ctx = enc_ctx;
    ost->enc = enc;
    ost->st = st;
    ost->st->codec->codec_type = type;
    ost->enc_ctx->codec_type = type;
    output_streams[nb_output_streams - 1] = ost;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        ost->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    return ost;
}

int open_output_file(const char *output_path, int new_width, int new_height) {
    int ret = 0;
    AVFormatContext *oc = avformat_alloc_context();
    ret = avformat_alloc_output_context2(&oc, NULL, NULL, output_path);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    output_file = av_mallocz(sizeof(*output_file));
    output_file->oc = oc;
    for (int i = 0; i < nb_input_streams; ++i) {
        InputStream *ist = input_streams[i];
        switch (ist->st->codec->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (av_guess_codec(oc->oformat, NULL, output_path, NULL, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) {
                    OutputStream *ost = new_output_stream(oc, AVMEDIA_TYPE_VIDEO, "libx264", i);
                    if (ost == NULL) {
                        return AVERROR(ENOMEM);
                    }
                    if (new_width > 0 && new_height > 0) {
                        char video_size[10];
                        snprintf(video_size, sizeof(video_size), "%dx%d", new_width, new_height);
                        ret = av_parse_video_size(&ost->enc_ctx->width, &ost->enc_ctx->height,
                                                  video_size);
                        if (ret < 0) {
                            av_free(video_size);
                            av_err2str(ret);
                            return ret;
                        }
                    }
                    ost->st->sample_aspect_ratio = ost->enc_ctx->sample_aspect_ratio;
                    ost->avfilter = "null";
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (av_guess_codec(oc->oformat, NULL, output_path, NULL, AVMEDIA_TYPE_AUDIO) != AV_CODEC_ID_NONE) {
                    OutputStream *ost = new_output_stream(oc, AVMEDIA_TYPE_AUDIO, "aac", i);
                    if (ost == NULL) {
                        return AVERROR(ENOMEM);
                    }
                    ost->avfilter = "anull";
                }
                break;
            default:
                break;
        }
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&oc->pb, output_path, AVIO_FLAG_WRITE, NULL, NULL);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    return ret;
}

int configure_input_video_filter(FilterGraph *graph, AVFilterInOut *in) {
    int ret = 0;
    const AVFilter *buffer = avfilter_get_by_name("buffer");
    if (!buffer) {
        return AVERROR(EAGAIN);
    }
    AVBPrint args;
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    const int width = graph->input->ist->dec_ctx->width;
    const int height = graph->input->ist->dec_ctx->height;
    const enum AVPixelFormat format = graph->input->ist->dec_ctx->pix_fmt;
    const AVRational time_base = graph->input->ist->dec_ctx->time_base;
    const AVRational sample_aspect_ratio = graph->input->ist->dec_ctx->sample_aspect_ratio;
    av_bprintf(&args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", width, height,
                format, time_base.num, time_base.den, sample_aspect_ratio.num, sample_aspect_ratio.den);
    char name[255];
    snprintf(name, sizeof(name), "video graph input stream %d", graph->input->ist->st->index);
    ret = avfilter_graph_create_filter(&graph->input->filter, buffer, name, args.str, NULL, graph->graph);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avfilter_link(graph->input->filter, 0, in->filter_ctx, 0);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

int configure_input_audio_filter(FilterGraph *graph, AVFilterInOut *in) {
    int ret = 0;
    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        return AVERROR(EAGAIN);
    }
    AVBPrint args;
    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s", 1, graph->input->ist->dec_ctx->sample_rate,
                graph->input->ist->dec_ctx->sample_rate, av_get_sample_fmt_name(graph->input->ist->dec_ctx->sample_fmt));
    if (graph->input->ist->dec_ctx->channel_layout) {
        av_bprintf(&args, ":channel_layout=0x%"PRIX64, graph->input->ist->dec_ctx->channel_layout);
    } else {
        av_bprintf(&args, ":channels=%d", graph->input->ist->dec_ctx->channels);
    }
    char name[255];
    snprintf(name, sizeof(name), "audio graph input stream %d", graph->input->ist->st->index);
    ret = avfilter_graph_create_filter(&graph->input->filter, abuffer, name, args.str, NULL, graph->graph);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avfilter_link(graph->input->filter, 0, in->filter_ctx, 0);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

int configure_output_video_filter(FilterGraph *graph, AVFilterInOut *out) {
    int ret = 0;
    AVFilter *buffersink = avfilter_get_by_name("buffersink");
    char name[255];
    snprintf(name, sizeof(name), "video graph output stream %d", graph->output->ost->st->index);
    ret = avfilter_graph_create_filter(&graph->output->filter, buffersink, name, NULL, NULL, graph->graph);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    AVFilterContext *format_context;
    const AVFilter *format_filter = avfilter_get_by_name("format");
    const enum AVPixelFormat *p = graph->output->ost->enc->pix_fmts;
    AVIOContext *s;
    ret = avio_open_dyn_buf(&s);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    for (; *p != AV_PIX_FMT_NONE; p++) {
        avio_printf(s, "%s|", av_get_pix_fmt_name(*p));
    }
    uint8_t *tmp;
    int len = avio_close_dyn_buf(s, &tmp);
    tmp[len - 1] = 0;
    ret = avfilter_graph_create_filter(&format_context, format_filter, "format", (char *) tmp, NULL, graph->graph);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avfilter_link(out->filter_ctx, 0, format_context, 0);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    AVFilterContext *scale_context = NULL;
    if (graph->output->ost->enc_ctx->width || graph->output->ost->enc_ctx->height) {
        AVFilter *scale_filter = avfilter_get_by_name("scale");
        AVBPrint args;
        av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(&args, "%d:%d", graph->output->ost->enc_ctx->width, graph->output->ost->enc_ctx->height);
        ret = avfilter_graph_create_filter(&scale_context, scale_filter, "scale", args.str, NULL, graph->graph);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
        ret = avfilter_link(format_context, 0, scale_context, 0);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    ret = avfilter_link(scale_context ? scale_context : format_context, 0, graph->output->filter, 0);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

#define DEF_CHOOSE_FORMAT(type, var, supported_list, none, get_name)                                            \
char *choose_ ## var ## s(OutputStream *ost) {                                                                  \
    if (ost->enc_ctx->var != none) {                                                                            \
        get_name(ost->enc_ctx->var);                                                                            \
        return av_strdup(name);                                                                                 \
    } else if (ost->enc->supported_list) {                                                                      \
        const type *p;                                                                                          \
        AVIOContext *s = NULL;                                                                                  \
        uint8_t *tmp = NULL;                                                                                    \
        if (avio_open_dyn_buf(&s) < 0) {                                                                        \
            return NULL;                                                                                        \
        }                                                                                                       \
        for (p = ost->enc->supported_list; *p != none; p++) {                                                   \
            get_name(*p);                                                                                       \
            avio_printf(s, "%s|", name);                                                                        \
        }                                                                                                       \
        int len = avio_close_dyn_buf(s, &tmp);                                                                  \
        tmp[len - 1] = 0;                                                                                       \
        return (char *) tmp;                                                                                    \
    } else {                                                                                                    \
        return NULL;                                                                                            \
    }                                                                                                           \
}                                                                                                               \

#define GET_SAMPLE_FMT_NAME(sample_fmt)                                                                         \
    const char *name = av_get_sample_fmt_name(sample_fmt);                                                      \

#define GET_SAMPLE_RATE_NAME(sample_rate)                                                                       \
    char name[255];                                                                                             \
    snprintf(name, sizeof(name), "%d", sample_rate);                                                            \

#define GET_CHANNEL_LAYOUT_NAME(channel_layout)                                                                 \
    char name[255];                                                                                             \
    snprintf(name, sizeof(name), "0x%"PRIX64, channel_layout);                                                  \

DEF_CHOOSE_FORMAT(enum AVSampleFormat, sample_fmt, sample_fmts, AV_SAMPLE_FMT_NONE, GET_SAMPLE_FMT_NAME);

DEF_CHOOSE_FORMAT(int, sample_rate, supported_samplerates, 0, GET_SAMPLE_RATE_NAME);

DEF_CHOOSE_FORMAT(uint64_t, channel_layout, channel_layouts, 0, GET_CHANNEL_LAYOUT_NAME);

int configure_output_audio_filter(FilterGraph *graph, AVFilterInOut *out) {
    int ret = 0;
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    char name[255];
    snprintf(name, sizeof(name), "audio graph output stream %d", graph->output->ost->st->index);
    ret = avfilter_graph_create_filter(&graph->output->filter, abuffersink, name, NULL, NULL, graph->graph);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    const char *sample_fmts = choose_sample_fmts(graph->output->ost);
    const char *sample_rates = choose_sample_rates(graph->output->ost);
    const char *channel_layouts = choose_channel_layouts(graph->output->ost);
    char args[255];
    args[0] = 0;
    if (sample_fmts) {
        av_strlcatf(args, sizeof(args), "sample_fmts=%s:", sample_fmts);
    }
    if (sample_rates) {
        av_strlcatf(args, sizeof(args), "sample_rates=%s:", sample_rates);
    }
    if (channel_layouts) {
        av_strlcatf(args, sizeof(args), "channel_layouts=%s:", channel_layouts);
    }
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilterContext *aformat_context;
    ret = avfilter_graph_create_filter(&aformat_context, aformat, "aformat", args, NULL, graph->graph);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avfilter_link(out->filter_ctx, 0, aformat_context, 0);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = avfilter_link(aformat_context, 0, graph->output->filter, 0);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

int configure_filtergraph(FilterGraph *graph) {
    int ret = 0;
    avfilter_graph_free(&graph->graph);
    if (!(graph->graph = avfilter_graph_alloc())) {
        return AVERROR(ENOMEM);
    }
    AVFilterInOut *in, *out, *cur;
    ret = avfilter_graph_parse2(graph->graph, graph->output->ost->avfilter, &in, &out);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    if (!in || in->next || !out || out->next) {
        return AVERROR(EAGAIN);
    }
    int i = 0;
    for (i = 0, cur = in; cur; cur = in->next, i++) {
        switch (avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx)) {
            case AVMEDIA_TYPE_VIDEO:
                if ((ret = configure_input_video_filter(graph, cur)) < 0) {
                    avfilter_inout_free(&in);
                    avfilter_inout_free(&out);
                    return ret;
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                if ((ret = configure_input_audio_filter(graph, cur)) < 0) {
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
        switch (avfilter_pad_get_type(cur->filter_ctx->output_pads, cur->pad_idx)) {
            case AVMEDIA_TYPE_VIDEO:
                if ((ret = configure_output_video_filter(graph, cur)) < 0) {
                    avfilter_inout_free(&out);
                    return ret;
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                if ((ret = configure_output_audio_filter(graph, cur)) < 0) {
                    avfilter_inout_free(&out);
                    return ret;
                }
                break;
            default:
                break;
        }
    }
    avfilter_inout_free(&out);
    ret = avfilter_graph_config(graph->graph, NULL);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

FilterGraph* init_filtergraph(InputStream *ist, OutputStream *ost) {
    FilterGraph *graph = av_mallocz(sizeof(*graph));
    if (!graph)
        return NULL;
    graph->input = av_mallocz(sizeof(*graph->input));
    graph->output = av_mallocz(sizeof(*graph->output));
    graph->input->ist = ist;
    graph->input->graph = graph;
    graph->output->ost = ost;
    graph->output->graph = graph;
    ist->filter= graph->input;
    ost->filter = graph->output;
    return graph;
}

void log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    printf("fmt %s", fmt);
}

int get_buffer(AVCodecContext *s, AVFrame *frame, int flags) {
    return avcodec_default_get_buffer2(s, frame, flags);
}

int transcode_init() {
    int ret = 0;
    for (int i = 0; i < nb_output_streams; ++i) {
        InputStream *ist = input_streams[i];
        OutputStream *ost = output_streams[i];
        ost->st->discard = ist->st->discard;
        ost->enc_ctx->bits_per_raw_sample = ist->dec_ctx->bits_per_raw_sample;
        ost->enc_ctx->chroma_sample_location = ist->dec_ctx->chroma_sample_location;
        if (!ost->filter && (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO || ost->enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO)) {
            FilterGraph *graph = init_filtergraph(ist, ost);
            if ((ret = configure_filtergraph(graph)) < 0) {
                return ret;
            }
        }
        AVRational frame_rate = { 0, 0 };
        if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            frame_rate = av_buffersink_get_frame_rate(ost->filter->filter);
            if (!frame_rate.num) {
                frame_rate = (AVRational) { 25, 1 };
            }
        }
        switch (ost->enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                ost->enc_ctx->pix_fmt = (enum AVPixelFormat) ost->filter->filter->inputs[0]->format;
                ost->enc_ctx->width = ost->filter->filter->inputs[0]->w;
                ost->enc_ctx->height = ost->filter->filter->inputs[0]->h;
                ost->enc_ctx->time_base = av_inv_q(frame_rate);
                ost->st->avg_frame_rate = frame_rate;
                break;
            case AVMEDIA_TYPE_AUDIO:
                ost->enc_ctx->sample_fmt = (enum AVSampleFormat) ost->filter->filter->inputs[0]->format;
                ost->enc_ctx->sample_rate = ost->filter->filter->inputs[0]->sample_rate;
                ost->enc_ctx->channels = avfilter_link_get_channels(ost->filter->filter->inputs[0]);
                ost->enc_ctx->channel_layout = ost->filter->filter->inputs[0]->channel_layout;
                ost->enc_ctx->time_base = (AVRational) { 1, ost->enc_ctx->sample_rate };
                break;
            default:
                break;
        }
    }
    for (int i = 0; i < nb_input_streams; ++i) {
        InputStream *ist = input_streams[i];
        ist->dec_ctx->opaque = ist;
        ist->dec_ctx->get_buffer2 = get_buffer;
        if ((ret = avcodec_open2(ist->dec_ctx, ist->dec, NULL)) < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    for (int i = 0; i < nb_output_streams; ++i) {
        OutputStream *ost = output_streams[i];
        if ((ret = avcodec_open2(ost->enc_ctx, ost->enc, NULL)) < 0) {
            av_err2str(ret);
            return ret;
        }
        if ((ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx)) < 0) {
            av_err2str(ret);
            return ret;
        }
        ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational) { 0, 1 });
        ost->st->codec->codec = ost->enc_ctx->codec;
    }
    if ((ret = avformat_write_header(output_file->oc, NULL)) < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

int need_output() {
    for (int i = 0; i < nb_output_streams; ++i) {
        OutputStream *ost = output_streams[i];
        if (ost->finished) {
            continue;
        }
        return 1;
    }
    return 0;
}

int decode_video(InputStream *ist, AVPacket *pkt, int *got_output) {
    int ret = 0;
    AVFrame *frame = av_frame_alloc();
    ret = avcodec_decode_video2(ist->dec_ctx, frame, got_output, pkt);
    if (!*got_output || ret < 0) {
        return ret;
    }
    pkt->size = 0;
    if (ist->st->sample_aspect_ratio.num) {
        frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;
    }
    ret = av_buffersrc_add_frame_flags(ist->filter->filter, frame, AV_BUFFERSRC_FLAG_PUSH);
    if (ret != AVERROR_EOF && ret < 0) {
        return ret;
    }
    av_frame_unref(frame);
    return ret;
}

int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output) {
    int ret = 0;
    AVFrame *frame = av_frame_alloc();
    ret = avcodec_decode_audio4(ist->dec_ctx, frame, got_output, pkt);
    if (!*got_output || ret < 0) {
        return ret;
    }
    ret = av_buffersrc_add_frame_flags(ist->filter->filter, frame, AV_BUFFERSRC_FLAG_PUSH);
    if (ret == AVERROR_EOF) {
        ret = 0;
    } else if (ret < 0) {
        return ret;
    }
    av_frame_unref(frame);
    return ret;
}


int process_input_packet(InputStream *ist, AVPacket *pkt) {
    int ret = 0;
    AVPacket avpkt;
    if (!pkt) {
        av_init_packet(&avpkt);
        avpkt.size = 0;
        avpkt.data = NULL;
    } else {
        avpkt = *pkt;
    }
    int got_output = 0;
    AVFrame *frame = av_frame_alloc();
    switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            ret = decode_video(ist, &avpkt, &got_output);
            break;
        case AVMEDIA_TYPE_AUDIO:
            ret = decode_audio(ist, &avpkt, &got_output);
            break;
        default:
            break;
    }
    av_frame_unref(frame);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    if (!got_output && avpkt.size > 0) {
        return AVERROR_UNKNOWN;
    }
    if (!pkt && !got_output) {
        ret = av_buffersrc_add_frame(ist->filter->filter, NULL);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    return got_output;
}

int do_video_out(OutputStream *ost, AVFrame *next_picture) {
    int ret = 0;
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.size = 0;
    pkt.data = NULL;
    next_picture->pts = ost->sync_opts;
    next_picture->quality = ost->enc_ctx->global_quality;
    int got_output;
    ret = avcodec_encode_video2(ost->enc_ctx, &pkt, next_picture, &got_output);
    if (ret < 0) {
        av_frame_unref(next_picture);
        av_packet_unref(&pkt);
        av_err2str(ret);
        return ret;
    }
    if (got_output) {
        if (pkt.pts == AV_NOPTS_VALUE && !(ost->enc_ctx->codec->capabilities & AV_CODEC_CAP_DELAY)) {
            pkt.pts= ost->sync_opts;
        }
        av_packet_rescale_ts(&pkt, ost->enc_ctx->time_base, ost->st->time_base);
        pkt.stream_index = ost->source_index;
        ret = av_interleaved_write_frame(output_file->oc, &pkt);
        if (ret < 0) {
            av_packet_unref(&pkt);
            av_frame_unref(next_picture);
            av_err2str(ret);
            return ret;
        }
        av_packet_unref(&pkt);
    }
    ost->sync_opts++;
    av_frame_unref(next_picture);
    return ret;
}

int do_audio_out(OutputStream *ost, AVFrame *next_picture) {
    int ret = 0;
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.size = 0;
    pkt.data = NULL;
    int got_output;
    next_picture->pts = ost->sync_opts;
    ost->sync_opts = (uint64_t) (next_picture->pts + next_picture->nb_samples);
    ret = avcodec_encode_audio2(ost->enc_ctx, &pkt, next_picture, &got_output);
    if (ret < 0) {
        av_packet_unref(&pkt);
        av_frame_unref(next_picture);
        av_err2str(ret);
        return ret;
    }
    if (got_output) {
        av_packet_rescale_ts(&pkt, ost->enc_ctx->time_base, ost->st->time_base);
        pkt.stream_index = ost->source_index;
        ret = av_interleaved_write_frame(output_file->oc, &pkt);
        if (ret < 0) {
            av_err2str(ret);
            av_packet_unref(&pkt);
            av_frame_unref(next_picture);
            return ret;
        }
    }
    av_frame_unref(next_picture);
    return ret;
}

int reap_filters() {
    int ret = 0;
    for (int i = 0; i < nb_output_streams; ++i) {
        OutputStream *ost = output_streams[i];
        AVFrame *frame = av_frame_alloc();
        while (1) {
            ret = av_buffersink_get_frame_flags(ost->filter->filter, frame, AV_BUFFERSINK_FLAG_NO_REQUEST);
            if (ret < 0) {
                av_err2str(ret);
                break;
            }
            if (ost->finished) {
                av_frame_unref(frame);
                continue;
            }
            switch (ost->filter->filter->inputs[0]->type) {
                case AVMEDIA_TYPE_VIDEO:
                    ost->enc_ctx->sample_aspect_ratio = frame->sample_aspect_ratio;
                    ret = do_video_out(ost, frame);
                    if (ret < 0) {
                        av_frame_unref(frame);
                        return ret;
                    }
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    if (!(ost->enc_ctx->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
                            ost->enc_ctx->channels != av_frame_get_channels(frame)) {
                        break;
                    }
                    ret = do_audio_out(ost, frame);
                    if (ret < 0) {
                        av_frame_unref(frame);
                        return ret;
                    }
                    break;
                default:
                    break;
            }
        }
        av_frame_unref(frame);
    }
    return 0;
}

int flush_encoders() {
    int ret = 0;
    for (int i = 0; i < nb_output_streams; ++i) {
        OutputStream *ost = output_streams[i];
        if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO && ost->enc_ctx->frame_size <= 1) {
            continue;
        }
        int stop_encoding;
        while (1) {
            int (*encode) (AVCodecContext*, AVPacket*, const AVFrame*, int*) = NULL;
            switch (ost->enc_ctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    encode = avcodec_encode_video2;
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    encode = avcodec_encode_audio2;
                    break;
                default:
                    break;
            }
            if (encode) {
                AVPacket pkt;
                av_init_packet(&pkt);
                pkt.size = 0;
                pkt.data = NULL;
                int got_output;
                ret = encode(ost->enc_ctx, &pkt, NULL, &got_output);
                if (ret < 0) {
                    av_packet_unref(&pkt);
                    av_err2str(ret);
                    return ret;
                }
                if (!got_output) {
                    av_packet_unref(&pkt);
                    stop_encoding = 1;
                    break;
                }
                av_packet_rescale_ts(&pkt, ost->enc_ctx->time_base, ost->st->time_base);
                pkt.stream_index = ost->source_index;
                ret = av_interleaved_write_frame(output_file->oc, &pkt);
                if (ret < 0) {
                    av_err2str(ret);
                    av_packet_unref(&pkt);
                    return ret;
                }
                av_packet_unref(&pkt);
            }
            if (stop_encoding) {
                break;
            }
        }
    }
    return ret;
}

int transcode() {
    int ret = 0;
    if (transcode_init() < 0) {
        return ret;
    }
    OutputStream *ost = NULL;
    while (need_output()) {
        for (int i = 0; i < nb_output_streams; i++) {
            if (!output_streams[i]->finished) {
                ost = output_streams[i];
                break;
            }
        }
        if (ost->filter) {
            ret = avfilter_graph_request_oldest(ost->filter->graph->graph);
            if (ret == AVERROR_EOF) {
                ost->finished = 1;
                continue;
            } else if (ret != AVERROR(EAGAIN)) {
                continue;
            }
        }
        AVPacket pkt;
        ret = av_read_frame(input_file->ic, &pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            for (int i = 0; i < nb_input_streams; ++i) {
                InputStream *ist = input_streams[i];
                ret = process_input_packet(ist, NULL);
                if (ret > 0) {
                    reap_filters();
                    break;
                }
            }
            continue;
        }
        av_pkt_dump_log2(NULL, AV_LOG_ERROR, &pkt, 0, input_file->ic->streams[pkt.stream_index]);
        process_input_packet(input_streams[pkt.stream_index], &pkt);
        av_packet_unref(&pkt);
        reap_filters();
    }
    for (int i = 0; i < nb_input_streams; ++i) {
        process_input_packet(input_streams[i], NULL);
    }
    flush_encoders();
    ret = av_write_trailer(output_file->oc);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    return ret;
}

void release() {
    if (input_file) {
        avformat_close_input(&input_file->ic);
        avformat_free_context(input_file->ic);
        av_free(input_file);
    }
    if (output_file) {
        avformat_free_context(output_file->oc);
        av_free(output_file);
    }

    for (int i = 0; i < nb_input_streams; ++i) {
        InputStream *ist = input_streams[i];
        avcodec_close(ist->dec_ctx);
        avcodec_free_context(&ist->dec_ctx);
    }

    for (int i = 0; i < nb_output_streams; ++i) {
        OutputStream *ost = output_streams[i];
        avcodec_close(ost->enc_ctx);
        avcodec_free_context(&ost->enc_ctx);
        av_free(ost->avfilter);
    }
}

int open_files(const char *input_path, const char *output_path, int new_width, int new_height) {
    int ret = 0;
    av_register_all();
    avcodec_register_all();
    avfilter_register_all();
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(log_callback);
    ret = open_input_file(input_path);
    if (ret < 0) {
        release();
        return ret;
    }
    ret = open_output_file(output_path, new_width, new_height);
    if (ret < 0) {
        release();
        return ret;
    }
    return ret;
}