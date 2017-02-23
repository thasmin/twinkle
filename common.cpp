#include "common.h"

#include <iomanip>

extern "C"
{
#include "libavutil/time.h"
}

#include "logger.h"

using namespace std;

float Decoder_Ctx::get_duration_secs(const std::string& filename)
{
	AVFormatContext* format_ctx = nullptr;

	// open decoder file, and allocate format context
	int ret = avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr);
	if (ret < 0) {
		Logger::get("error") << "Could not open source file " << filename << ": " << av_err2str(ret) << "\n";
		return -1;
	}

	// retrieve stream information
	ret = avformat_find_stream_info(format_ctx, nullptr);
	if (ret < 0) {
		Logger::get("error") << "Could not find stream information in file " << filename << ": " << av_err2str(ret) << "\n";
		return -1;
	}

	if (format_ctx->nb_streams < 1) {
		Logger::get("error") << "No streams in file " << filename << "\n";
		return -1;
	}

	AVStream* stream = format_ctx->streams[0];
	float duration = av_q2d(stream->time_base) * stream->duration;

	avformat_close_input(&format_ctx);
	return duration;
}

Decoder_Ctx::Decoder_Ctx()
{
	this->errnum = 0;
	this->format_ctx = nullptr;

	this->video_stream_index = -1;
	this->video_decoder = nullptr;
	this->video_decoder_ctx = nullptr;

	this->audio_stream_index = -1;
	this->audio_decoder = nullptr;
	this->audio_decoder_ctx = nullptr;

	this->seek_secs = -1;
	this->stop_decoding_thread = false;
}

Decoder_Ctx::~Decoder_Ctx()
{
	close();
}

void Decoder_Ctx::close()
{
	this->stop_decoding_thread = true;
	if (decoding_thread.joinable())
		decoding_thread.join();
	this->stop_decoding_thread = false;

	avformat_close_input(&this->format_ctx);
	if (this->video_decoder_ctx != nullptr)
		avcodec_close(this->video_decoder_ctx);
	if (this->audio_decoder_ctx != nullptr)
		avcodec_close(this->audio_decoder_ctx);

	empty_frame_caches();
}

void Decoder_Ctx::empty_frame_caches()
{
	for (auto it = this->audio_frames.begin(); it != this->audio_frames.end(); it++)
		av_frame_free(&it->second);
	this->audio_frames.clear();

	for (auto it = this->video_frames.begin(); it != this->video_frames.end(); it++)
		av_frame_free(&it->second);
	this->video_frames.clear();
}

AVFrame* Decoder_Ctx::get_video_frame()
{
	AVFrame* first_frame = nullptr;
	this->video_mutex.lock();
	auto it = this->video_frames.begin();
	if (it != this->video_frames.end()) {
		first_frame = it->second;
		this->last_video_frame_secs = first_frame->pts * av_q2d(this->get_video_stream()->time_base);
		this->video_frames.erase(it);
	}
	this->video_mutex.unlock();
	return first_frame;
}

AVFrame* Decoder_Ctx::get_audio_frame()
{
	AVFrame* first_frame = nullptr;
	this->audio_mutex.lock();
	auto it = this->audio_frames.begin();
	if (it != this->audio_frames.end()) {
		first_frame = it->second;
		this->audio_frames.erase(it);
	}
	this->audio_mutex.unlock();
	return first_frame;
}

float Decoder_Ctx::get_last_video_frame_secs()
{
	return this->last_video_frame_secs;
}

AVFrame* Decoder_Ctx::peek_video_frame()
{
	AVFrame* first_frame = nullptr;
	this->video_mutex.lock();
	auto it = this->video_frames.begin();
	if (it != this->video_frames.end())
		first_frame = it->second;
	this->video_mutex.unlock();
	return first_frame;
}

AVFrame* Decoder_Ctx::peek_audio_frame()
{
	AVFrame* first_frame = nullptr;
	this->audio_mutex.lock();
	auto it = this->audio_frames.begin();
	if (it != this->audio_frames.end())
		first_frame = it->second;
	this->audio_mutex.unlock();
	return first_frame;
}

