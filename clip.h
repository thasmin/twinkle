#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <string>
#include <sstream>

#include "common.h"

enum FilterEffect {
	None,
	FadeOut,
	FadeIn = 2
};

class Filter
{
public:
	Decoder_Ctx* decoder;
	std::string descr;

	Filter(Decoder_Ctx* decoder, std::string descr);
	virtual ~Filter();

	int feed(AVFrame* in);
	AVFrame* get_output_frame();

	static Filter* FadeOut(Decoder_Ctx* decoder) { return new Filter(decoder, "fade=t=out"); }
	static Filter* FadeOut(Decoder_Ctx* decoder, float duration) {
		std::stringstream desc;
		desc << "fade=t=out;d=" << duration;
		return new Filter(decoder, desc.str());
	}

	static Filter* FadeIn(Decoder_Ctx* decoder) { return new Filter(decoder, "fade=t=in"); }
	static Filter* FadeIn(Decoder_Ctx* decoder, float duration) {
		std::stringstream desc;
		desc << "fade=t=in;d=" << duration;
		return new Filter(decoder, desc.str());
	}

protected:
	AVFrame* output_frame;

	AVFilterContext *buffersink_ctx = nullptr;
	AVFilterContext *buffersrc_ctx = nullptr;
	AVFilterGraph *filter_graph = nullptr;

	int init_filters();
};

// API requires filtergraph outputs only one frame per input frame
class Clip
{
public:
	double start_secs;
	double duration_secs;
	Filter* filter;

	int feed(AVFrame* in);
	AVFrame* get_output_frame();

	Clip(double start_secs, double duration);
	Clip(double start_secs, double duration, Decoder_Ctx* decoder, enum FilterEffect effect);
	~Clip();

private:
	AVFrame* output_frame;
};

