/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * multimedia converter based on the FFmpeg libraries
 */

#include "filter.h"
#include "ffmpeg.h"

#include "libavutil/avassert.h"

const char program_name[] = "ffmpeg";
const int program_birth_year = 2000;

const char *const forced_keyframes_const_names[] = {
    "n",
    "n_forced",
    "prev_forced_n",
    "prev_forced_t",
    "t",
    NULL
};

static int decode_interrupt_cb(void *ctx)
{
    return received_nb_signals > transcode_init_done;
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static void write_frame(AVFormatContext *s, AVPacket *pkt, OutputStream *ost)
{
    AVBitStreamFilterContext *bsfc = ost->bitstream_filters;
    AVCodecContext          *avctx = ost->encoding_needed ? ost->enc_ctx : ost->st->codec;
    int ret;
    
    /*
     * Audio encoders may split the packets --  #frames in != #packets out.
     * But there is no reordering, so we can limit the number of output packets
     * by simply dropping them here.
     * Counting encoded video frames needs to be done separately because of
     * reordering, see do_video_out()
     */
    if (!(avctx->codec_type == AVMEDIA_TYPE_VIDEO && avctx->codec)) {
        if (ost->frame_number >= ost->max_frames) {
            av_packet_unref(pkt);
            return;
        }
        ost->frame_number++;
    }
    
    if (bsfc)
        av_packet_split_side_data(pkt);
    
    if ((ret = av_apply_bitstream_filters(avctx, pkt, bsfc)) < 0) {
    }
    ost->last_mux_dts = pkt->dts;
    
    pkt->stream_index = ost->index;
    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
    }
    av_packet_unref(pkt);
}

static void close_output_stream(OutputStream *ost)
{
    ost->finished = ENCODER_FINISHED;
}

static void do_audio_out(AVFormatContext *s, OutputStream *ost,
                         AVFrame *frame)
{
    AVCodecContext *enc = ost->enc_ctx;
    AVPacket pkt;
    int got_packet = 0;
    
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
 
    frame->pts = ost->sync_opts;
    ost->sync_opts = frame->pts + frame->nb_samples;
    if (avcodec_encode_audio2(enc, &pkt, frame, &got_packet) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Audio encoding failed (avcodec_encode_audio2)\n");
    }
    
    if (got_packet) {
        av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);
        write_frame(s, &pkt, ost);
    }
}

static void do_video_out(AVFormatContext *s,
                         OutputStream *ost,
                         AVFrame *next_picture,
                         double sync_ipts)
{
    int ret;
    AVPacket pkt;
    AVCodecContext *enc = ost->enc_ctx;
    AVCodecContext *mux_enc = ost->st->codec;
    int nb_frames,i;
    int frame_size = 0;
    /* duplicates frame if needed */
    for (i = 0; i < nb_frames; i++) {
        AVFrame *in_picture;
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;
        
        in_picture = next_picture;
        
        if (!in_picture)
            return;
        
        in_picture->pts = ost->sync_opts;
        
#if FF_API_LAVF_FMT_RAWPICTURE
        if (s->oformat->flags & AVFMT_RAWPICTURE &&
            enc->codec->id == AV_CODEC_ID_RAWVIDEO) {
            
        } else
#endif
        {
            int got_packet;
            mux_enc->field_order = AV_FIELD_PROGRESSIVE;
            
            in_picture->quality = enc->global_quality;
            in_picture->pict_type = 0;
            
            ret = avcodec_encode_video2(enc, &pkt, in_picture, &got_packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_FATAL, "Video encoding failed\n");
            }
            
            if (got_packet) {
                if (pkt.pts == AV_NOPTS_VALUE && !(enc->codec->capabilities & AV_CODEC_CAP_DELAY))
                    pkt.pts = ost->sync_opts;
                
                av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);
                frame_size = pkt.size;
                write_frame(s, &pkt, ost);
            }
        }
        ost->sync_opts++;
        /*
         * For video, number of frames in == number of packets out.
         * But there may be reordering, so we can't throw away frames on encoder
         * flush, we need to limit them here, before they go into encoder.
         */
        ost->frame_number++;
    }
    
    av_frame_unref(next_picture);
}
/**
 * Get and encode new output from any of the filtergraphs, without causing
 * activity.
 *
 * @return  0 for success, <0 for severe errors
 */