AVFrame* Decoder_Ctx::internal_get_frame_at(float secs, int media_type)
{
	std::map<int64_t, AVFrame*>* cache;
	std::mutex* mutex;
	std::ostream* logger;
	const AVStream* stream;
	std::string media;

	if (media_type == AVMEDIA_TYPE_VIDEO) {
		cache = &this->video_frames;
		mutex = &this->video_mutex;
		logger = &Logger::get("get_video_frame");
		stream = this->get_video_stream();
		media = "video";
	} else if (media_type == AVMEDIA_TYPE_AUDIO) {
		cache = &this->audio_frames;
		mutex = &this->audio_mutex;
		logger = &Logger::get("get_audio_frame");
		stream = this->get_audio_stream();
		media = "audio";
	} else {
		Logger::get("error") << "unknown media type while getting frame\n";
		return nullptr;
	}

	int64_t target_pts = this->get_pts_at(stream, secs);
	*logger << "got request for frame at " << secs << ", pts " << target_pts << "\n";
	*logger << media << " time base " << stream->time_base.num << " / " << stream->time_base.den << "\n";

	mutex->lock();
	if (cache->size() == 0) {
		Logger::get("decoder") << "decoder " << this << " no cached " << media << " frames\n";
		mutex->unlock();
		return nullptr;
	}

	// assumes are sequential
	// if frame cache doesn't have pts, seek
	auto first_frame = cache->begin();
	auto last_frame = std::next(cache->end(), -1);
	*logger << "cache has pts " << first_frame->first << " to " << last_frame->first << "\n";
	if (last_frame->first < target_pts || first_frame->first > target_pts) {
		*logger << "seeking to " << secs << "\n";
		mutex->unlock();
		seek(secs);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		mutex->lock();

		first_frame = cache->begin();
		last_frame = std::next(cache->end(), -1);
		*logger << "done seeking, cache has pts " << first_frame->first << " to " << last_frame->first << "\n";
	}

	// if the frame still isn't in the frame cache, we don't have it
	if (last_frame->first < target_pts || first_frame->first > target_pts) {
		*logger << "frame not found after seeking\n";
		mutex->unlock();
		return nullptr;
	}

	// remove the first frames from the queue until we found the right one
	AVFrame* frame = first_frame->second;
	auto second_frame = std::next(first_frame, 1);
	*logger << "first frame pts " << first_frame->first << ", second frame pts " << second_frame->first << "\n";
	while (second_frame != cache->end() && second_frame->first < target_pts) {
		*logger << "skipping to second frame\n";
		cache->erase(first_frame);
		first_frame = cache->begin();
		second_frame = std::next(first_frame, 1);
		frame = first_frame->second;
	}

	*logger << "returning frame with pts " << frame->pts << "\n";

	if (media_type == AVMEDIA_TYPE_VIDEO) {
		this->last_video_frame_secs = frame->pts * av_q2d(stream->time_base);
		Logger::get("get_video_frame") << "decoder pts " << frame->pts << ", last_video_frame_secs: " << std::setprecision(3) << this->last_video_frame_secs << "\n---\n";
	}

	mutex->unlock();

	return frame;
}

AVFrame* Decoder_Ctx::get_audio_frame_at(float secs)
{
	return internal_get_frame_at(secs, AVMEDIA_TYPE_AUDIO);
}

AVFrame* Decoder_Ctx::get_video_frame_at(float secs)
{
	return internal_get_frame_at(secs, AVMEDIA_TYPE_VIDEO);
}

int Decoder_Ctx::read_and_decode(AVFormatContext* format_ctx, AVFrame** out_frame)
{
	*out_frame = nullptr;
	int stream_index = -1;

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = nullptr;
	pkt.size = 0;

	while (*out_frame == nullptr) {
		this->errnum = av_read_frame(this->format_ctx, &pkt);
		Logger::get("decoder") << "decoder " << this << " read frame for decoding, pts " << pkt.pts << "\n";
		if (this->errnum < 0) {
			Logger::get("error") << "decoder " << this << "Error reading frame: " << av_err2str(this->errnum) << "\n";
			av_packet_unref(&pkt);
			return this->errnum;
		}

		AVCodecContext* codec_ctx = nullptr;
		stream_index = pkt.stream_index;
		if (stream_index == this->audio_stream_index) {
			codec_ctx = this->audio_decoder_ctx;
		} if (stream_index == this->video_stream_index) {
			codec_ctx = this->video_decoder_ctx;
		}

		// keep reading if it's not an interesting stream
		if (codec_ctx == nullptr)
			continue;

		// decode the packet
		*out_frame = decode_frame(codec_ctx, &pkt);
		if (*out_frame == nullptr && this->errnum != AVERROR(EAGAIN))
			return this->errnum;
	}

	av_packet_unref(&pkt);
	return stream_index;
}

