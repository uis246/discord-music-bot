#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <voice/decoding.h>

static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    assert(opaque);
    buffer_data *bd = (buffer_data *) opaque;
    buf_size = FFMIN((size_t) buf_size, bd->size - bd->loc);
    memcpy(buf, &bd->data[bd->loc], buf_size);
    bd->loc += buf_size;
    return buf_size;
}

static int64_t seek(void *opaque, int64_t pos, int whence)
{
    assert(opaque);
    buffer_data *bd = (buffer_data *) opaque;
    switch (whence) {
        case SEEK_SET:
            return bd->loc = pos;
        case SEEK_CUR:
            return bd->loc += pos;
        case SEEK_END:
            return bd->loc = bd->size + pos;
        case AVSEEK_SIZE:
            return bd->size;
    }
    return -1;
}

avio_info::avio_info(std::vector<uint8_t> &audio_data)
{
    avio_buf_len = 32768;
    avio_buf = reinterpret_cast<uint8_t *>(av_malloc(avio_buf_len));
    if (!avio_buf) {
        fprintf(stderr, "Could not allocate avio context buffer\n");
        exit(1);
    }
    audio_file_data = {&audio_data[0], audio_data.size(), 0};

    // Instead of using avformat_open_input and passing path, we're going to use AVIO
    // which allows us to point to an already allocated area of memory that contains the media
    avio_context = avio_alloc_context(avio_buf, avio_buf_len, 0, &audio_file_data, &read_packet,
                                      nullptr, &seek);
    if (!avio_context) {
        fprintf(stderr, "Could not allocate AVIO context\n");
        exit(1);
    }
}

avio_info::~avio_info()
{
    av_free(avio_context->buffer);
    av_free(avio_context);
}

audio_decoder::audio_decoder(avio_info &av) : do_read{true}, do_feed{true}
{
    format_context = avformat_alloc_context();
    if (!format_context) {
        fprintf(stderr, "Could not allocate format context.\n");
        exit(1);
    }
    // Use the AVIO context
    format_context->pb = av.avio_context;

    // Open the file, read the header, export information into format_context
    if (avformat_open_input(&format_context, nullptr, nullptr, nullptr)) {
        fprintf(stderr, "Error opening input.");
        exit(1);
    }
    // Some format do not have header, or do not store enough information there so try to read
    // and decode a few frames if necessary to find missing information
    if (avformat_find_stream_info(format_context, nullptr) < 0) {
        fprintf(stderr, "Could not retrieve stream info.\n");
        exit(1);
    }

    AVCodec *decoder;

    // Get best audio stream from format_context, and possibly decoder for this stream
    stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream_index == -1) {
        fprintf(stderr, "Could not find an audio stream in file.\n");
        exit(1);
    }

    AVStream *stream = format_context->streams[stream_index];

    // We weren't able to get decoder when opening stream, find decoder by stream's codec_id
    if (!decoder) {
        fprintf(stderr, "Failed to retrieve decoder when opening stream. Finding decoder... ");
        decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            fprintf(stderr, "Unable to find audio decoder.\n");
            exit(1);
        }
        fprintf(stderr, "Found!\n");
    }

    // Allocate codec context for the decoder
    decoder_context = avcodec_alloc_context3(decoder);
    if (!decoder_context) {
        fprintf(stderr, "Failed to allocate the audio codec context.\n");
        exit(1);
    }
    if (avcodec_parameters_to_context(decoder_context, stream->codecpar) < 0) {
        fprintf(stderr, "Failed to copy audio codec parameters to decoder context.\n");
        exit(1);
    }

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "refcounted_frames", "1", 0);
    if (avcodec_open2(decoder_context, decoder, &opts) < 0) {
        fprintf(stderr, "Failed to open decoder for stream #%u.\n", stream_index);
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Unable to allocate audio frame.\n");
    }
}

audio_decoder::~audio_decoder()
{
    av_frame_free(&frame);
    avcodec_close(decoder_context);
    avcodec_free_context(&decoder_context);
    avformat_close_input(&format_context);
    avformat_free_context(format_context);
}