static int reap_filters(int flush)
{
    AVFrame *filtered_frame = NULL;
    int i;
    
    /* Reap all buffers present in the buffer sinks */
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        OutputFile    *of = output_files[0];
        AVFilterContext *filter;
        AVCodecContext *enc = ost->enc_ctx;
        int ret = 0;
        
        if (!ost->filter)
            continue;
        filter = ost->filter->filter;
        
        if (!ost->filtered_frame && !(ost->filtered_frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
        filtered_frame = ost->filtered_frame;
        
        while (1) {
            double float_pts = AV_NOPTS_VALUE; // this is identical to filtered_frame.pts but with higher precision
            ret = av_buffersink_get_frame_flags(filter, filtered_frame,
                                                AV_BUFFERSINK_FLAG_NO_REQUEST);
            if (ret < 0) {
                if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                } else if (flush && ret == AVERROR_EOF) {
                    if (filter->inputs[0]->type == AVMEDIA_TYPE_VIDEO)
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
                    if (!(enc->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
                        enc->channels != av_frame_get_channels(filtered_frame)) {
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
    
    return 0;
}

static void flush_encoders(void)
{
    int i, ret;
    
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream   *ost = output_streams[i];
        AVCodecContext *enc = ost->enc_ctx;
        AVFormatContext *os = output_files[ost->file_index]->ctx;
        int stop_encoding = 0;
        
        if (!ost->encoding_needed)
            continue;
        
        if (enc->codec_type == AVMEDIA_TYPE_AUDIO && enc->frame_size <= 1)
            continue;
#if FF_API_LAVF_FMT_RAWPICTURE
        if (enc->codec_type == AVMEDIA_TYPE_VIDEO && (os->oformat->flags & AVFMT_RAWPICTURE) && enc->codec->id == AV_CODEC_ID_RAWVIDEO)
            continue;
#endif
        
        for (;;) {
            int (*encode)(AVCodecContext*, AVPacket*, const AVFrame*, int*) = NULL;
            const char *desc;
            
            switch (enc->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    encode = avcodec_encode_audio2;
                    desc   = "audio";
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    encode = avcodec_encode_video2;
                    desc   = "video";
                    break;
                default:
                    stop_encoding = 1;
            }
            
            if (encode) {
                AVPacket pkt;
                int pkt_size;
                int got_packet;
                av_init_packet(&pkt);
                pkt.data = NULL;
                pkt.size = 0;
                
                ret = encode(enc, &pkt, NULL, &got_packet);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_FATAL, "%s encoding failed: %s\n",
                           desc,
                           av_err2str(ret));
                }
                if (!got_packet) {
                    stop_encoding = 1;
                    break;
                }
                if (ost->finished & MUXER_FINISHED) {
                    av_packet_unref(&pkt);
                    continue;
                }
                av_packet_rescale_ts(&pkt, enc->time_base, ost->st->time_base);
                pkt_size = pkt.size;
                write_frame(os, &pkt, ost);
            }
            
            if (stop_encoding)
                break;
        }
    }
}

int guess_input_channel_layout(InputStream *ist)
{
    AVCodecContext *dec = ist->dec_ctx;
    
    if (!dec->channel_layout) {
        char layout_name[256];
        
        dec->channel_layout = av_get_default_channel_layout(dec->channels);
        if (!dec->channel_layout)
            return 0;
        av_get_channel_layout_string(layout_name, sizeof(layout_name),
                                     dec->channels, dec->channel_layout);
    }
    return 1;
}

static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output)
{
    AVFrame *decoded_frame;
    AVCodecContext *avctx = ist->dec_ctx;
    int i, ret, err = 0;
    AVRational decoded_frame_tb;
    decoded_frame = av_frame_alloc();
    
    ret = avcodec_decode_audio4(avctx, decoded_frame, got_output, pkt);
    
    if (ret >= 0 && avctx->sample_rate <= 0) {
        ret = AVERROR_INVALIDDATA;
    }
    
    if (!*got_output || ret < 0)
        return ret;

    /* if the decoder provides a pts, use it instead of the last packet pts.
     the decoder could be delaying output by a packet or more. */
    if (decoded_frame->pts != AV_NOPTS_VALUE) {
        ist->dts = ist->next_dts = ist->pts = ist->next_pts = av_rescale_q(decoded_frame->pts, avctx->time_base, AV_TIME_BASE_Q);
        decoded_frame_tb   = avctx->time_base;
    } else if (decoded_frame->pkt_pts != AV_NOPTS_VALUE) {
        decoded_frame->pts = decoded_frame->pkt_pts;
        decoded_frame_tb   = ist->st->time_base;
    } else if (pkt->pts != AV_NOPTS_VALUE) {
        decoded_frame->pts = pkt->pts;
        decoded_frame_tb   = ist->st->time_base;
    }else {
        decoded_frame->pts = ist->dts;
        decoded_frame_tb   = AV_TIME_BASE_Q;
    }
    pkt->pts           = AV_NOPTS_VALUE;
    err = av_buffersrc_add_frame_flags(ist->filters[i]->filter, decoded_frame, AV_BUFFERSRC_FLAG_PUSH);
    if (err == AVERROR_EOF)
        err = 0; /* ignore */
    decoded_frame->pts = AV_NOPTS_VALUE;
    
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int decode_video(InputStream *ist, AVPacket *pkt, int *got_output)
{
    AVFrame *decoded_frame;
    int ret = 0, err = 0;
    decoded_frame = av_frame_alloc();
    pkt->dts  = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ist->st->time_base);
    
    ret = avcodec_decode_video2(ist->dec_ctx, decoded_frame, got_output, pkt);
    if (!*got_output || ret < 0)
        return ret;
    pkt->size = 0;
    if (ist->st->sample_aspect_ratio.num)
        decoded_frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;
    ret = av_buffersrc_add_frame_flags(ist->filters[0]->filter, decoded_frame, AV_BUFFERSRC_FLAG_PUSH);
    if (ret == AVERROR_EOF) {
        ret = 0; /* ignore */
    } else if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Failed to inject frame into filter network: %s\n", av_err2str(ret));
    }
    
fail:
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

/* pkt = NULL means EOF (needed to flush decoder buffers) */
static int process_input_packet(InputStream *ist, const AVPacket *pkt, int no_eof)
{
    int ret = 0;
    int got_output = 0;
    
    AVPacket avpkt;
    if (!pkt) {
        /* EOF handling */
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
//        goto handle_eof;
        switch (ist->dec_ctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                ret = decode_audio    (ist, &avpkt, &got_output);
                break;
            case AVMEDIA_TYPE_VIDEO:
                ret = decode_video    (ist, &avpkt, &got_output);
                break;
            default:
                return -1;
        }
    } else {
        avpkt = *pkt;
    }
    
    // while we have more to decode or while the decoder did output something on EOF
    while ((avpkt.size > 0 || (!pkt && got_output))) {
        ist->pts = ist->next_pts;
        ist->dts = ist->next_dts;
        
        switch (ist->dec_ctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                ret = decode_audio    (ist, &avpkt, &got_output);
                break;
            case AVMEDIA_TYPE_VIDEO:
                ret = decode_video    (ist, &avpkt, &got_output);
                break;
            default:
                return -1;
        }
        
        if (ret < 0) {
            break;
        }
        avpkt.dts=
        avpkt.pts= AV_NOPTS_VALUE;
        
        // touch data and size only if not EOF
        if (pkt) {
            if(ist->dec_ctx->codec_type != AVMEDIA_TYPE_AUDIO)
                ret = avpkt.size;
            avpkt.data += ret;
            avpkt.size -= ret;
        }
        if (!got_output) {
            continue;
        }
        if (got_output && !pkt)
            break;
    }
    
    /* after flushing, send an EOF on all the filter inputs attached to the stream */
    /* except when looping we need to flush but not to send an EOF */
    if (!pkt && !got_output && !no_eof) {
        av_buffersrc_add_frame(ist->filters[0]->filter, NULL);
    }
    return got_output;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    return avcodec_default_get_buffer2(s, frame, flags);
}

static int init_input_stream(int ist_index, char *error, int error_len)
{
    int ret;
    InputStream *ist = input_streams[ist_index];
    
    if (ist->decoding_needed) {
        AVCodec *codec = ist->dec;
        if (!codec) {
            return AVERROR(EINVAL);
        }
        
        ist->dec_ctx->opaque                = ist;
        ist->dec_ctx->get_buffer2           = get_buffer;
        ist->dec_ctx->thread_safe_callbacks = 1;
        
        av_opt_set_int(ist->dec_ctx, "refcounted_frames", 1, 0);
        if (ist->dec_ctx->codec_id == AV_CODEC_ID_DVB_SUBTITLE &&
            (ist->decoding_needed & DECODING_FOR_OST)) {
            av_dict_set(&ist->decoder_opts, "compute_edt", "1", AV_DICT_DONT_OVERWRITE);
        }
        
        if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0))
            av_dict_set(&ist->decoder_opts, "threads", "auto", 0);
        if ((ret = avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts)) < 0) {
            return ret;
        }
    }
    
    ist->next_pts = AV_NOPTS_VALUE;
    ist->next_dts = AV_NOPTS_VALUE;
    
    return 0;
}

