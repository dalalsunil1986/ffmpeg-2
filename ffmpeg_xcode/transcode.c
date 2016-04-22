//
//  transcode.c
//  ffmpeg_xcode
//
//  Created by wlanjie on 16/4/7.
//  Copyright © 2016年 com.wlanjie.ffmpeg. All rights reserved.
//

#include "transcode.h"

static int init_input_stream(int ist_index) {
    int ret;
    InputStream *ist = input_streams[ist_index];
    if (ist->decoding_needed) {
        AVCodec *codec = ist->dec;
        if (!codec) {
            av_log(NULL, AV_LOG_ERROR, "Decoder (codec %s) not found for input stream #%d:%d",
                   avcodec_get_name(ist->dec_ctx->codec_id), ist->file_index, ist->st->index);
            return AVERROR(EINVAL);
        }
        ist->dec_ctx->opaque = ist;
        ist->dec_ctx->thread_safe_callbacks = 1;
        av_opt_set_int(ist->dec_ctx, "refcounted_frames", 1, 0);
        if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0)) {
            av_dict_set(&ist->decoder_opts, "threads", "auto", 0);
        }
        if ((ret = avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts)) < 0) {
            av_err2str(ret);
            return ret;
        }
    }
//    ist->next_pts = AV_NOPTS_VALUE;
//    ist->next_dts = AV_NOPTS_VALUE;
    return ret;
}

static int init_output_stream(int ost_index) {
    int ret;
    OutputStream *ost = output_streams[ost_index];
    if (ost->encoding_needed) {
        AVCodec *codec = ost->enc;
        AVCodecContext *dec;
        InputStream *ist;
        if (ost->source_index >= 0) {
            ist = input_streams[ost->source_index];
            dec = ist->dec_ctx;
        }
        if (ist == NULL) {
            av_log(NULL, AV_LOG_ERROR, "Could not find InputStream.\n");
            return AVERROR(ENOMEM);
        }
        if (!av_dict_get(ost->encoder_opts, "threads", NULL, 0)) {
            av_dict_set(&ost->encoder_opts, "threads", "auto", 0);
        }
        if ((ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts)) < 0) {
            av_err2str(ret);
            return ret;
        }
        ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx);
        if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
        if (ost->enc_ctx->nb_coded_side_data) {
            ost->st->side_data = av_realloc_array(NULL, ost->enc_ctx->nb_coded_side_data, sizeof(*ost->st->side_data));
            if (!ost->st->side_data) {
                av_log(NULL, AV_LOG_ERROR, "Could not alloc side data.\n");
                return AVERROR(ENOMEM);
            }
            for (int i = 0; i < ost->enc_ctx->nb_coded_side_data; i++) {
                const AVPacketSideData *sd_src = &ost->enc_ctx->coded_side_data[i];
                AVPacketSideData *sd_dst = &ost->st->side_data[i];
                sd_dst->data = av_malloc(sd_src->size);
                if (!sd_dst->data) {
                    av_log(NULL, AV_LOG_ERROR, "Could not malloc side data.\n");
                    return AVERROR(ENOMEM);
                }
                memcpy(sd_dst->data, sd_src->data, sd_src->size);
                sd_dst->size = sd_src->size;
                sd_dst->type = sd_src->type;
                ost->st->nb_side_data++;
            }
        }
        // copy timebase while removing common factors
        ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational){0, 1});
        ost->st->codec->codec= ost->enc_ctx->codec;
    }
    return ret;
}

