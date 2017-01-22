#include "common.h"

extern "C" {
#include "libavutil/time.h"
}

using namespace std;

// OMG global across files fix it fix it fix it
extern bool quit;

FrameClock::FrameClock(AVRational time_base) {
	this->pts_to_msec = av_q2d(time_base) * 1000.0;
	this->frame_pts = 0;
	this->last_pts = 0;
}

double time_diff_to_millis(chrono::time_point<chrono::steady_clock> end, chrono::time_point<chrono::steady_clock> start) {
	return chrono::duration_cast<chrono::milliseconds>(end - start).count();
}

int FrameClock::GetDelayMsec(AVFrame* frame) {
	static auto last_time_point = chrono::steady_clock::now();

	int64_t delta_pts = frame->pts - this->last_pts;
	double delta_msec = delta_pts * pts_to_msec;
	auto show_at = last_time_point + chrono::milliseconds((int)delta_msec);

	auto now = chrono::steady_clock::now();
	int delay_msec = 0;
	if (this->last_pts > 0 && now < show_at)
		delay_msec = (int)time_diff_to_millis(show_at, now);

	this->last_pts = frame->pts;
	last_time_point = now + chrono::milliseconds(delay_msec);
	this->frame_pts = frame->pts;

	return delay_msec;
}

void FrameClock::ResetPTS(int64_t pts) {
	this->last_pts = pts;
}

Decoder_Ctx::Decoder_Ctx() {
	this->errnum = 0;
	this->format_ctx = nullptr;

	this->video_stream_index = -1;
	this->video_decoder = nullptr;
	this->video_decoder_ctx = nullptr;

	this->audio_stream_index = -1;
	this->audio_decoder = nullptr;
	this->audio_decoder_ctx = nullptr;

	this->decoding_thread = nullptr;
	this->video_mutex = SDL_CreateMutex();
	this->audio_mutex = SDL_CreateMutex();

	this->seek_pts = -1;
	this->seeking_mutex = SDL_CreateMutex();
}

Decoder_Ctx::~Decoder_Ctx() {
	close();
}

void Decoder_Ctx::close() {
	if (this->decoding_thread != nullptr)
		SDL_DetachThread(this->decoding_thread);
	SDL_DestroyMutex(this->video_mutex);
	SDL_DestroyMutex(this->audio_mutex);

	avformat_close_input(&this->format_ctx);
	avcodec_close(this->video_decoder_ctx);

	empty_frames();
}

void Decoder_Ctx::empty_frames() {
	for (auto it = this->audio_frames.begin(); it != this->audio_frames.end(); it++)
		av_frame_free(&it->second);
	this->audio_frames.clear();

	for (auto it = this->video_frames.begin(); it != this->video_frames.end(); it++)
		av_frame_free(&it->second);
	this->video_frames.clear();
}

AVFrame* Decoder_Ctx::get_video_frame() {
	AVFrame* first_frame = nullptr;
	SDL_LockMutex(this->video_mutex);
	auto it = this->video_frames.begin();
	if (it != this->video_frames.end()) {
		first_frame = it->second;
		this->video_frames.erase(it);
	}
	SDL_UnlockMutex(this->video_mutex);
	return first_frame;
}

AVFrame* Decoder_Ctx::get_audio_frame() {
	AVFrame* first_frame = nullptr;
	SDL_LockMutex(this->audio_mutex);
	auto it = this->audio_frames.begin();
	if (it != this->audio_frames.end()) {
		first_frame = it->second;
		this->audio_frames.erase(it);
	}
	SDL_UnlockMutex(this->audio_mutex);
	return first_frame;
}

AVFrame* Decoder_Ctx::peek_video_frame() {
	AVFrame* first_frame = nullptr;
	SDL_LockMutex(this->video_mutex);
	auto it = this->video_frames.begin();
	if (it != this->video_frames.end())
		first_frame = it->second;
	SDL_UnlockMutex(this->video_mutex);
	return first_frame;
}

