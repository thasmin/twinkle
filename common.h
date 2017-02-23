#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
// ffmpeg headers
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
}

class Decoder_Ctx {
public:
	// only written in decoding thread, only read when decoding thread is finished
	int errnum;
	std::string filename;

	static float get_duration_secs(const std::string& filename);

	Decoder_Ctx();
	virtual ~Decoder_Ctx();

	void close();

	AVFrame* get_video_frame();
	AVFrame* get_audio_frame();
	AVFrame* peek_video_frame();
	AVFrame* peek_audio_frame();
	AVFrame* get_video_frame_at(float secs);
	AVFrame* get_audio_frame_at(float secs);
	float get_last_video_frame_secs();

	int64_t seek(float target_secs);
	int open_file(const std::string& filename);
	int open_file(const std::string& filename, float seek_secs);

	bool has_video();
	bool has_audio();
	const AVStream* get_video_stream() const;
	const AVStream* get_audio_stream() const;
	const AVCodecContext* get_video_context() const;
	const AVCodecContext* get_audio_context() const;

	int get_num_frames_in(float duration_secs) const;
	int64_t get_pts_at(const AVStream* stream, float secs) const;

protected:
	std::mutex video_mutex;
	std::map<int64_t, AVFrame*> video_frames;
	float last_video_frame_secs = 0;

	std::mutex audio_mutex;
	std::map<int64_t, AVFrame*> audio_frames;

	std::mutex seeking_mutex;

	int internal_open_file(const std::string& filename);
	AVFrame* internal_get_frame_at(float secs, int media_type);

	float seek_secs; // switch to atomic_float?
	std::atomic_bool stop_decoding_thread;
	std::thread decoding_thread;
	int internal_start_decoding();
	int internal_seek();
	static int internal_start_decoding_thread(void* param) { return ((Decoder_Ctx*)param)->internal_start_decoding(); }

	void empty_frame_caches();
	AVFrame* decode_frame(AVCodecContext* codec_ctx, AVPacket* pkt);

	// file
	AVFormatContext* format_ctx;
	int read_and_decode(AVFormatContext* format_ctx, AVFrame** out_frame);

	// video stream
	int video_stream_index;
	AVCodec* video_decoder;
	AVCodecContext *video_decoder_ctx;
	int reopen_video_context();

	// audio stream
	int audio_stream_index;
	AVCodec* audio_decoder;
	AVCodecContext *audio_decoder_ctx;
	int reopen_audio_context();
};
