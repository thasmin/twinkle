#include "clip.h"

#include <iomanip>
#include <numeric>
#include <sstream>

extern "C" {
#include "libavutil/opt.h"
}

#include "logger.h"

std::ostream& operator<<(std::ostream& out, const TransitionEffect value){
	const char* s = 0;
#define SWITCH_VAL(p) case(p): s = #p; break;
	switch(value){
		SWITCH_VAL(TransitionEffect::None);
		SWITCH_VAL(TransitionEffect::Fade);
	}
#undef SWITCH_VAL
	return out << s;
}

std::ostream& operator<<(std::ostream& out, const FilterEffect value){
	const char* s = 0;
#define SWITCH_VAL(p) case(p): s = #p; break;
	switch(value){
		SWITCH_VAL(FilterEffect::None);
		SWITCH_VAL(FilterEffect::FadeOut);
		SWITCH_VAL(FilterEffect::FadeIn);
		SWITCH_VAL(FilterEffect::Overlay);
		SWITCH_VAL(FilterEffect::Scale);
		SWITCH_VAL(FilterEffect::RGB);
		SWITCH_VAL(FilterEffect::SoloTrack);
		SWITCH_VAL(FilterEffect::OverlayTrack);
		SWITCH_VAL(FilterEffect::AudioMix);
		SWITCH_VAL(FilterEffect::AudioPrep);
	}
#undef SWITCH_VAL
	return out << s;
}

std::string get_buffer_str(const Decoder_Ctx* decoder, const std::string& output_name)
{
	const AVCodecContext* video_ctx = decoder->get_video_context();
	const AVStream* video_stream = decoder->get_video_stream();

	std::stringstream ss;
	ss << "buffer=";
	ss << "video_size=" << video_ctx->width << "x" << video_ctx->height << ":";
	ss << "pix_fmt=" << video_ctx->pix_fmt << ":";
	ss << "time_base=" << video_stream->time_base.num << "/" << video_stream->time_base.den << ":";
	ss << "pixel_aspect=" << video_stream->sample_aspect_ratio.num << "/" << video_stream->sample_aspect_ratio.den << " ";
	ss << "[" << output_name << "];";
	return ss.str();
}

std::string get_buffersink_str(const std::string& input_name)
{
	std::stringstream ss;
	ss << "[" << input_name << "] buffersink";
	return ss.str();
}

std::string get_abuffer_str(const Decoder_Ctx* decoder, const std::string& output_name)
{
	const AVCodecContext* audio_ctx = decoder->get_audio_context();
	const AVStream* audio_stream = decoder->get_audio_stream();

	std::stringstream ss;
	ss << "abuffer=";
	ss << "time_base=" << audio_stream->time_base.num << "/" << audio_stream->time_base.den << ":";
	ss << "sample_rate=" << audio_ctx->sample_rate << ":";
	ss << "sample_fmt=" << audio_ctx->sample_fmt << ":";
	ss << "channel_layout=" << audio_ctx->channel_layout << " ";
	ss << "[" << output_name << "];";
	return ss.str();
}

std::string get_abuffersink_str(const std::string& input_name)
{
	std::stringstream ss;
	ss << "[" << input_name << "] abuffersink";
	return ss.str();
}


Filter* Filter::FadeOut(const Decoder_Ctx* decoder, float duration)
{
	std::string buffer_str = get_buffer_str(decoder, "in_1");
	std::string sink_str = get_buffersink_str("result");
	std::stringstream fade_str;
	fade_str << "[in_1] fade=t=out:s=0:n=" << decoder->get_num_frames_in(duration) << " [result];";
	std::string filter_str = buffer_str + fade_str.str() + sink_str;
	return new Filter(FilterEffect::FadeOut, filter_str);
}

Filter* Filter::FadeIn(const Decoder_Ctx* decoder, float duration)
{
	std::string buffer_str = get_buffer_str(decoder, "in_1");
	std::string sink_str = get_buffersink_str("result");
	std::stringstream fade_str;
	fade_str << "[in_1] fade=t=in:s=0:n=" << decoder->get_num_frames_in(duration) << " [result];";
	std::string filter_str = buffer_str + fade_str.str() + sink_str;
	return new Filter(FilterEffect::FadeIn, filter_str);
}

