#include "clip.h"

Clip::Clip(Decoder_Ctx* decoder, std::string filters_descr, double start_secs, double duration_secs)
{
	this->decoder = decoder;
	this->start_secs = start_secs;
	this->duration_secs = duration_secs;
	this->filters_descr = filters_descr;

	this->output_frame = av_frame_alloc();
}

Clip::Clip(Decoder_Ctx* decoder, double start_secs, double duration_secs)
{
	this->decoder = decoder;
	this->start_secs = start_secs;
	this->duration_secs = duration_secs;

	this->output_frame = av_frame_alloc();
}


Clip::~Clip()
{
	av_frame_unref(this->output_frame);
	if (filter_graph != nullptr)
		avfilter_graph_free(&this->filter_graph);
}

void Clip::set_filter_descr(std::string descr)
{
	this->filters_descr = filters_descr;

	if (filter_graph != nullptr)
		avfilter_graph_free(&this->filter_graph);
	filter_graph = nullptr;
}

AVFrame* Clip::get_output_frame()
{
	return this->output_frame;
}

int Clip::feed(AVFrame* in_frame)
{
	int ret;

	// if no filter then just copy frame
	// otherwise make sure filters are initialized
	if (filters_descr.empty()) {
		av_frame_ref(this->output_frame, in_frame);
		return 0;
	} else if (this->filter_graph == nullptr) {
		ret = init_filters();
		if (ret < 0)
			return ret;
	}

	ret = av_buffersrc_add_frame_flags(this->buffersrc_ctx, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0;
	if (ret < 0) {
		printf("Error while feeding the filtergraph: %s\n", av_err2str(ret));
		return ret;
	}

	while (true) {
		ret = av_buffersink_get_frame(this->buffersink_ctx, this->output_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			printf("Error while retrieving from the filter graph: %s\n", av_err2str(ret));
			return ret;
		}
	}

	return 0;
}

int Clip::init_filters()
{
	int ret = 0;

	AVCodecContext* dec_ctx = decoder->get_video_context();
	AVStream* video_stream = decoder->get_video_stream();

    AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = video_stream->time_base;

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    // buffer video source: the decoded frames from the decoder will be inserted here.
	char args[512];
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
    if (ret < 0) {
        printf("Cannot create buffer source: %s\n", av_err2str(ret));
        goto end;
    }

    // buffer video sink: to terminate the filter chain.
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
    if (ret < 0) {
        printf("Cannot create buffer sink: %s\n", av_err2str(ret));
        goto end;
    }

    // Set the endpoints for the filter graph. The filter_graph will be linked to the graph described by filters_descr.

    // The buffer source output must be connected to the input pad of the first filter described by filters_descr
	// since the first filter input label is not specified, it is set to "in" by default.
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    // The buffer sink input must be connected to the output pad of the last filter described by filters_descr
	// since the last filter output label is not specified, it is set to "out" by default.
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr.c_str(), &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

