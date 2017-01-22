#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#include <vector>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
extern "C" {
#include "libswresample/swresample.h"
}

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL3_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_sdl_gl3.h"

#include "common.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

#define MAX_VERTEX_MEMORY 512 * 1024
#define MAX_ELEMENT_MEMORY 128 * 1024

bool quit = false;
bool paused = false;
bool just_seeked = false;

Decoder_Ctx decoder;
SwsContext* sws_ctx;
AVFrame* rgb_frame;
FrameClock* video_clock = nullptr;
FrameClock* audio_clock = nullptr;

const char* input_file = "countdown.mp4";

#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

void audio_callback(void*, Uint8 *stream, int len) {
	// unfreed memory
	static uint8_t* audio_buf = (uint8_t*) av_malloc(MAX_AUDIO_FRAME_SIZE * 2);
	static unsigned int audio_buf_size = 0;

	int orig_len = len;

	static SwrContext* swr_ctx = nullptr;
	if (swr_ctx == nullptr) {
		AVCodecContext* decoder_ctx = decoder.get_audio_context();
		swr_ctx = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100,
			decoder_ctx->channel_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate,
		   	0, NULL);
		if (swr_ctx == nullptr) {
			printf("error allocing and setting opts on swr\n");
			exit(1);
		}

		int ret = swr_init(swr_ctx);
		if (ret < 0) {
			printf("error initing swr: %s\n", av_err2str(ret));
			exit(1);
		}
	}

	while (len > 0) {
		// check whether we have enough stored
		if (audio_buf_size >= len) {
			// copy from buffer to stream
			memcpy(stream, audio_buf, len);
			// move remainder to beginning of stream
			memmove(audio_buf, &audio_buf[len], audio_buf_size - len);
			audio_buf_size -= len;
			return;
		}

		// copy whatever we have saved
		if (audio_buf_size > 0) {
			// copy from buffer to stream
			memcpy(stream, audio_buf, audio_buf_size);
			len -= audio_buf_size;
			audio_buf_size = 0;
		}

		// read another frame and store it in the buf
		AVFrame* audio_frame = decoder.get_audio_frame();
		if (audio_frame == nullptr) {
			memset(&stream[orig_len - len], 0, len);
			return;
		}
		int samples_converted = swr_convert(swr_ctx, &audio_buf, MAX_AUDIO_FRAME_SIZE, (const uint8_t **) audio_frame->data, audio_frame->nb_samples);
		audio_buf_size = av_samples_get_buffer_size(NULL, audio_frame->channels, audio_frame->nb_samples, (enum AVSampleFormat)audio_frame->format, 1) / 2;
	}
}

void play() {
	paused = false;
	if (decoder.has_audio())
		SDL_PauseAudio(0);
}

void pause() {
	paused = true;
	SDL_PauseAudio(1);
}

void play_pause() {
	paused ? play() : pause();
}