Filter* Filter::Scale(const Decoder_Ctx* decoder, int out_width, int out_height)
{
	std::string buffer_str = get_buffer_str(decoder, "in_1");
	std::string sink_str = get_buffersink_str("result");
	std::stringstream scale_str;
	scale_str << buffer_str;
	scale_str << "[in_1] scale=w=" << out_width << ":h=" << out_height << " [scaled];";
	scale_str << sink_str;
	return new Filter(FilterEffect::Scale, scale_str.str());
}

Filter* Filter::RGB(const Decoder_Ctx* decoder) {
	std::string buffer_str = get_buffer_str(decoder, "in_1");
	std::string sink_str = get_buffersink_str("result");
	std::stringstream rgb_str;
	rgb_str << "[in_1] format=pix_fmts=rgb24 [result];";
	return new Filter(FilterEffect::RGB, rgb_str.str());
}

Filter* Filter::Overlay(const Decoder_Ctx* decoder1, const Decoder_Ctx* decoder2)
{
	std::string buffer1_str = get_buffer_str(decoder1, "in_1");
	std::string buffer2_str = get_buffer_str(decoder2, "in_2");
	std::string sink_str = get_buffersink_str("result");

	std::string box_str = "[in_1] drawbox=x=9:y=9:w=52:h=52:c=red:t=3 [boxed];";
	std::string scale_str = "[in_2] scale=w=50:h=50 [overlay];";
	std::string overlay_str = "[boxed] [overlay] overlay=x=10:y=10 [result];";

	std::stringstream filter_str;
	filter_str << buffer1_str << buffer2_str << box_str << scale_str << overlay_str << sink_str;

	Logger::get("overlay") << filter_str.str() << "\n";
	return new Filter(FilterEffect::Overlay, filter_str.str());
}

Filter* Filter::SoloTrack(const Decoder_Ctx* decoder, int out_width, int out_height)
{
	std::string buffer_str = get_buffer_str(decoder, "in_1");
	std::string sink_str = get_buffersink_str("result");
	std::stringstream filter_str;
	filter_str << buffer_str;
	filter_str << "[in_1] scale=w=" << out_width << ":h=" << out_height << " [scaled];";
	filter_str << "[scaled] format=pix_fmts=rgb24 [result];";
	filter_str << sink_str;
	return new Filter(FilterEffect::SoloTrack, filter_str.str());
}

Filter* Filter::OverlayTrack(const Decoder_Ctx* decoder1, const Decoder_Ctx* decoder2, int out_width, int out_height)
{
	std::string buffer1_str = get_buffer_str(decoder1, "in_1");
	std::string buffer2_str = get_buffer_str(decoder2, "in_2");
	std::string sink_str = get_buffersink_str("result");

	std::stringstream filter_str;
	filter_str << buffer1_str;
	filter_str << buffer2_str;
	filter_str << "[in_1] scale=w=" << out_width << ":h=" << out_height << " [scaled];";
	filter_str << "[scaled] drawbox=x=9:y=9:w=52:h=52:c=red:t=3 [boxed];";
	filter_str << "[in_2] scale=w=50:h=50 [overlay];";
	filter_str << "[boxed] [overlay] overlay=x=10:y=10 [overlayed];";
	filter_str << "[overlayed] format=pix_fmts=rgb24 [result];";
	filter_str << sink_str;

	Logger::get("overlay") << "overlay video filter: " << filter_str.str() << "\n";
	return new Filter(FilterEffect::OverlayTrack, filter_str.str());
}

Filter* Filter::AudioMix(const Decoder_Ctx* decoder1, const Decoder_Ctx* decoder2)
{
	std::string buffer1_str = get_abuffer_str(decoder1, "in_1");
	std::string buffer2_str = get_abuffer_str(decoder2, "in_2");
	std::string sink_str = get_abuffersink_str("result");

	std::stringstream filter_str;
	filter_str << buffer1_str;
	filter_str << buffer2_str;
	filter_str << "[in_1] volume=0.8 [v1];";
	filter_str << "[in_2] volume=2.5 [v2];";
	filter_str << "[v1] [v2] amix [mixed];";
	filter_str << "[mixed] aresample=osr=44100:ocl=stereo:osf=s16 [result];";
	filter_str << sink_str;

	Logger::get("overlay") << "audiomix filter: " << filter_str.str() << "\n";
	return new Filter(FilterEffect::AudioMix, filter_str.str());
}