AVFrame* Decoder_Ctx::peek_audio_frame() {
	AVFrame* first_frame = nullptr;
	SDL_LockMutex(this->audio_mutex);
	auto it = this->audio_frames.begin();
	if (it != this->audio_frames.end())
		first_frame = it->second;
	SDL_UnlockMutex(this->audio_mutex);
	return first_frame;
}

int Decoder_Ctx::internal_start_decoding() {
	AVPacket pkt;

	while (!quit) {
		SDL_LockMutex(this->seeking_mutex);
		internal_seek();
		SDL_UnlockMutex(this->seeking_mutex);

		// wait until we need frames - switch to condition
		int need_video_frames = this->video_stream_index >= 0 ? 10 : 0;
		int need_audio_frames = this->audio_stream_index >= 0 ? 10 : 0;
		if (this->video_frames.size() >= need_video_frames && this->audio_frames.size() >= need_audio_frames) {
			SDL_Delay(1);
			continue;
		}

		// read packet until we get a video frame
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		this->errnum = av_read_frame(this->format_ctx, &pkt);
		if (this->errnum < 0) {
			fprintf(stderr, "Error reading frame (%s)\n", av_err2str(this->errnum));
			return this->errnum;
		}

		if (pkt.stream_index == this->audio_stream_index) {
			AVFrame* frame = decode_frame(this->audio_decoder_ctx, &pkt);
			av_packet_unref(&pkt);

			if (frame == nullptr && this->errnum == AVERROR(EAGAIN))
				continue;
			if (frame == nullptr)
				return this->errnum;

			SDL_LockMutex(this->audio_mutex);
			this->audio_frames[frame->pts] = frame;
			SDL_UnlockMutex(this->audio_mutex);
		} else if (pkt.stream_index == this->video_stream_index) {
			AVFrame* frame = decode_frame(this->video_decoder_ctx, &pkt);
			av_packet_unref(&pkt);

			if (frame == nullptr && this->errnum == AVERROR(EAGAIN))
				continue;
			if (frame == nullptr)
				return this->errnum;

//printf("read video frame with pts %lld\n", frame->pts);
			SDL_LockMutex(this->video_mutex);
			this->video_frames[frame->pts] = frame;
			SDL_UnlockMutex(this->video_mutex);
		}

	}

	return 0;
}

AVFrame* Decoder_Ctx::decode_frame(AVCodecContext* codec_ctx, AVPacket* pkt) {
	// decode video frame
	this->errnum = avcodec_send_packet(codec_ctx, pkt);
	// TODO: handle interesting return codes if any

	// read more packets until we have received a frame
	AVFrame* frame = av_frame_alloc();
	this->errnum = avcodec_receive_frame(codec_ctx, frame);
	if (this->errnum < 0) {
		// EAGAIN is OK
		if (this->errnum != AVERROR(EAGAIN))
			fprintf(stderr, "Error decoding frame (%s)\n", av_err2str(this->errnum));
		return nullptr;
	}

	return frame;
}

void Decoder_Ctx::seek(int target_pts) {
	SDL_LockMutex(this->seeking_mutex);
	this->seek_pts = target_pts;
	SDL_UnlockMutex(this->seeking_mutex);
}

int Decoder_Ctx::internal_seek() {
	if (this->seek_pts == -1)
		return 0;

	SDL_LockMutex(this->audio_mutex);
	SDL_LockMutex(this->video_mutex);

	empty_frames();

	// seek to the previous iframe
	av_seek_frame(this->format_ctx, this->video_stream_index, this->seek_pts, AVSEEK_FLAG_BACKWARD);

	AVPacket pkt;
	while (pkt.pts < this->seek_pts) {
		this->errnum = av_read_frame(this->format_ctx, &pkt);
		if (this->errnum < 0) {
			fprintf(stderr, "Error reading video frame after seeking (%s)\n", av_err2str(this->errnum));
			return this->errnum;
		}
		av_packet_unref(&pkt);
	}

	this->seek_pts = -1;

	SDL_UnlockMutex(this->audio_mutex);
	SDL_UnlockMutex(this->video_mutex);

	return 0;
}