int Decoder_Ctx::internal_start_decoding()
{
	while (!this->stop_decoding_thread) {
		this->seeking_mutex.lock();
		internal_seek();
		this->seeking_mutex.unlock();

		// wait until we need frames - switch to condition
		int need_video_frames = this->video_stream_index >= 0 ? 10 : 0;
		int need_audio_frames = this->audio_stream_index >= 0 ? 10 : 0;
		if (this->video_frames.size() >= need_video_frames && this->audio_frames.size() >= need_audio_frames) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		AVFrame* decoded_frame = nullptr;
		int stream_index = this->read_and_decode(this->format_ctx, &decoded_frame);

		if (stream_index < 0) {
			Logger::get("error") << "decoder " << this << "got an error while reading and decoding: " << av_err2str(this->errnum) << "\n";
			return stream_index;
		} else if (stream_index == this->audio_stream_index) {
			this->audio_mutex.lock();
			this->audio_frames[decoded_frame->pts] = decoded_frame;
			Logger::get("decoder") << "decoder " << this << " decoded a audio frame with pts " << decoded_frame->pts << ", cache now has " << this->audio_frames.size() << " frames\n";
			this->audio_mutex.unlock();
		} else if (stream_index == this->video_stream_index) {
			this->video_mutex.lock();
			this->video_frames[decoded_frame->pts] = decoded_frame;
			Logger::get("decoder") << "decoder " << this << " decoded a video frame with pts " << decoded_frame->pts << ", cache now has " << this->video_frames.size() << " frames\n";
			this->video_mutex.unlock();
		}
	}

	return 0;
}

// unrefs the packet after use
AVFrame* Decoder_Ctx::decode_frame(AVCodecContext* codec_ctx, AVPacket* pkt)
{
	// decode video frame
	this->errnum = avcodec_send_packet(codec_ctx, pkt);
	av_packet_unref(pkt);
	// TODO: handle interesting return codes if any

	// read more packets until we have received a frame
	AVFrame* frame = av_frame_alloc();
	this->errnum = avcodec_receive_frame(codec_ctx, frame);
	if (this->errnum < 0) {
		// EAGAIN is OK
		if (this->errnum != AVERROR(EAGAIN))
			Logger::get("error") << "decoder " << this << "Error decoding frame: " << av_err2str(this->errnum) << "\n";
		return nullptr;
	}

	return frame;
}

// returns pts for secs
int64_t Decoder_Ctx::seek(float target_secs)
{
	Logger::get("decoder") << "decoder " << this << " decoder seeking to " << target_secs << "\n";
	this->seeking_mutex.lock();
	this->seek_secs = target_secs;
	this->seeking_mutex.unlock();
	return av_q2d(this->get_video_stream()->time_base) * this->seek_secs;
}

int Decoder_Ctx::internal_seek()
{
	this->errnum = 0;

	if (this->seek_secs == -1)
		return 0;

	this->audio_mutex.lock();
	this->video_mutex.lock();

	reopen_audio_context();
	reopen_video_context();
	empty_frame_caches();

	// seek to the previous iframe
	int64_t seek_pts = this->seek_secs / av_q2d(this->get_video_stream()->time_base);
	av_seek_frame(this->format_ctx, this->video_stream_index, seek_pts, AVSEEK_FLAG_BACKWARD);

	// read packets until found the right pts on the right stream
	if (this->seek_secs > 0) {
		AVPacket pkt;
		av_init_packet(&pkt);
		while (true) {
			AVFrame* decoded_frame;
			int stream_index = this->read_and_decode(this->format_ctx, &decoded_frame);

			if (stream_index < 0) {
				Logger::get("error") << "decoder " << this << "Error reading video frame after seeking: " << av_err2str(this->errnum) << "\n";
				return stream_index;
			}

			// this is the next video frame
			if (stream_index == this->video_stream_index && decoded_frame->pts >= seek_pts) {
				this->video_frames[decoded_frame->pts] = decoded_frame;
				Logger::get("get_video_frame") << "seeked to a video frame with pts " << decoded_frame->pts << "\n";
				break;
			}
		}
	}

	this->seek_secs = -1;

	this->video_mutex.unlock();
	this->audio_mutex.unlock();

	return 0;
}

int Decoder_Ctx::reopen_audio_context() {
	int ret;

	if (!has_audio())
		return 0;

	if (this->audio_decoder_ctx != nullptr)
		avcodec_close(this->audio_decoder_ctx);

	// allocate decoder context
	this->audio_decoder_ctx = avcodec_alloc_context3(this->audio_decoder);
	if (!this->audio_decoder_ctx) {
		Logger::get("error") << "decoder " << this << "Could not allocate a decoding context\n";
		ret = AVERROR(ENOMEM);
		return ret;
	}

	// initialize the stream parameters with demuxer information
	ret = avcodec_parameters_to_context(this->audio_decoder_ctx, this->format_ctx->streams[this->audio_stream_index]->codecpar);
	if (ret < 0) {
		Logger::get("error") << "decoder " << this << "Failed to copy parameters to context\n";
		return ret;
	}

	if ((ret = avcodec_open2(this->audio_decoder_ctx, this->audio_decoder, nullptr)) < 0) {
		Logger::get("error") << "decoder " << this << "Failed to open " << av_get_media_type_string(AVMEDIA_TYPE_AUDIO) << " codec\n";
		return ret;
	}

	return 0;
}


