#pragma once

#include <map>
#include <chrono>

extern "C" {
// ffmpeg headers
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

// SDL2 headers for SDL_Mutex and SDL_Queue
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
}

class FrameClock {
public:
	FrameClock(AVRational time_base);
	int GetDelayMsec(AVFrame* frame);
	void ResetPTS(int64_t pts);

	int64_t last_pts;
	int64_t frame_pts;

protected:
	double pts_to_msec;
};

class Decoder_Ctx {
public:
	// only written in decoding thread, only read when decoding thread is finished
	int errnum;

	Decoder_Ctx();
	virtual ~Decoder_Ctx();

	void close();

	AVFrame* get_video_frame();
	AVFrame* get_audio_frame();
	AVFrame* peek_video_frame();
	AVFrame* peek_audio_frame();

	void seek(int pts);
	int open_file(const char* src_filename);

	bool has_video();
	bool has_audio();
	AVStream* get_video_stream();
	AVStream* get_audio_stream();
	AVCodecContext* get_audio_context();

protected:
	SDL_mutex* video_mutex;
	std::map<int64_t, AVFrame*> video_frames;

	SDL_mutex* audio_mutex;
	std::map<int64_t, AVFrame*> audio_frames;

	SDL_mutex* seeking_mutex;

	int64_t seek_pts;
	SDL_Thread* decoding_thread;
	int internal_start_decoding();
	int internal_seek();
	static int internal_start_decoding_thread(void* param) { return ((Decoder_Ctx*)param)->internal_start_decoding(); }

	void empty_frames();
	AVFrame* decode_frame(AVCodecContext* codec_ctx, AVPacket* pkt);

	// file
	AVFormatContext *format_ctx;

	// video stream
	int video_stream_index;
	AVCodec* video_decoder;
	AVCodecContext *video_decoder_ctx;

	// audio stream
	int audio_stream_index;
	AVCodec* audio_decoder;
	AVCodecContext *audio_decoder_ctx;
};
