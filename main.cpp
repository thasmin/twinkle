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
#include <list>
#include <string>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
extern "C"
{
#include <libswresample/swresample.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
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
#include "clip.h"

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

#define MAX_VERTEX_MEMORY 512 * 1024
#define MAX_ELEMENT_MEMORY 128 * 1024

bool quit = false;
bool paused = true;
float last_display_secs = 0;
float last_seek_secs = -1;
bool just_seeked = true;
float clips_bar_last_click_secs = -1;

struct nk_font_atlas *atlas;

Decoder_Ctx decoder;
SwsContext* sws_ctx;
AVFrame* rgb_frame;
FrameClock* video_clock = nullptr;
FrameClock* audio_clock = nullptr;

// filter graph data
//const char *filter_str = "drawbox=x=10:y=10:w=200:h=200:color=blue:t=10";
//const char *filter_str = "fade=t=out:st=1,fade=t=in:st=2"; // doesn't work because the fade in sets beginning to black, fade out sets end as black, so it's all black
//const char *filter_str = "fade=t=in:st=1,fade=t=out:st=2"; // works

std::list<Clip> clips;

const char* input_file = "countdown.mp4";

#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

void audio_callback(void*, Uint8 *stream, int len)
{
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

void play()
{
	paused = false;
	if (decoder.has_audio())
		SDL_PauseAudio(0);
}

void pause()
{
	paused = true;
	SDL_PauseAudio(1);
}

void play_pause()
{
	paused ? play() : pause();
}

void seek(float seek_secs) {
	if (last_seek_secs == seek_secs)
		return;

//printf("seeking to %.2f secs\n", seek_secs);
	int seek_pts = decoder.seek(seek_secs);
	if (video_clock != nullptr)
		video_clock->ResetPTS(seek_pts);
	last_seek_secs = seek_secs;
	just_seeked = true;
}

void widget_clips_bar(struct nk_context *ctx)
{
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);

	struct nk_rect space;
	enum nk_widget_layout_states widget_state = nk_widget(&space, ctx);
	if (!widget_state)
		return;

	float video_duration = av_q2d(decoder.get_video_stream()->time_base) * decoder.get_video_stream()->duration;
	float pixels_per_sec = space.w / video_duration;
	// color scheme from https://flatuicolors.com/
	struct nk_color fill_colors[] = {
		nk_rgb(192, 57, 43),
		nk_rgb(211, 84, 0),
		nk_rgb(243, 156, 18),
		nk_rgb(39, 174, 96),
		nk_rgb(41, 128, 185),
		nk_rgb(142, 68, 173),
		nk_rgb(44, 62, 80),
		nk_rgb(127, 140, 141),
		nk_rgb(189, 195, 199),
	};
	struct nk_color line_colors[] = {
		nk_rgb(231, 76, 60),
		nk_rgb(230, 126, 34),
		nk_rgb(241, 196, 15),
		nk_rgb(46, 204, 113),
		nk_rgb(52, 152, 219),
		nk_rgb(155, 89, 182),
		nk_rgb(52, 73, 94),
		nk_rgb(149, 165, 166),
		nk_rgb(236, 240, 241),
	};

	// draw boxes for clips
	int max_height = 28;
	int color_num = 0;
	for (auto clip_it = clips.begin(); clip_it != clips.end(); ++clip_it) {
		float start = space.w * clip_it->start_secs / video_duration + 1; // add 1 for line thickness
		float width = space.w * clip_it->duration_secs / video_duration - 10; // dunno why need to subtract 9
		float height = max_height < space.h ? max_height : space.h;
		struct nk_rect size = nk_rect(space.x + start, space.y + 1, space.x + width, height);
		nk_fill_rect(canvas, size, 2, fill_colors[color_num]);
		nk_stroke_rect(canvas, size, 2, 3, line_colors[color_num]);
		color_num += 1;
	}

	// draw marks for each second
	for (int i = 0; i < video_duration; ++i) {
		struct nk_rect mark = nk_rect(space.x + space.w * i / video_duration, space.y + space.h - 10, 1, 10);
		nk_stroke_rect(canvas, mark, 0, 1, nk_rgb(200,200,200));
	}

	// draw full time
	char time[10];
	int timelen = snprintf(time, 10, "%ds", (int) video_duration);
	const struct nk_user_font* def_font = ctx->style.font;
	int width = def_font->width(def_font->userdata, def_font->height, time, timelen);
	int height = def_font->height;
	struct nk_rect time_rect = nk_rect(space.x + space.w - width, space.y + space.h - height, width, height);
	nk_draw_text(canvas, time_rect, time, timelen, def_font, nk_black, nk_rgb(200,200,200));

	float mouse_x = ctx->input.mouse.pos.x;

	// draw hover vertical line
	if (nk_input_is_mouse_hovering_rect(&ctx->input, space))
		nk_stroke_line(canvas, mouse_x, space.y, mouse_x, space.y + space.h, 1, nk_rgb(200,200,200));

	// draw selection vertical line
	if (clips_bar_last_click_secs != -1) {
		int selection_x = space.x + space.w * clips_bar_last_click_secs / video_duration;
		nk_stroke_line(canvas, selection_x, space.y, selection_x, space.y + space.h, 3, nk_rgb(100, 100, 200));
	}

	// draw current place indicator
	int current_x = space.x + space.w * last_display_secs / video_duration;
	nk_stroke_line(canvas, current_x, space.y, current_x, space.y + space.h, 1, nk_rgb(100,100,100));

	// handle seeks via click or drag
	if (widget_state != NK_WIDGET_ROM) {
		if (nk_input_has_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, space, nk_true)) {
			float mouse_x = ctx->input.mouse.pos.x;
			float mouse_x_pct = (mouse_x - space.x) / space.w;
			float seek_secs = mouse_x_pct * video_duration;
			seek(seek_secs);

			clips_bar_last_click_secs = seek_secs;
		}
	}

	// border
	//nk_stroke_rect(canvas, space, 0, 1, nk_rgb(200,200,200));
}