int main(int argc, char* argv[]) {
	// video decoder
	av_register_all();
	if (decoder.open_file(input_file) != 0)
		exit(1);
	SDL_Delay(10);

	if (decoder.has_video())
		video_clock = new FrameClock(decoder.get_video_stream()->time_base);

	if (decoder.has_audio()) {
		audio_clock = new FrameClock(decoder.get_audio_stream()->time_base);
		// audio
		const int AUDIO_BUFFER_SIZE = 1024;
		SDL_AudioSpec wanted_spec;
		wanted_spec.freq = decoder.get_audio_context()->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = decoder.get_audio_context()->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}
	}

    // Platform
    SDL_Window *win;
    SDL_GLContext glContext;
    struct nk_color background;
    int win_width, win_height;

    // GUI
    struct nk_context *ctx;

    // SDL setup
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_EVENTS);
    SDL_GL_SetAttribute (SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow("Demo",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_ALLOW_HIGHDPI);
    glContext = SDL_GL_CreateContext(win);
    SDL_GetWindowSize(win, &win_width, &win_height);

    // OpenGL setup
    glViewport(0, 0, win_width, win_height);
    glewExperimental = 1;
    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to setup GLEW\n");
        exit(1);
    }

    ctx = nk_sdl_init(win);

    {
	// load fonts
	struct nk_font_atlas *atlas;
    nk_sdl_font_stash_begin(&atlas);
    nk_sdl_font_stash_end();
	}

	int video_w = 256;
	int video_h = 256;

	glEnable(GL_TEXTURE_2D);
	GLuint frame_texture;
	glGenTextures(1, &frame_texture);
	// parameters shouldn't matter since we're resizing frame to match texture size
	glBindTexture(GL_TEXTURE_2D, frame_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, video_w, video_h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	struct nk_image frame_image = nk_image_id(frame_texture);

	play();

    background = nk_rgb(28,48,62);
    while (!quit)
    {
        // Input
        SDL_Event evt;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) goto cleanup;
            nk_sdl_handle_event(&evt);
        }
        nk_input_end(ctx);

		static int video_frame_pts = -1;
		if (decoder.has_video() && (just_seeked || !paused)) {
			just_seeked = false;
			// decode a video frame
			AVFrame* decoded_frame = decoder.get_video_frame();
			if (decoded_frame != nullptr && decoded_frame->width != 0 && decoded_frame->height != 0) {
				video_frame_pts = decoded_frame->pts;

				if (sws_ctx == nullptr) {
					sws_ctx = sws_getContext(decoded_frame->width, decoded_frame->height, (enum AVPixelFormat) decoded_frame->format,
							video_w, video_h, AV_PIX_FMT_RGB24,
							SWS_BICUBIC, NULL, NULL, NULL);
					rgb_frame = av_frame_alloc();
					av_image_alloc(rgb_frame->data, rgb_frame->linesize, video_w, video_h, AV_PIX_FMT_RGB24, 1);
				}

				sws_scale(sws_ctx, decoded_frame->data, decoded_frame->linesize, 0, decoded_frame->height, rgb_frame->data, rgb_frame->linesize);

				glBindTexture(GL_TEXTURE_2D, frame_texture);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_w, video_h, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame->data[0]);

				// delay until next video frame and render
				int video_delay_msec = video_clock->GetDelayMsec(decoded_frame);
				if (video_delay_msec > 0)
					SDL_Delay(video_delay_msec);
			}
		}

        // GUI
        if (nk_begin(ctx, "Demo", nk_rect(50, 50, video_w + 50, 500),
            NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
            NK_WINDOW_CLOSABLE|NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
        {
            nk_menubar_begin(ctx);
            nk_layout_row_begin(ctx, NK_STATIC, 25, 2);
            nk_layout_row_push(ctx, 45);
            if (nk_menu_begin_label(ctx, "FILE", NK_TEXT_LEFT, nk_vec2(120, 200))) {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_menu_item_label(ctx, "OPEN", NK_TEXT_LEFT);
                nk_menu_item_label(ctx, "CLOSE", NK_TEXT_LEFT);
                nk_menu_end(ctx);
            }
            nk_layout_row_push(ctx, 45);
            if (nk_menu_begin_label(ctx, "EDIT", NK_TEXT_LEFT, nk_vec2(120, 200))) {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_menu_item_label(ctx, "COPY", NK_TEXT_LEFT);
                nk_menu_item_label(ctx, "CUT", NK_TEXT_LEFT);
                nk_menu_item_label(ctx, "PASTE", NK_TEXT_LEFT);
                nk_menu_end(ctx);
            }
            nk_layout_row_end(ctx);
            nk_menubar_end(ctx);

			nk_layout_row_static(ctx, video_h, video_w, 1);
			nk_image(ctx, frame_image);
			nk_layout_row_end(ctx);

            nk_layout_row_dynamic(ctx, 30, 2);
            if (nk_button_label(ctx, paused ? "play" : "pause"))
				play_pause();

			AVStream* video_stream = decoder.get_video_stream();
			int64_t duration = video_stream->duration;
			int64_t steps = video_stream->duration / video_stream->nb_frames;
			if (nk_slider_int(ctx, 0, &video_frame_pts, duration, 1)) {
				decoder.seek(video_frame_pts);
				just_seeked = true;
				video_clock->ResetPTS(video_frame_pts);
			}
        }
        nk_end(ctx);

        // Draw
        {
		float bg[4];
        nk_color_fv(bg, background);
        SDL_GetWindowSize(win, &win_width, &win_height);
        glViewport(0, 0, win_width, win_height);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(bg[0], bg[1], bg[2], bg[3]);
        /* IMPORTANT: `nk_sdl_render` modifies some global OpenGL state
         * with blending, scissor, face culling, depth test and viewport and
         * defaults everything back into a default state.
         * Make sure to either a.) save and restore or b.) reset your own state after
         * rendering the UI. */
        nk_sdl_render(NK_ANTI_ALIASING_ON, MAX_VERTEX_MEMORY, MAX_ELEMENT_MEMORY);
        SDL_GL_SwapWindow(win);
		}
    }

cleanup:
	quit = true;

	av_frame_free(&rgb_frame);
	sws_freeContext(sws_ctx);

    nk_sdl_shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(win);
    SDL_Quit();

    return 0;
}

