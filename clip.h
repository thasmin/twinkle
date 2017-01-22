#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <string>

#include "common.h"

// API requires filtergraph outputs only one frame per input frame
class Clip {
public:
	Decoder_Ctx* decoder;
	double start_secs;
	double duration_secs;
	std::string filters_descr;

	Clip(Decoder_Ctx* decoder, std::string filters_descr, double start_secs, double duration);
	~Clip();

	int feed(AVFrame* in);
	AVFrame* get_output_frame();

private:
	AVFrame* output_frame;

	AVFilterContext *buffersink_ctx = nullptr;
	AVFilterContext *buffersrc_ctx = nullptr;
	AVFilterGraph *filter_graph = nullptr;

	int init_filters();
};
