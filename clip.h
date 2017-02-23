#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <list>
#include <string>
#include <vector>

#include "common.h"

enum class TransitionEffect {
	None,
	Fade,
};

enum class FilterEffect {
	None,
	FadeOut,
	FadeIn,
	Overlay,
	Scale,
	RGB,
	SoloTrack,
	OverlayTrack,
	AudioMix,
	AudioPrep,
};

// API requires filtergraph outputs only one frame per input frame
// also requires output frame to be same size as input frame
class Filter
{
public:
	std::string filter_str;
	FilterEffect effect;

	virtual ~Filter();

	int feed(AVFrame* in);
	int feed(AVFrame* in, AVFrame* in2);
	AVFrame* get_output_frame();
	bool is_finished();

	static Filter* FadeOut(const Decoder_Ctx* decoder, float duration);
	static Filter* FadeIn(const Decoder_Ctx* decoder, float duration);
	static Filter* Overlay(const Decoder_Ctx* decoder1, const Decoder_Ctx* decoder2);
	static Filter* Scale(const Decoder_Ctx* decoder1, int out_width, int out_height);
	static Filter* RGB(const Decoder_Ctx* decoder1);
	static Filter* SoloTrack(const Decoder_Ctx* decoder, int out_width, int out_height);
	static Filter* OverlayTrack(const Decoder_Ctx* decoder1, const Decoder_Ctx* decoder2, int out_width, int out_height);
	static Filter* AudioMix(const Decoder_Ctx* decoder1, const Decoder_Ctx* decoder2);
	static Filter* AudioPrep(const Decoder_Ctx* decoder1);

protected:
	Filter(FilterEffect effect, const std::string& filter_str);

	AVFilterGraph* graph;

	int frame_duration;
	int frames_fed = 0;
	int64_t last_pts1_fed = -1;
	int64_t last_pts2_fed = -1;
	AVFrame* output_frame;

	AVFilterGraph *filter_graph = nullptr;
	AVFilterContext *buffersrc_ctx = nullptr;
	AVFilterContext *buffersrc2_ctx = nullptr;
	AVFilterContext *buffersink_ctx = nullptr;

	int init(const std::string& filter_str);
};

class FilePiece {
public:
	std::string filename;
	float video_start_secs;
	float file_start_secs;
	float duration_secs;

	FilePiece(std::string filename, float video_start_secs);
	FilePiece(std::string filename, float video_start_secs, float file_start_secs, float duration_secs);
	FilePiece(const FilePiece& fp);

	FilePiece with_duration(float duration_secs);
};

class Clip
{
public:
	std::string filename;
	float video_start_secs;
	float file_start_secs;
	float duration_secs;
	enum FilterEffect effect;

	// seems weird that no decoder is needed - but all this does ATM is filter frames
	Clip(const FilePiece& file_piece);
	Clip(const FilePiece& file_piece, enum FilterEffect effect);
	~Clip();
};

struct TrackPiece
{
	// transition occurs before file plays
	FilePiece file;
	TransitionEffect transition;

	TrackPiece(FilePiece file_piece, TransitionEffect effect);
};

class Video;

class Track {
public:
	std::list<TrackPiece> pieces;

	Track(Video* video);
	Track(Video* video, const std::string& filename);
	Track(Video* video, FilePiece file_piece);
	Track(Video* video, FilePiece file_piece, TransitionEffect effect);

	void add(FilePiece file_piece, TransitionEffect effect);
	void split(float secs, TransitionEffect effect);
	bool seek(float secs);

	AVFrame* get_video_frame(float secs);
	//AVFrame* get_audio_frame(float secs);
	AVFrame* get_next_audio_frame();
	const Decoder_Ctx* get_decoder() const;
	const AVCodecContext* get_audio_context() const;

	const std::list<Clip>& get_clips();
	float get_duration_secs() const;
	float last_shown_frame_secs = 0;

protected:
	static bool ensure_decoder_at(Decoder_Ctx* decoder, const std::string& filename, float seek_secs);

	Video* video;

	Clip* get_next_frame_clip();
	Clip* get_next_clip();
	float get_last_video_frame_secs();

	std::list<Clip> clips;
	void recalc_clips();
	Clip* find_clip_at(float secs);
	Clip* find_next_clip_after(float secs);

	Filter* current_filter = nullptr;
	std::unique_ptr<Decoder_Ctx> decoder;
};

class Video {
public:
	Track main_track;
	Track overlay_track;

	Video();
	~Video();

	void addToMainTrack(const std::string& filename, TransitionEffect effect);
	void addToOverlayTrack(const std::string& filename, TransitionEffect effect);
	bool seek(float secs);

	AVFrame* out_video_frame = nullptr;
	float get_duration_secs();
	int get_video_frame(float secs, int width, int height);
	float get_last_video_frame_secs();

	AVFrame* out_audio_frame = nullptr;
	int get_next_audio_frame();

private:
	Filter* solo_track_filter;
	Filter* overlay_track_filter;
	Filter* audiomix_filter;
	Filter* audioprep_filter;
};