int transcode_init(void) {
    int ret = 0;
    for (int i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        InputStream *ist;
        if (ost->source_index >= 0) {
            ist = input_streams[ost->source_index];
        }
        AVCodecContext *enc_ctx = ost->enc_ctx;
        AVCodecContext *dec_ctx;
        if (ist) {
            dec_ctx = ist->dec_ctx;
            ost->st->disposition = ist->st->disposition;
            enc_ctx->bits_per_raw_sample = dec_ctx->bits_per_raw_sample;
            enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;
        }
        if (!ost->enc) {
            ost->enc = avcodec_find_encoder(enc_ctx->codec_id);
        }
        if (!ost->enc) {
            av_log(NULL, AV_LOG_ERROR, "AVCodec is NULL.\n");
            return AVERROR(EINVAL);
        }
        if (!ost->filter && (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO || enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO)) {
            FilterGraph *fg = init_simple_filtergraph(ist, ost);
            if (fg == NULL) {
                return AVERROR(ENOMEM);
            }
            if (configure_filtergraph(fg)) {
                return AVERROR(ENOMEM);
            }
        }
        if (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (!ost->frame_rate.num) {
                ost->frame_rate = av_buffersink_get_frame_rate(ost->filter->filter);
            }
            if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4) {
                av_reduce(&ost->frame_rate.num, &ost->frame_rate.den, ost->frame_rate.num, ost->frame_rate.den, 65535);
            }
        }
        switch (enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                enc_ctx->time_base = av_inv_q(ost->frame_rate);
                enc_ctx->width = ost->filter->filter->inputs[0]->w;
                enc_ctx->height = ost->filter->filter->inputs[0]->h;
                enc_ctx->sample_aspect_ratio = ost->st->sample_aspect_ratio;
                enc_ctx->pix_fmt = ost->filter->filter->inputs[0]->format;
                ost->st->avg_frame_rate = ost->frame_rate;
                break;

            case AVMEDIA_TYPE_AUDIO:
                enc_ctx->sample_fmt = ost->filter->filter->inputs[0]->format;
                enc_ctx->sample_rate = ost->filter->filter->inputs[0]->sample_rate;
                enc_ctx->channel_layout = ost->filter->filter->inputs[0]->channel_layout;
                enc_ctx->channels = avfilter_link_get_channels(ost->filter->filter->inputs[0]);
                enc_ctx->time_base = (AVRational) {1, enc_ctx->sample_rate};
                break;
            default:
                break;
        }
    }
    /* init input streams **/
    for (int i = 0; i < nb_input_streams; i++) {
        if ((ret = init_input_stream(i)) < 0) {
            for (int j = 0; j < nb_output_streams; j++) {
                avcodec_close(output_streams[j]->enc_ctx);
            }
        }
    }

    for (int i = 0; i < nb_output_streams; i++) {
        init_output_stream(i);
    }
    for (int i = 0; i < nb_output_files; i++) {
        AVFormatContext *oc = output_files[i]->ctx;
        //            oc->interrupt_callback = int_cb;
        if ((ret = avformat_write_header(oc, &output_files[i]->opts)) < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    return ret;
}

static OutputStream *choose_output(void) {
    int i;
    int64_t opts_min = INT64_MAX;
    OutputStream *ost_min = NULL;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        int64_t opts = ost->st->cur_dts == AV_NOPTS_VALUE ? INT64_MIN :
        av_rescale_q(ost->st->cur_dts, ost->st->time_base,
                     AV_TIME_BASE_Q);
        if (ost->st->cur_dts == AV_NOPTS_VALUE)
            av_log(NULL, AV_LOG_DEBUG, "cur_dts is invalid (this is harmless if it occurs once at the start per stream)\n");

        if (!ost->finished && opts < opts_min) {
            opts_min = opts;
            ost_min  = ost;
        }
    }
    return ost_min;
}

static int write_frame(AVFormatContext *s, AVPacket *pkt, OutputStream *ost) {
    int ret;
    AVCodecContext *avctx = ost->encoding_needed ? ost->enc_ctx : ost->st->codec;
    if (!(avctx->codec_type == AVMEDIA_TYPE_VIDEO && avctx->codec)) {
        if (ost->frame_number >= ost->max_frames) {
            av_packet_unref(pkt);
        }
        ost->frame_number++;
    }
    ost->data_size += pkt->size;
    ost->last_mux_dts = pkt->dts;
    pkt->stream_index = ost->index;
    if ((ret = av_apply_bitstream_filters(avctx, pkt, ost->bitstream_filters)) < 0) {
        av_err2str(ret);
        return ret;
    }
    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    av_packet_unref(pkt);
    return ret;
}

static int do_video_out(AVFormatContext *s, OutputStream *ost, AVFrame *next_picure, double sync_ipts) {
    int ret;
    AVPacket pkt;
    AVFrame *in_picture = next_picure;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    if (!in_picture) {
        return AVERROR(ENOMEM);
    }
    in_picture->pts = ost->sync_opts;
    int got_packet;
    AVCodecContext *enc = ost->enc_ctx;
    AVCodecContext *mux_enc = ost->st->codec;
    if (!in_picture) {
        return AVERROR(ENOMEM);
    }
    in_picture->pts = ost->sync_opts;
    mux_enc->field_order = AV_FIELD_PROGRESSIVE;
    in_picture->quality = enc->global_quality;
    in_picture->pict_type = 0;
    ost->frame_encoded++;

    ret = avcodec_encode_video2(enc, &pkt, in_picture, &got_packet);
    if (ret < 0) {
        av_err2str(ret);
        return ret;
    }
    if (got_packet) {
        if (pkt.pts == AV_NOPTS_VALUE && !(enc->codec->capabilities & AV_CODEC_CAP_DELAY)) {
            pkt.pts = ost->sync_opts;
        }
        av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);
        write_frame(s, &pkt, ost);
    }
    ost->sync_opts++;
    ost->frame_number++;
    return ret;
}