Filter* Filter::AudioPrep(const Decoder_Ctx* decoder)
{
	std::string buffer1_str = get_abuffer_str(decoder, "in_1");
	std::string sink_str = get_abuffersink_str("result");

	std::stringstream filter_str;
	filter_str << buffer1_str;
	filter_str << "[in_1] aresample=osr=44100:ocl=stereo:osf=s16 [result];";
	filter_str << sink_str;

	Logger::get("overlay") << "audioprep filter: " << filter_str.str() << "\n";
	return new Filter(FilterEffect::AudioPrep, filter_str.str());
}

Filter::Filter(FilterEffect effect, const std::string& filter_str)
{
	this->effect = effect;
	this->filter_str = filter_str;

	this->output_frame = av_frame_alloc();
	this->init(filter_str);
}

Filter::~Filter()
{
	av_frame_unref(this->output_frame);
	if (filter_graph != nullptr)
		avfilter_graph_free(&this->filter_graph);
}

int Filter::init(const std::string& filter_str)
{
	int ret;
	AVFilterInOut *unused_ins = NULL;
	AVFilterInOut *unused_outs = NULL;

	this->graph = avfilter_graph_alloc();
	if (this->graph == NULL) {
		Logger::get("error") << "Cannot allocate filter graph.";
		return AVERROR(ENOMEM);
	}

	ret = avfilter_graph_parse2(graph, filter_str.c_str(), &unused_ins, &unused_outs);
	if (ret < 0) {
		Logger::get("error") << "Cannot parse graph:" << av_err2str(ret) << "\n";
		return ret;
	}

	ret = avfilter_graph_config(graph, NULL);
	if (ret < 0) {
		Logger::get("error") << "Cannot configure graph:" << av_err2str(ret) << "\n";
		return ret;
	}

	if (unused_ins != nullptr || unused_outs != nullptr) {
		Logger::get("error") << "Incomplete filter chain\n";
		return -1;
	}

	// find buffers and buffersinks
	for (int i = 0; i < this->graph->nb_filters; ++i) {
		AVFilterContext* f_ctx = graph->filters[i];
		std::string filter_name = f_ctx->filter->name;
		if (filter_name == "buffer" || filter_name == "abuffer") {
			if (this->buffersrc_ctx == nullptr)
				this->buffersrc_ctx = f_ctx;
			else
				this->buffersrc2_ctx = f_ctx;
		} else if (filter_name == "buffersink" || filter_name == "abuffersink")
			this->buffersink_ctx = f_ctx;
	}

	return 0;
}

bool Filter::is_finished()
{
	return this->frame_duration == this->frames_fed;
}

AVFrame* Filter::get_output_frame()
{
	return this->output_frame;
}