static InputStream *get_input_stream(OutputStream *ost)
{
    if (ost->source_index >= 0)
        return input_streams[ost->source_index];
    return NULL;
}

static int init_output_stream(OutputStream *ost, char *error, int error_len)
{
    int ret = 0;
    
    if (ost->encoding_needed) {
        AVCodec      *codec = ost->enc;
        AVCodecContext *dec = NULL;
        InputStream *ist;
        
        if ((ist = get_input_stream(ost)))
            dec = ist->dec_ctx;
        if (dec && dec->subtitle_header) {
            /* ASS code assumes this buffer is null terminated so add extra byte. */
            ost->enc_ctx->subtitle_header = av_mallocz(dec->subtitle_header_size + 1);
            if (!ost->enc_ctx->subtitle_header)
                return AVERROR(ENOMEM);
            memcpy(ost->enc_ctx->subtitle_header, dec->subtitle_header, dec->subtitle_header_size);
            ost->enc_ctx->subtitle_header_size = dec->subtitle_header_size;
        }
        if (!av_dict_get(ost->encoder_opts, "threads", NULL, 0))
            av_dict_set(&ost->encoder_opts, "threads", "auto", 0);
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !codec->defaults &&
            !av_dict_get(ost->encoder_opts, "b", NULL, 0) &&
            !av_dict_get(ost->encoder_opts, "ab", NULL, 0))
            av_dict_set(&ost->encoder_opts, "b", "128000", 0);
        
        if (ost->filter && ost->filter->filter->inputs[0]->hw_frames_ctx) {
            ost->enc_ctx->hw_frames_ctx = av_buffer_ref(ost->filter->filter->inputs[0]->hw_frames_ctx);
            if (!ost->enc_ctx->hw_frames_ctx)
                return AVERROR(ENOMEM);
        }
        
        if ((ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts)) < 0) {
            return ret;
        }
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
            av_buffersink_set_frame_size(ost->filter->filter,
                                         ost->enc_ctx->frame_size);
        
        ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx);
        if (ret < 0) {
        }
        
        if (ost->enc_ctx->nb_coded_side_data) {
            int i;
            
            ost->st->side_data = av_realloc_array(NULL, ost->enc_ctx->nb_coded_side_data,
                                                  sizeof(*ost->st->side_data));
            if (!ost->st->side_data)
                return AVERROR(ENOMEM);
            
            for (i = 0; i < ost->enc_ctx->nb_coded_side_data; i++) {
                const AVPacketSideData *sd_src = &ost->enc_ctx->coded_side_data[i];
                AVPacketSideData *sd_dst = &ost->st->side_data[i];
                
                sd_dst->data = av_malloc(sd_src->size);
                if (!sd_dst->data)
                    return AVERROR(ENOMEM);
                memcpy(sd_dst->data, sd_src->data, sd_src->size);
                sd_dst->size = sd_src->size;
                sd_dst->type = sd_src->type;
                ost->st->nb_side_data++;
            }
        }
        
        // copy timebase while removing common factors
        ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational){0, 1});
        ost->st->codec->codec= ost->enc_ctx->codec;
    } else {
        ret = av_opt_set_dict(ost->enc_ctx, &ost->encoder_opts);
        if (ret < 0) {
            return ret;
        }
        // copy timebase while removing common factors
        ost->st->time_base = av_add_q(ost->st->codec->time_base, (AVRational){0, 1});
    }
    
    return ret;
}