int Decoder_Ctx::reopen_video_context() {
	int ret;

	if (!has_video())
		return 0;

	if (this->video_decoder_ctx != nullptr)
		avcodec_close(this->video_decoder_ctx);

	// allocate decoder context
	this->video_decoder_ctx = avcodec_alloc_context3(this->video_decoder);
	if (!this->video_decoder_ctx) {
		Logger::get("error") << "decoder " << this << "Could not allocate a decoding context\n";
		ret = AVERROR(ENOMEM);
		return ret;
	}

	// initialize the stream parameters with demuxer information
	ret = avcodec_parameters_to_context(this->video_decoder_ctx, this->format_ctx->streams[this->video_stream_index]->codecpar);
	if (ret < 0) {
		Logger::get("error") << "decoder " << this << "Failed to copy parameters to context\n";
		return ret;
	}

	if ((ret = avcodec_open2(this->video_decoder_ctx, this->video_decoder, nullptr)) < 0) {
		Logger::get("error") << "decoder " << this << "Failed to open " << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << " codec\n";
		return ret;
	}

	return 0;
}

int Decoder_Ctx::open_file(const std::string& filename, float seek_secs)
{
	int ret = this->internal_open_file(filename);
	if (ret < 0)
		return ret;

	if (seek_secs > 0)
		this->seek(seek_secs);
	this->decoding_thread = std::thread(&Decoder_Ctx::internal_start_decoding, this);
	return 0;
}

int Decoder_Ctx::open_file(const std::string& filename)
{
	int ret = this->internal_open_file(filename);
	if (ret < 0)
		return ret;

	this->decoding_thread = std::thread(&Decoder_Ctx::internal_start_decoding, this);
	return 0;
}

int Decoder_Ctx::internal_open_file(const std::string& filename)
{
	int ret = 0;

	close();

	// open decoder file, and allocate format context
	ret = avformat_open_input(&this->format_ctx, filename.c_str(), nullptr, nullptr);
	if (ret < 0) {
		Logger::get("error") << "decoder " << this << "Could not open source file " << filename << ": " << av_err2str(ret) << "\n";
		return ret;
	}
	this->filename = filename;

	// retrieve stream information
	ret = avformat_find_stream_info(this->format_ctx, nullptr);
	if (ret < 0) {
		Logger::get("error") << "decoder " << this << "Could not find stream information: " << av_err2str(ret) << "\n";
		return ret;
	}

	// open video decoder and context
	this->video_stream_index = av_find_best_stream(this->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (this->video_stream_index >= 0) {
		// find decoder
		this->video_decoder = avcodec_find_decoder(this->format_ctx->streams[this->video_stream_index]->codecpar->codec_id);
		if (!this->video_decoder) {
			Logger::get("error") << "decoder " << this << "Failed to find " << av_get_media_type_string(AVMEDIA_TYPE_VIDEO) << " codec\n";
			ret = AVERROR(EINVAL);
			return ret;
		}

		if (reopen_video_context() < 0)
			return ret;
	}

	// open audio decoder and context
	this->audio_stream_index = av_find_best_stream(this->format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (this->audio_stream_index >= 0) {
		// find decoder
		this->audio_decoder = avcodec_find_decoder(this->format_ctx->streams[this->audio_stream_index]->codecpar->codec_id);
		if (!this->audio_decoder) {
			Logger::get("error") << "decoder " << this << "Failed to find " << av_get_media_type_string(AVMEDIA_TYPE_AUDIO) << " codec\n";
			ret = AVERROR(EINVAL);
			return ret;
		}

		if (reopen_audio_context() < 0)
			return ret;
	}

	return 0;
}

bool Decoder_Ctx::has_video()
{
	return this->video_stream_index >= 0;
}

bool Decoder_Ctx::has_audio()
{
	return this->audio_stream_index >= 0;
}

const AVStream* Decoder_Ctx::get_video_stream() const
{
	if (this->video_stream_index < 0)
		return nullptr;
	return this->format_ctx->streams[this->video_stream_index];
}

const AVStream* Decoder_Ctx::get_audio_stream() const
{
	return this->format_ctx->streams[this->audio_stream_index];
}

const AVCodecContext* Decoder_Ctx::get_video_context() const
{
	return this->video_decoder_ctx;
}

const AVCodecContext* Decoder_Ctx::get_audio_context() const
{
	return this->audio_decoder_ctx;
}

int Decoder_Ctx::get_num_frames_in(float duration_secs) const
{
	return duration_secs * av_q2d(get_video_stream()->avg_frame_rate);
}

int64_t Decoder_Ctx::get_pts_at(const AVStream* stream, float secs) const
{
	return secs / av_q2d(stream->time_base) + stream->start_time;
}