int audio_decoder::read()
{
    int ret = 0;
    av_init_packet(&packet);
    // Grab the next packet for the audio stream we're interested in
    while ((ret = av_read_frame(format_context, &packet)) >= 0) {
        if (packet.stream_index != stream_index) {
            av_packet_unref(&packet);
        } else {
            break;
        }
    }
    return ret;
}

// Feed the decoder the next packet from the demuxer (format_context)
int audio_decoder::feed(bool flush)
{
    int ret;
    if (flush) {
        ret = avcodec_send_packet(decoder_context, nullptr);
        do_read = false;
    } else {
        // Send the packet to the decoder
        ret = avcodec_send_packet(decoder_context, &packet);
    }

    if (ret == AVERROR(EAGAIN)) {
        fprintf(stderr, "Decoder denied input. Output must be read.\n");
        do_read = false;
        do_feed = false;
    } else if (ret == AVERROR_EOF) {
        fprintf(stderr, "Decoder has been flushed. No new packets can be sent.\n");
        do_feed = false;
    } else if (ret == AVERROR(EINVAL)) {
        fprintf(stderr, "Decoder is not opened.\n");
        exit(1);
    } else if (ret == AVERROR(ENOMEM)) {
        fprintf(stderr, "Decoder failed to add packet to internal queue: no memory.\n");
        exit(1);
    } else {
        av_packet_unref(&packet);
        do_feed = true;
    }
    return ret;
}

int audio_decoder::decode()
{
    assert(frame);

    // Retrieve (decoded) frame from decoder
    int ret = avcodec_receive_frame(decoder_context, frame);

    if (ret == AVERROR(EAGAIN)) {
        fprintf(stderr, "Error receiving frame from decoder.\n");
    } else if (ret == AVERROR_EOF) {
        fprintf(stderr, "Decoder is fully flushed. No more output frames.\n");
    } else if (ret == AVERROR(EINVAL)) {
        fprintf(stderr, "Decoder is not opened.\n");
        exit(1);
    }
    return ret;
}

AVFrame *audio_decoder::next_frame()
{
    int ret = 0;
    if (do_read)
        ret = read();
    if (ret || do_feed)
        feed(ret != 0);

    ret = decode();
    if (ret == AVERROR(EAGAIN))
        return next_frame();
    if (ret == AVERROR_EOF && !do_feed && !do_read) {
        if (frame)
            av_frame_free(&frame);
        frame = nullptr;
    }
    return frame;
}

audio_resampler::audio_resampler(audio_decoder &decoder, int sample_rate, int channels,
                                 AVSampleFormat format)
    : frame_buf{nullptr}, current_alloc{0}, format{format}
{
    swr = swr_alloc();
    if (!swr) {
        fprintf(stderr, "Could not allocate resampling context\n");
        return;
    }
    av_opt_set_int(swr, "in_channel_count", decoder.decoder_context->channels, 0);
    av_opt_set_int(swr, "out_channel_count", channels, 0);
    av_opt_set_int(swr, "in_channel_layout", decoder.decoder_context->channel_layout, 0);
    av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(swr, "in_sample_rate", decoder.decoder_context->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", decoder.decoder_context->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", format, 0);
    swr_init(swr);
    if (!swr_is_initialized(swr)) {
        fprintf(stderr, "Could not initialize resampler\n");
        swr_free(&swr);
    }
}

audio_resampler::~audio_resampler()
{
    if (swr)
        swr_free(&swr);
    if (frame_buf)
        av_free(frame_buf);
}

void *audio_resampler::resample(AVFrame *frame, int &frame_count)
{
    assert(swr);
    int swr_output_count = swr_get_out_samples(swr, frame->nb_samples);
    if (swr_output_count < 0) {
        fprintf(stderr, "Error swr_get_out_samples\n");
        frame_count = 0;
        return nullptr;
    }

    if (swr_output_count > current_alloc) {
        av_free(frame_buf);
        av_samples_alloc(&frame_buf, nullptr, 2, swr_output_count, format, 0);
        current_alloc = swr_output_count;
    }

    if (frame_buf) {
        frame_count = swr_convert(swr, &frame_buf, swr_output_count,
                                  const_cast<const uint8_t **>(frame->data), frame->nb_samples);
    }
    return frame_buf;
}