static int transcode_init(void)
{
    int ret = 0, i;
    AVFormatContext *oc;
    OutputStream *ost = NULL;
    InputStream *ist;
    char error[1024] = {0};
    
    /* for each output stream, we compute the right encoding parameters */
    for (i = 0; i < nb_output_streams; i++) {
        AVCodecContext *dec_ctx = NULL;
        ost = output_streams[i];
        oc  = output_files[ost->file_index]->ctx;
        ist = get_input_stream(ost);
        AVCodecContext *enc_ctx = ost->enc_ctx;
        
        if (ist) {
            dec_ctx = ist->dec_ctx;
            
            ost->st->disposition          = ist->st->disposition;
            enc_ctx->bits_per_raw_sample    = dec_ctx->bits_per_raw_sample;
            enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;
        }
        
        if (!ost->filter &&
            (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
             enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO)) {
                FilterGraph *fg;
                fg = init_simple_filtergraph(ist, ost);
                if (configure_filtergraph(fg)) {
                    av_log(NULL, AV_LOG_FATAL, "Error opening filters!\n");
                }
            }
        
        if (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (!ost->frame_rate.num)
                ost->frame_rate = av_buffersink_get_frame_rate(ost->filter->filter);
            if (ist && !ost->frame_rate.num)
                ost->frame_rate = ist->st->r_frame_rate;
            if (ist && !ost->frame_rate.num) {
                ost->frame_rate = (AVRational){25, 1};
            }
            
            // reduce frame rate for mpeg4 to be within the spec limits
            if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4) {
                av_reduce(&ost->frame_rate.num, &ost->frame_rate.den,
                          ost->frame_rate.num, ost->frame_rate.den, 65535);
            }
        }
        
        switch (enc_ctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                enc_ctx->sample_fmt     = ost->filter->filter->inputs[0]->format;
                enc_ctx->sample_rate    = ost->filter->filter->inputs[0]->sample_rate;
                enc_ctx->channel_layout = ost->filter->filter->inputs[0]->channel_layout;
                enc_ctx->channels       = avfilter_link_get_channels(ost->filter->filter->inputs[0]);
                enc_ctx->time_base      = (AVRational){ 1, enc_ctx->sample_rate };
                break;
            case AVMEDIA_TYPE_VIDEO:
                enc_ctx->time_base = av_inv_q(ost->frame_rate);
                enc_ctx->width  = ost->filter->filter->inputs[0]->w;
                enc_ctx->height = ost->filter->filter->inputs[0]->h;
                enc_ctx->pix_fmt = ost->filter->filter->inputs[0]->format;
                
                ost->st->avg_frame_rate = ost->frame_rate;
                break;
                default:
                break;
        }
    }
    
    /* init input streams */
    for (i = 0; i < nb_input_streams; i++)
        if ((ret = init_input_stream(i, error, sizeof(error))) < 0) {
            for (i = 0; i < nb_output_streams; i++) {
                ost = output_streams[i];
                avcodec_close(ost->enc_ctx);
            }
        }
    
    /* open each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        ret = init_output_stream(output_streams[i], error, sizeof(error));
    }

    /* open files and write file headers */
    for (i = 0; i < nb_output_files; i++) {
        oc = output_files[i]->ctx;
        oc->interrupt_callback = int_cb;
        if ((ret = avformat_write_header(oc, &output_files[i]->opts)) < 0) {
            ret = AVERROR(EINVAL);
        }
    }
    transcode_init_done = 1;
    
    return 0;
}