static int do_audio_out(AVFormatContext *s, OutputStream *ost, AVFrame *frame) {
    int ret;
//    AVPacket pkt;
//    av_init_packet(&pkt);
//    pkt.data = NULL;
//    pkt.size = 0;
//
//    frame->pts = ost->sync_opts;
//    ost->sync_opts = frame->pts + frame->nb_samples;
//    ost->frame_encoded++;
//    int got_packet = 0;
//    if ((ret = avcodec_encode_audio2(ost->enc_ctx, &pkt, frame, &got_packet)) < 0) {
//        av_err2str(ret);
//        return ret;
//    }
//    if (got_packet) {
//        av_packet_rescale_ts(&pkt, ost->enc_ctx->time_base, ost->st->time_base);
//        write_frame(s, &pkt, ost);
//    }
    return ret;
}

static int reap_filters(int flush) {
    int ret = 0;
    AVFrame *filtered_frame = NULL;
    for (int i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        OutputFile *of = output_files[ost->file_index];
        AVFilterContext *filter;
        AVCodecContext *enc = ost->enc_ctx;
        if (!ost->filter) {
            continue;
        }
        filter = ost->filter->filter;
        if (!ost->filtered_frame && !(ost->filtered_frame = av_frame_alloc())) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc filtered frame.\n");
            return AVERROR(ENOMEM);
        }
        filtered_frame = ost->filtered_frame;
        while (1) {
            double float_pts = AV_NOPTS_VALUE;
            ret = av_buffersink_get_frame_flags(filter, filtered_frame, AV_BUFFERSINK_FLAG_NO_REQUEST);
            if (ret < 0) {
                av_err2str(ret);
                char error[255] = {0};
                av_make_error_string(error, 128, ret);
                if (flush && ret == AVERROR_EOF && filter->inputs[0]->type == AVMEDIA_TYPE_VIDEO) {
                    do_video_out(of->ctx, ost, NULL, AV_NOPTS_VALUE);
                }
                break;
            }
            if (ost->finished) {
                av_frame_unref(filtered_frame);
                continue;
            }

            switch (filter->inputs[0]->type) {
                case AVMEDIA_TYPE_VIDEO:
                    enc->sample_aspect_ratio = filtered_frame->sample_aspect_ratio;
                    do_video_out(of->ctx, ost, filtered_frame, float_pts);
                    break;

                case AVMEDIA_TYPE_AUDIO:
                    if (!(enc->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) && enc->channels != av_frame_get_channels(filtered_frame)) {
                        break;
                    }
                    do_audio_out(of->ctx, ost, filtered_frame);
                    break;
                default:
                    break;
            }
            av_frame_unref(filtered_frame);
        }
    }
//    return ret;
    return 0;
}

static int volatile num_ = 0;

static int transcode_from_filter(FilterGraph *graph, InputStream **best_ist) {
    int ret;
    int nb_requests_max = 0, nb_requests;
    *best_ist = NULL;
    ret = avfilter_graph_request_oldest(graph->graph);
    if (ret >= 0) {
        return reap_filters(0);
    }

    av_log(NULL, AV_LOG_ERROR, "ret = %d\n", ret);
    if (ret == AVERROR_EOF) {
        ret = reap_filters(1);
        for (int i = 0; i < graph->nb_outputs; i++) {
            OutputStream *ost = graph->outputs[i]->ost;
            av_log(NULL, AV_LOG_ERROR, "-------------- type = %d i = %d outputs = %d num = %d\n", graph->outputs[i]->ost->enc_ctx->codec_type, i, graph->nb_outputs, num_);
            //这里的finished 不一样
            ost->finished = ENCODER_FINISHED;
        }
        return ret;
    }
    num_++;
    if (ret != AVERROR(EAGAIN)) {
        return ret;
    }

    for (int i = 0; i < graph->nb_inputs; i++) {
        InputFilter *iflter = graph->inputs[i];
        InputStream *ist = iflter->ist;
        nb_requests = av_buffersrc_get_nb_failed_requests(iflter->filter);
        if(nb_requests > nb_requests_max) {
            nb_requests_max = nb_requests;
            *best_ist = ist;
        }
    }
    return 0;
}