void print_error(int code, const char *msg, ...) {
	va_list args;
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	fprintf(stderr, "  %s\n", av_err2str(code));
}

int Decoder_Ctx::open_file(const char* src_filename) {
	int ret = 0;

	// open decoder file, and allocate format context
	ret = avformat_open_input(&this->format_ctx, src_filename, nullptr, nullptr);
	if (ret < 0) {
		print_error(ret, "Could not open source file %s\n", src_filename);
		return ret;
	}

	// retrieve stream information
	ret = avformat_find_stream_info(this->format_ctx, nullptr);
	if (ret < 0) {
		print_error(ret, "Could not find stream information\n");
		return ret;
	}

	// open video decoder and context
	this->video_stream_index = av_find_best_stream(this->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (this->video_stream_index >= 0) {
		// find decoder
		this->video_decoder = avcodec_find_decoder(this->format_ctx->streams[this->video_stream_index]->codecpar->codec_id);
		if (!this->video_decoder) {
			fprintf(stderr, "Failed to find %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
			ret = AVERROR(EINVAL);
			return ret;
		}

		// allocate decoder context
		this->video_decoder_ctx = avcodec_alloc_context3(this->video_decoder);
		if (!this->video_decoder_ctx) {
			fprintf(stderr, "Could not allocate a decoding context\n");
			ret = AVERROR(ENOMEM);
			return ret;
		}

		// initialize the stream parameters with demuxer information
		ret = avcodec_parameters_to_context(this->video_decoder_ctx, this->format_ctx->streams[this->video_stream_index]->codecpar);
		if (ret < 0) {
			fprintf(stderr, "Failed to copy parameters to context\n");
			return ret;
		}

		if ((ret = avcodec_open2(this->video_decoder_ctx, this->video_decoder, nullptr)) < 0) {
			fprintf(stderr, "Failed to open %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
			return ret;
		}
	}

	// open audio decoder and context
	this->audio_stream_index = av_find_best_stream(this->format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (this->audio_stream_index >= 0) {
		// find decoder
		this->audio_decoder = avcodec_find_decoder(this->format_ctx->streams[this->audio_stream_index]->codecpar->codec_id);
		if (!this->audio_decoder) {
			fprintf(stderr, "Failed to find %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
			ret = AVERROR(EINVAL);
			return ret;
		}

		// allocate decoder context
		this->audio_decoder_ctx = avcodec_alloc_context3(this->audio_decoder);
		if (!this->audio_decoder_ctx) {
			fprintf(stderr, "Could not allocate a decoding context\n");
			ret = AVERROR(ENOMEM);
			return ret;
		}

		// initialize the stream parameters with demuxer information
		ret = avcodec_parameters_to_context(this->audio_decoder_ctx, this->format_ctx->streams[this->audio_stream_index]->codecpar);
		if (ret < 0) {
			fprintf(stderr, "Failed to copy parameters to context\n");
			return ret;
		}

		if ((ret = avcodec_open2(this->audio_decoder_ctx, this->audio_decoder, nullptr)) < 0) {
			fprintf(stderr, "Failed to open %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
			return ret;
		}
	}

	this->decoding_thread = SDL_CreateThread(&internal_start_decoding_thread, "DecoderThread", this);

	return 0;
}

bool Decoder_Ctx::has_video() {
	return this->video_stream_index >= 0;
}

bool Decoder_Ctx::has_audio() {
	return this->audio_stream_index >= 0;
}

AVStream* Decoder_Ctx::get_video_stream() {
	if (this->video_stream_index < 0)
		return nullptr;
	return this->format_ctx->streams[this->video_stream_index];
}

AVStream* Decoder_Ctx::get_audio_stream() {
	return this->format_ctx->streams[this->audio_stream_index];
}

AVCodecContext* Decoder_Ctx::get_video_context() {
	return this->video_decoder_ctx;
}

AVCodecContext* Decoder_Ctx::get_audio_context() {
	return this->audio_decoder_ctx;
}