/* Return 1 if there remain streams where more output is wanted, 0 otherwise. */
static int need_output(void)
{
    for (int i = 0; i < nb_output_streams; i++) {
        OutputStream *ost    = output_streams[i];
        if (ost->finished)
            continue;
        return 1;
    }
    
    return 0;
}

/**
 * Perform a step of transcoding for the specified filter graph.
 *
 * @param[in]  graph     filter graph to consider
 * @param[out] best_ist  input stream where a frame would allow to continue
 * @return  0 for success, <0 for error
 */
static int transcode_from_filter(FilterGraph *graph, InputStream **best_ist)
{
    int i, ret;
    int nb_requests, nb_requests_max = 0;
    InputFilter *ifilter;
    InputStream *ist;
    
    *best_ist = NULL;
    ret = avfilter_graph_request_oldest(graph->graph);
    if (ret >= 0)
        return reap_filters(0);
    
    if (ret == AVERROR_EOF) {
//        ret = reap_filters(1);
        for (i = 0; i < graph->nb_outputs; i++) {
            close_output_stream(graph->outputs[i]->ost);
        }
        
        return ret;
    }
    if (ret != AVERROR(EAGAIN))
        return ret;
    
    for (i = 0; i < graph->nb_inputs; i++) {
        ifilter = graph->inputs[i];
        ist = ifilter->ist;
        nb_requests = av_buffersrc_get_nb_failed_requests(ifilter->filter);
        if (nb_requests > nb_requests_max) {
            nb_requests_max = nb_requests;
            *best_ist = ist;
        }
    }
    return 0;
}