static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output) {
    int ret, err = 0;
    AVFrame *decoded_frame;
    AVCodecContext *avctx = ist->dec_ctx;
    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not audio alloc frame.\n");
        return AVERROR(ENOMEM);
    }
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not audio alloc filter frame.\n");
        return AVERROR(ENOMEM);
    }
    decoded_frame = ist->decoded_frame;
    ret = avcodec_decode_audio4(avctx, decoded_frame, got_output, pkt);
    if (!*got_output || ret < 0) {
        av_err2str(ret);
        return ret;
    }
    for (int i = 0; i < ist->nb_filters; i++) {
        err = av_buffersrc_add_frame_flags(ist->filters[i]->filter, decoded_frame, AV_BUFFERSRC_FLAG_PUSH);
        if (err == AVERROR_EOF) {
            err = 0;
        }
        if (err < 0) {
            av_err2str(ret);
            break;
        }
    }
    decoded_frame->pts = AV_NOPTS_VALUE;
    av_frame_unref(ist->filter_frame);
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int decode_video(InputStream *ist, AVPacket *pkt , int *got_output) {
    int ret, err = 0;
    AVFrame *decoded_frame, *f;
    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not video alloc frame.\n");
        return AVERROR(ENOMEM);
    }
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not video filter frame.\n");
        return AVERROR(ENOMEM);
    }
    decoded_frame = ist->decoded_frame;
    pkt->dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ist->st->time_base);
    if ((ret = avcodec_decode_video2(ist->dec_ctx, decoded_frame, got_output, pkt) < 0)) {
        av_err2str(ret);
        return ret;
    }
    if (!*got_output || ret < 0) {
        return ret;
    }
    int64_t best_effort_timestamp = av_frame_get_best_effort_timestamp(decoded_frame);
    if (best_effort_timestamp != AV_NOPTS_VALUE) {
        int64_t ts = av_rescale_q(decoded_frame->pts = best_effort_timestamp, ist->st->time_base, AV_TIME_BASE_Q);
        if (ts != AV_NOPTS_VALUE) {
            ist->next_pts = ist->pts = ts;
        }
    }
    pkt->size = 0;
    if (ist->st->sample_aspect_ratio.num) {
        decoded_frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;
    }
    AVRational *frame_sample_aspect = av_opt_ptr(avcodec_get_frame_class(), decoded_frame, "sample_aspect_ratio");
    for (int i = 0; i < ist->nb_filters; i++) {
        if (!frame_sample_aspect->num) {
            *frame_sample_aspect = ist->st->sample_aspect_ratio;
        }
        if (i < ist->nb_filters - 1) {
            f = ist->filter_frame;
            if ( (err = av_frame_ref(f, decoded_frame) < 0))
                break;
        } else {
            f = decoded_frame;
        }
        ret = av_buffersrc_add_frame_flags(ist->filters[i]->filter, f, AV_BUFFERSRC_FLAG_PUSH);
        if (ret == AVERROR_EOF) {
            ret = 0;
        } else if (ret < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    av_frame_unref(ist->filter_frame);
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int volatile input_num = 0;
static int process_input_packet(InputStream *ist, const AVPacket *pkt, int no_eof) {
    int ret = 0;
    AVPacket avpkt;
    if (!pkt) {
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
           goto handle_eof;
    } else {
        avpkt = *pkt;
        if (pkt->dts != AV_NOPTS_VALUE)
            ist->next_dts = ist->dts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);
    }
    int got_output = 0;
    while ((avpkt.size > 0 || (!pkt && got_output))) {
        int64_t duration = 0;
            handle_eof:
        ist->pts = ist->next_pts;
        ist->dts = ist->next_dts;
        switch (ist->dec_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                ret = decode_video(ist, &avpkt, &got_output);
                if (avpkt.duration) {
                    duration = av_rescale_q(avpkt.duration, ist->st->time_base, AV_TIME_BASE_Q);
                }
                if (ist->dts != AV_NOPTS_VALUE && duration) {
                    ist->next_dts += duration;
                }
                if (got_output) {
                    ist->next_pts += duration;
                }
                break;

            case AVMEDIA_TYPE_AUDIO:
                ret = decode_audio(ist, &avpkt, &got_output);
                break;
            default:
                break;
        }
        avpkt.dts = avpkt.pts = AV_NOPTS_VALUE;
        if (pkt) {
            if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_AUDIO) {
                ret = avpkt.size;
            }
            avpkt.data += ret;
            avpkt.size -= ret;
        }
        if (!got_output) {
            continue;
        }
        if (got_output && !pkt) {
            break;
        }
    }

    if (!pkt && ist->decoding_needed && !got_output && !no_eof) {
        av_log(NULL, AV_LOG_ERROR, "send filter eof NULL %d\n", input_num);
        for (int i = 0; i < ist->nb_filters; i++) {
            ret = av_buffersrc_add_frame(ist->filters[i]->filter, NULL);
            if (ret < 0) {
                return ret;
            }
        }
    }
    input_num++;
    return got_output;
}