int Filter::feed(AVFrame* in_frame)
{
	int ret;

	// if the input frame hasn't changed, don't change the output frame
	if (in_frame->pts == this->last_pts1_fed)
		return 0;
	this->last_pts1_fed = in_frame->pts;

	frames_fed += 1;

	ret = av_buffersrc_add_frame_flags(this->buffersrc_ctx, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0;
	if (ret < 0) {
		Logger::get("filter") << "Error while feeding the filtergraph: " << av_err2str(ret) << "\n";
		return ret;
	}

	while (true) {
		ret = av_buffersink_get_frame(this->buffersink_ctx, this->output_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			Logger::get("error") << "Error while retrieving from the filter graph: " << av_err2str(ret) << "\n";
			return ret;
		}
	}

	return 0;
}

int Filter::feed(AVFrame* in, AVFrame* in2)
{
	int ret;

	// if the input frame hasn't changed, don't change the output frame
	if (in->pts == this->last_pts1_fed && in2->pts == this->last_pts2_fed)
		return 0;

	frames_fed += 1;

	if (this->last_pts1_fed != in->pts) {
		ret = av_buffersrc_write_frame(this->buffersrc_ctx, in) < 0;
		if (ret < 0) {
			Logger::get("error") << "Error while adding frame 1 to the filtergraph: " << av_err2str(ret) << "\n";
			return ret;
		}
		this->last_pts1_fed = in->pts;
	}

	if (this->buffersrc2_ctx != nullptr && this->last_pts2_fed != in2->pts) {
		ret = av_buffersrc_write_frame(this->buffersrc2_ctx, in2) < 0;
		if (ret < 0) {
			Logger::get("error") << "Error while adding frame 2 to the filtergraph: " << av_err2str(ret) << "\n";
			return ret;
		}
		this->last_pts2_fed = in2->pts;
	}

	while (true) {
		ret = av_buffersink_get_frame(this->buffersink_ctx, this->output_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			Logger::get("error") << "Error while retrieving from the filter graph: " << av_err2str(ret) << "\n";
			return ret;
		}
	}

	return 0;
}

/********
 * Clip *
 ********/
Clip::Clip(const FilePiece& file_piece)
{
	this->filename = file_piece.filename;
	this->video_start_secs = file_piece.video_start_secs;
	this->file_start_secs = file_piece.file_start_secs;
	this->duration_secs = file_piece.duration_secs;
	this->effect = FilterEffect::None;
}

Clip::Clip(const FilePiece& file_piece, enum FilterEffect effect)
{
	this->filename = file_piece.filename;
	this->video_start_secs = file_piece.video_start_secs;
	this->file_start_secs = file_piece.file_start_secs;
	this->duration_secs = file_piece.duration_secs;
	this->effect = effect;
}

Clip::~Clip()
{
}

/*************
 * FilePiece *
 *************/
FilePiece::FilePiece(std::string filename, float video_start_secs)
{
	this->filename = filename;
	this->video_start_secs = video_start_secs;
	this->file_start_secs = 0;
	this->duration_secs = Decoder_Ctx::get_duration_secs(filename);
}

FilePiece::FilePiece(std::string filename, float video_start_secs, float file_start_secs, float duration_secs)
{
	this->filename = filename;
	this->video_start_secs = video_start_secs;
	this->file_start_secs = file_start_secs;
	this->duration_secs = duration_secs;
}

FilePiece::FilePiece(const FilePiece& fp)
{
	this->filename = fp.filename;
	this->video_start_secs = fp.video_start_secs;
	this->file_start_secs = fp.file_start_secs;
	this->duration_secs = fp.duration_secs;
}

FilePiece FilePiece::with_duration(float duration_secs)
{
	return FilePiece(this->filename, this->video_start_secs, this->file_start_secs, duration_secs);
}

/********
* Track *
********/
TrackPiece::TrackPiece(FilePiece file_piece, TransitionEffect effect)
	: file(file_piece)
{
	this->transition = effect;
}

Track::Track(Video* video)
{
	this->video = video;
	this->decoder = std::make_unique<Decoder_Ctx>();
}

Track::Track(Video* video, const std::string& filename)
{
	this->video = video;
	add(FilePiece(filename, 0), TransitionEffect::None);
}

Track::Track(Video* video, FilePiece file_piece)
{
	this->video = video;
	add(file_piece, TransitionEffect::None);
}

Track::Track(Video* video, FilePiece file_piece, TransitionEffect effect)
{
	this->video = video;
	add(file_piece, effect);
}

struct FilePiece_Compare
{
	bool operator()( const FilePiece& lhs, const FilePiece& rhs ) const {
		return lhs.video_start_secs > rhs.video_start_secs;
	}
};

void Track::add(FilePiece file_piece, TransitionEffect effect)
{
	this->pieces.push_back(TrackPiece(file_piece, effect));
	this->pieces.sort([](const TrackPiece& tp1, const TrackPiece& tp2) {
		return tp1.file.video_start_secs > tp2.file.video_start_secs;
	});
	recalc_clips();
}

const std::list<Clip>& Track::get_clips()
{
	return clips;
}

float Track::get_duration_secs() const
{
	return std::accumulate(this->pieces.begin(), this->pieces.end(), 0,
		[](int duration_secs_so_far, const TrackPiece& piece) {
			return duration_secs_so_far + piece.file.duration_secs;
		}
	);
}

Clip* Track::get_next_frame_clip()
{
	return find_clip_at(this->last_shown_frame_secs);
}

Clip* Track::get_next_clip()
{
	return find_next_clip_after(this->last_shown_frame_secs);
}

float Track::get_last_video_frame_secs()
{
	return last_shown_frame_secs;
}


void Track::split(float secs, TransitionEffect effect)
{
	/*  scenario
		video_start_secs=1s - starts 1s into video
		file_start_secs=9s starts 9s into file
		duration_secs=5s lasts for 5s

		video: 1s-6s
		file: 9s-14s

		split at 3s
		1st filepiece: video=1s-3s, file 9s-11s - f/v start=unchanged, duration=secs-video_start(2)
		2nd filepiece: video=3s-6s, file 11s-14s - vstart=secs(3), fstart=fstart+secs-vstart(11), duration=duration+vstart-secs(3)
	*/

	// find file piece to split
	auto piece = std::find_if(this->pieces.begin(), this->pieces.end(),
			[secs](const TrackPiece& piece) { return piece.file.video_start_secs <= secs && secs < piece.file.video_start_secs + piece.file.duration_secs; });

	float new_piece_duration_secs = piece->file.duration_secs + piece->file.video_start_secs - secs;
	float new_piece_file_start_secs = piece->file.file_start_secs + secs - piece->file.video_start_secs;

	// lower current file piece duration
	piece->file.duration_secs = secs - piece->file.video_start_secs;
	// insert new track piece
	this->pieces.insert(std::next(piece, 1), TrackPiece(FilePiece(piece->file.filename, secs, new_piece_file_start_secs, new_piece_duration_secs), effect));

	recalc_clips();
}

void Track::recalc_clips()
{
	this->clips.clear();

	const float transition_duration_secs = 0.5f;

	for (auto track_piece = pieces.begin(); track_piece != pieces.end(); ++track_piece) {
		if (track_piece->file.duration_secs < transition_duration_secs) {
			clips.push_back(Clip(track_piece->file));
			continue;
		}

		bool has_incoming_effect = (track_piece->transition != TransitionEffect::None);
		auto next_track_piece = std::next(track_piece, 1);
		bool has_outgoing_effect = (next_track_piece != pieces.end() && next_track_piece->transition != TransitionEffect::None);

		if (has_incoming_effect)
			clips.push_back(Clip(track_piece->file.with_duration(transition_duration_secs), FilterEffect::FadeIn));

		float main_clip_file_start = track_piece->file.file_start_secs + (has_incoming_effect ? transition_duration_secs : 0);
		float main_clip_video_start = track_piece->file.video_start_secs + (has_incoming_effect ? transition_duration_secs : 0);
		float main_clip_duration = track_piece->file.duration_secs - (has_incoming_effect ? transition_duration_secs : 0) - (has_outgoing_effect ? transition_duration_secs : 0);
		if (main_clip_duration > 0)
			clips.push_back(Clip(FilePiece(track_piece->file.filename, main_clip_video_start, main_clip_file_start, main_clip_duration)));

		if (has_outgoing_effect) {
			float outgoing_clip_file_start = track_piece->file.file_start_secs + track_piece->file.duration_secs - transition_duration_secs;
			float outgoing_clip_video_start = track_piece->file.video_start_secs + track_piece->file.duration_secs - transition_duration_secs;
			clips.push_back(Clip(FilePiece(track_piece->file.filename, outgoing_clip_video_start, outgoing_clip_file_start, transition_duration_secs), FilterEffect::FadeOut));
		}
	}

	Logger::get("clip_recalc") << "track " << this << "\n";
	int p = 0;
	for (auto piece = pieces.begin(); piece != pieces.end(); ++piece) {
		Logger::get("clip_recalc") << "  piece " << p++ << "\n";
		Logger::get("clip_recalc") << "    file piece at " << std::setprecision(3) << piece->file.video_start_secs << "s: " << piece->file.filename << " from " << piece->file.file_start_secs << " for " << piece->file.duration_secs << "\n";
		Logger::get("clip_recalc") << "    transition " << piece->transition << "\n";
	}

	int c = 0;
	for (auto clip = clips.begin(); clip != clips.end(); ++clip)
		Logger::get("clip_recalc") << "clip " << c++ << " at " << clip->video_start_secs << " on " << clip->filename << " from " << clip->file_start_secs << " for " << clip->duration_secs << " with effect " << clip->effect << "\n";

	// make sure current decoders is on proper file at correct place
	Clip* cur_clip = this->get_next_frame_clip();
	if (cur_clip != nullptr) {
		float decoder_seek = this->last_shown_frame_secs - cur_clip->video_start_secs + cur_clip->file_start_secs;
		Track::ensure_decoder_at(decoder.get(), cur_clip->filename, decoder_seek);
	}

	Logger::get("clip_recalc") << "---\n";
}

Clip* Track::find_clip_at(float secs)
{
	auto clip = std::find_if(this->clips.begin(), this->clips.end(),
			[secs](const Clip& clip) { return clip.video_start_secs <= secs && secs < clip.video_start_secs + clip.duration_secs; });
	if (clip == clips.end())
		return nullptr;
	return &*clip;
}

Clip* Track::find_next_clip_after(float secs)
{
	auto clip = std::find_if(this->clips.begin(), this->clips.end(), [secs](const Clip& clip) { return clip.video_start_secs > secs; });
	if (clip == clips.end())
		return nullptr;
	return &*clip;
}

bool Track::seek(float secs)
{
	if (this->last_shown_frame_secs == secs)
		return false;

	// ensure decoders have the proper files
	Clip* cur_clip = this->find_clip_at(secs);
	float decoder_seek = secs - cur_clip->video_start_secs + cur_clip->file_start_secs;
	this->last_shown_frame_secs = secs;
	delete current_filter;
	current_filter = nullptr;
	return Track::ensure_decoder_at(decoder.get(), cur_clip->filename, decoder_seek);
}

bool Track::ensure_decoder_at(Decoder_Ctx* decoder, const std::string& filename, float seek_secs)
{
	if (decoder->filename != filename)
		decoder->open_file(filename, seek_secs);
	else if (decoder->get_last_video_frame_secs() != seek_secs)
		decoder->seek(seek_secs);
	else
		return false;
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	return true;
}

const Decoder_Ctx* Track::get_decoder() const
{
	return this->decoder.get();
}

const AVCodecContext* Track::get_audio_context() const
{
	return this->decoder->get_audio_context();
}

AVFrame* Track::get_next_audio_frame() {
	return decoder->get_audio_frame();
}

AVFrame* Track::get_video_frame(float secs)
{
	Clip* clip = find_clip_at(secs);
	if (clip == nullptr)
		return nullptr;

	// see whether we need to set up the filter
	// TODO: see if this was sequential
	Clip* last_clip = find_clip_at(this->last_shown_frame_secs);
	// get a new filter if the clip changed and we don't have a filter, it's the wrong effect, or it's finished its frames
	Filter* filter = this->current_filter;
	if (clip != last_clip && (filter == nullptr || filter->effect != clip->effect || filter->is_finished())) {
		Logger::get("filter") << "last frame " << this->last_shown_frame_secs << ", this frame " << secs << ", switching filter to " << clip->effect << "\n";
		delete this->current_filter;
		// setup the filter
		switch (clip->effect) {
			case FilterEffect::FadeOut:
				Logger::get("filter") << "creating a fadeout filter with duration " << clip->duration_secs << "s\n";
				this->current_filter = Filter::FadeOut(decoder.get(), clip->duration_secs);
				break;
			case FilterEffect::FadeIn:
				Logger::get("filter") << "creating a fadein filter with duration " << clip->duration_secs << "s\n";
				this->current_filter = Filter::FadeIn(decoder.get(), clip->duration_secs);
				break;
			case FilterEffect::None:
			default:
				this->current_filter = nullptr;
				break;
		}
	}

	AVFrame* decoded_frame = decoder->get_video_frame_at(secs - clip->video_start_secs + clip->file_start_secs);
	if (decoded_frame == nullptr) {
		Logger::get("get_video_frame") << "didn't get frame from decoder\n";
		return nullptr;
	} else if (decoded_frame->width == 0) {
		Logger::get("get_video_frame") << "got a frame with 0 width\n";
		return nullptr;
	}

	AVFrame* filtered_frame = decoded_frame;
	if (this->current_filter != nullptr) {
		int ret = this->current_filter->feed(decoded_frame);
		if (ret != 0) {
			Logger::get("error") << "error feeding the clip filter: " << av_err2str(ret) << "\n";
			return nullptr;
		}
		filtered_frame = this->current_filter->get_output_frame();
	}

	this->last_shown_frame_secs = clip->video_start_secs + decoder->get_last_video_frame_secs() - clip->file_start_secs;
	return filtered_frame;
}


/********
* Video *
********/
Video::Video() : main_track(this), overlay_track(this)
{
}

Video::~Video()
{
	if (this->out_video_frame != nullptr)
		av_frame_unref(this->out_video_frame);
	if (this->out_audio_frame != nullptr)
		av_frame_unref(this->out_audio_frame);
}

void Video::addToMainTrack(const std::string& filename, TransitionEffect effect)
{
	main_track.add(FilePiece(filename, this->main_track.get_duration_secs()), effect);
}

void Video::addToOverlayTrack(const std::string& filename, TransitionEffect effect)
{
	overlay_track.add(FilePiece(filename, this->overlay_track.get_duration_secs()), effect);
}

// returns whether the frame will change
bool Video::seek(float secs)
{
	return this->main_track.seek(secs);
}

float Video::get_duration_secs()
{
	return this->main_track.get_duration_secs();
}

float Video::get_last_video_frame_secs()
{
	return this->main_track.last_shown_frame_secs;
}

int Video::get_video_frame(float secs, int out_width, int out_height)
{
	AVFrame* main_frame = this->main_track.get_video_frame(secs);
	if (main_frame == nullptr) {
		// TODO: add status to decoder so we know whether it's got an error or operating normally
		Logger::get("error") << "xx error getting a video frame from the main track\n";
		return AVERROR(EAGAIN);
	}

	// put overlay track on top
	AVFrame* overlay_frame = this->overlay_track.get_video_frame(secs);
	if (overlay_frame != nullptr) {
		if (this->overlay_track_filter == nullptr)
			this->overlay_track_filter = Filter::OverlayTrack(main_track.get_decoder(), this->overlay_track.get_decoder(), out_width, out_height);
		int ret = this->overlay_track_filter->feed(main_frame, overlay_frame);
		if (ret != 0) {
			Logger::get("error") << "error feeding the overlay filter: " << av_err2str(ret) << "\n";
			return ret;
		}
		this->out_video_frame = this->overlay_track_filter->get_output_frame();
	} else {
		if (this->solo_track_filter == nullptr)
			this->solo_track_filter = Filter::SoloTrack(main_track.get_decoder(), out_width, out_height);
		int ret = this->solo_track_filter->feed(main_frame);
		if (ret != 0) {
			Logger::get("error") << "error feeding the overlay filter: " << av_err2str(ret) << "\n";
			return ret;
		}
		this->out_video_frame = this->solo_track_filter->get_output_frame();
	}

	return 0;
}

int Video::get_next_audio_frame()
{
	AVFrame* main_frame = this->main_track.get_next_audio_frame();
	if (main_frame == nullptr) {
		// TODO: add status to decoder so we know whether it's got an error or operating normally
		Logger::get("error") << "xx error getting an audio frame from the main track\n";
		return AVERROR(EAGAIN);
	}

	// put overlay track on top
	AVFrame* overlay_frame = this->overlay_track.get_next_audio_frame();
	if (overlay_frame == nullptr) {
		if (this->audioprep_filter == nullptr)
			this->audioprep_filter = Filter::AudioPrep(main_track.get_decoder());
		int ret = this->audioprep_filter->feed(main_frame);
		if (ret != 0) {
			Logger::get("error") << "error feeding the overlay filter: " << av_err2str(ret) << "\n";
			return ret;
		}
		this->out_audio_frame = this->audioprep_filter->get_output_frame();
		return 0;
	}

	if (this->audiomix_filter == nullptr)
		this->audiomix_filter = Filter::AudioMix(main_track.get_decoder(), this->overlay_track.get_decoder());
	int ret = this->audiomix_filter->feed(main_frame, overlay_frame);
	if (ret != 0) {
		Logger::get("error") << "error feeding the overlay filter: " << av_err2str(ret) << "\n";
		return ret;
	}
	this->out_audio_frame = this->audiomix_filter->get_output_frame();

	return 0;
}