/*
 * The following code is the main loop of the file converter
 */
int transcode(void)
{
    int ret, i;
    AVFormatContext *os;
    OutputStream *ost;
    InputStream *ist;
    
    ret = transcode_init();
    if (ret < 0)
        goto fail;
    while (need_output()) {
        OutputStream *ost = NULL;
        InputStream *ist = NULL;
        for (int i = 0; i < nb_output_streams; i++) {
            if (!output_streams[i]->finished) {
                ost = output_streams[i];
                break;
            }
        }
        if (ost->filter) {
            ret = avfilter_graph_request_oldest(ost->filter->graph->graph);
            if (ret >= 0) {
                if (reap_filters(0) < 0) {
                    continue;
                }
                
            } else {
                if (ret == AVERROR_EOF) {
                    for (int i = 0; i < ost->filter->graph->nb_outputs; i++) {
                        ost->finished = ENCODER_FINISHED;
                    }
                    continue;
                }
                if (ret != AVERROR(EAGAIN) && ret < 0) {
                    continue;
                }
                int nb_requests_max = 0;
                for (int i = 0; i < ost->filter->graph->nb_inputs; i++) {
                    InputFilter *ifilter = ost->filter->graph->inputs[i];
                    int nb_requests = av_buffersrc_get_nb_failed_requests(ifilter->filter);
                    if (nb_requests > nb_requests_max) {
                        nb_requests_max = nb_requests;
                        ist = ifilter->ist;
                    }
                }
            }
        }
        InputFile *ifile = input_files[0];
        AVPacket pkt;
        ret = av_read_frame(ifile->ctx, &pkt);
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret < 0) {
            for (int i = 0; i < ifile->nb_streams; i++) {
                ist = input_streams[i];
                ret = process_input_packet(ist, NULL, 0);
                if (ret > 0) {
                    reap_filters(0);
                    break;
                }
            }
            continue;
        }
        av_pkt_dump_log2(NULL, AV_LOG_INFO, &pkt, 0, ifile->ctx->streams[pkt.stream_index]);
        ist = input_streams[pkt.stream_index];
        process_input_packet(ist, &pkt, 0);
        av_packet_unref(&pkt);
        ret = reap_filters(0);
        if (ret < 0 && ret != AVERROR_EOF) {
            break;
        }
    }
    
    /* at the end of stream, we must flush the decoder buffers */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (ist->decoding_needed) {
            process_input_packet(ist, NULL, 0);
        }
    }
    flush_encoders();
    /* write the trailer if needed and close file */
    for (i = 0; i < nb_output_files; i++) {
        os = output_files[i]->ctx;
        if ((ret = av_write_trailer(os)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error writing trailer of %s: %s", os->filename, av_err2str(ret));
        }
    }
    
    /* close each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];
        if (ost->encoding_needed) {
            av_freep(&ost->enc_ctx->stats_in);
        }
    }
    
    /* close each decoder */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->dec_ctx);
        }
    }
    
    /* finished ! */
    ret = 0;
    
fail:
    
    if (output_streams) {
        for (i = 0; i < nb_output_streams; i++) {
            ost = output_streams[i];
            if (ost) {
                av_dict_free(&ost->encoder_opts);
            }
        }
    }
    return ret;
}