static int process_input(int file_index) {
    int ret;
    InputFile *ifile = input_files[file_index];
    AVPacket pkt;
    ret = av_read_frame(ifile->ctx, &pkt);
    if (ret < 0) {
//        for (int i = 0; i < nb_output_streams; i++) {
//            output_streams[i]->finished = 1;
//        }
        av_err2str(ret);
        for (int i = 0; i < input_files[file_index]->nb_streams; i++) {
            InputStream *ist = input_streams[input_files[file_index]->ist_index + i];
            ret = process_input_packet(ist, NULL, 0);
            if (ret > 0) {
                return 0;
            }
        }
        return AVERROR(EAGAIN);
    }
    av_pkt_dump_log2(NULL, AV_LOG_ERROR, &pkt, 0, ifile->ctx->streams[pkt.stream_index]);
    InputStream *ist = input_streams[ifile->ist_index + pkt.stream_index];
    ist->data_size = pkt.size;
    ist->nb_packets++;
    if (ist->discard) {
        av_packet_unref(&pkt);
    }
    process_input_packet(ist, &pkt, 0);
    return ret;
}

static int transcode_step(void) {
    int ret;
    OutputStream *ost = choose_output();
    InputStream *ist;
    if (ost->filter) {
        if ((ret = transcode_from_filter(ost->filter->graph, &ist)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not transcode from filter.\n");
            return ret;
        }
        if (!ist) {
            return 0;
        }
    } else {
        ist = input_streams[ost->source_index];
    }
    ret = process_input(ist->file_index);
    if (ret == AVERROR(EAGAIN)) {
        return 0;
    }
    return reap_filters(0);
}

static int need_output(void) {
    for (int i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        if (ost->finished) {
            continue;
        }
        return 1;
    }
    return 0;
}

int transcode(void) {
    int ret;
    ret = transcode_init();
    if (ret < 0) {

    }
    static int volatile decoded_num = 0;
    while (1) {

        ret = need_output();
        if (!ret) {
            break;
        }
        ret = transcode_step();

        av_log(NULL, AV_LOG_ERROR, "decoded_num = %d ret = %d\n", decoded_num, ret);
        decoded_num++;
    }
    av_log(NULL, AV_LOG_ERROR, "transcode done.\n");
//    flush_encoders();
    for (int i = 0; i < nb_output_files; i++) {
        AVFormatContext *os = output_files[i]->ctx;
        if ((ret = av_write_trailer(os)) < 0) {
            av_err2str(ret);
            return ret;
        }
    }
    for (int i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        if (ost->encoding_needed) {
            av_freep(&ost->enc_ctx->stats_in);
        }
    }
    for (int i = 0; i < nb_input_streams; i++) {
        InputStream *ist = input_streams[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->dec_ctx);
        }
    }

    for (int i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        avfilter_graph_free(&fg->graph);
        for (int j = 0; j < fg->nb_inputs; j++) {
            av_freep(&fg->inputs[j]->name);
            av_freep(&fg->inputs[j]);
        }
        av_freep(&fg->inputs);
        for (int j = 0; j < fg->nb_outputs; j++) {
            av_freep(&fg->outputs[j]->name);
            av_freep(&fg->outputs);
        }
        av_freep(&fg->outputs);
        av_freep(&fg->graph_desc);
    }
    for (int i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        if (!ost) {
            continue;
        }
        AVBitStreamFilterContext *bsfc = ost->bitstream_filters;
        while (bsfc) {
            AVBitStreamFilterContext *next = bsfc->next;
            av_bitstream_filter_close(bsfc);
            bsfc = next;
        }
        ost->bitstream_filters = NULL;
        av_frame_free(&ost->filtered_frame);
//        av_freep(&ost->avfilter);
        avcodec_free_context(&ost->enc_ctx);
        av_dict_free(&ost->encoder_opts);
        av_freep(&output_streams[i]);
    }
    for (int i = 0; i < nb_input_files; i++) {
        avformat_close_input(&input_files[i]->ctx);
        av_freep(&input_files[i]);
    }
    av_freep(&input_streams);
    av_freep(&input_files);
    av_freep(&output_streams);
    av_freep(&output_files);
    return ret;
}