void split_clip()
{
	if (clips_bar_last_click_secs == -1)
		return;

	auto clip_it = std::find_if(clips.begin(), clips.end(),
			[](const Clip& clip) { return clip.start_secs <= last_display_secs && last_display_secs < clip.start_secs + clip.duration_secs; });
	if (clip_it == clips.end())
		return;

	Clip to_add = Clip(clip_it->decoder, clips_bar_last_click_secs, clip_it->duration_secs + clip_it->start_secs - clips_bar_last_click_secs);
	clip_it->duration_secs = clips_bar_last_click_secs - clip_it->start_secs;
	clips.insert(++clip_it, to_add);
}

int main(int argc, char* argv[])
{
	// video decoder
	av_register_all();
	avfilter_register_all();

	if (decoder.open_file(input_file) != 0)
		exit(1);
	SDL_Delay(10);

	if (decoder.has_video()) {
		video_clock = new FrameClock(decoder.get_video_stream()->time_base);
		//clips.push_back(Clip(&decoder, "fade=t=out:st=1", 0, 2));
		//clips.push_back(Clip(&decoder, "fade=t=in:st=0", 2, av_q2d(decoder.get_video_stream()->time_base) * decoder.get_video_stream()->duration - 2));
		clips.push_back(Clip(&decoder, 0, av_q2d(decoder.get_video_stream()->time_base) * decoder.get_video_stream()->duration));
	}

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

	// load fonts
    nk_sdl_font_stash_begin(&atlas);
    nk_sdl_font_stash_end();

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

    background = nk_rgb(28,48,62);
    while (!quit)
    {
        // Input
        SDL_Event evt;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT)
				goto cleanup;
            nk_sdl_handle_event(&evt);

			const Uint8 *key_state = SDL_GetKeyboardState(nullptr);

			if (evt.type == SDL_KEYDOWN) {
				switch (evt.key.keysym.sym) {
					case SDLK_SPACE: play_pause(); break;
				}
			}

			if (evt.type == SDL_KEYUP) {
				switch (evt.key.keysym.sym) {
					case SDLK_b: if (key_state[SDL_SCANCODE_LGUI]) split_clip(); break;
				}
			}
        }
        nk_input_end(ctx);

		// show a new frame if we're unpaused or we've seeked -- use pts to determine
		if (decoder.has_video() && (just_seeked || !paused)) {
			// allow seeking again if we're not showing the just seeked frame
			if (!just_seeked)
				last_seek_secs = -1;

			// decode a video frame
			// decoded_frame is unreffed by decoder
			AVFrame* decoded_frame = decoder.get_video_frame();
			if (decoded_frame != nullptr && decoded_frame->width != 0 && decoded_frame->height != 0) {
				just_seeked = false;
//printf("displaying frame with pts %lld\n", decoded_frame->pts);
				// find out which clip we're in -- TODO: keep track or otherwise improve this
				// this needs to change when there are multiple files / decoders
				// use frame clock for this?
				last_display_secs = av_q2d(decoder.get_video_stream()->time_base) * decoded_frame->pts;
				// same code used in split_clip()
				auto clip_it = std::find_if(clips.begin(), clips.end(),
						[](const Clip& clip) { return clip.start_secs <= last_display_secs && last_display_secs < clip.start_secs + clip.duration_secs; });
				if (clip_it == clips.end()) {
					printf("cannot find clip to use\n");
					break;
				}

				int ret = clip_it->feed(decoded_frame);
				if (ret != 0) {
					printf("error feeding the clip filter\n");
					break;
				}

				sws_ctx = sws_getCachedContext(sws_ctx, decoded_frame->width, decoded_frame->height, (enum AVPixelFormat) decoded_frame->format,
						video_w, video_h, AV_PIX_FMT_RGB24,
						SWS_BICUBIC, NULL, NULL, NULL);
				if (rgb_frame == nullptr) {
					// rgb_frame is reused and unreffed at end of program
					rgb_frame = av_frame_alloc();
					av_image_alloc(rgb_frame->data, rgb_frame->linesize, video_w, video_h, AV_PIX_FMT_RGB24, 1);
				}

				AVFrame* filtered_frame = clip_it->get_output_frame();
				sws_scale(sws_ctx, filtered_frame->data, filtered_frame->linesize, 0, filtered_frame->height, rgb_frame->data, rgb_frame->linesize);

				glBindTexture(GL_TEXTURE_2D, frame_texture);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_w, video_h, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame->data[0]);

				// delay until next video frame and render
				int video_delay_msec = video_clock->GetDelayMsec(decoded_frame);
				if (video_delay_msec > 0)
					SDL_Delay(video_delay_msec);
			}
		}

        // GUI
        if (nk_begin(ctx, "Demo", nk_rect(0, 0, win_width, win_height), 0)) {
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

			nk_layout_row_dynamic(ctx, 50, 1);
			widget_clips_bar(ctx);
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

