#include <chrono>
#include <list>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

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

#include "logger.h"
#include "common.h"
#include "clip.h"
#ifdef __APPLE__
#include "mac.h"
#endif

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

#define MAX_VERTEX_MEMORY 512 * 1024
#define MAX_ELEMENT_MEMORY 128 * 1024

typedef std::chrono::high_resolution_clock timer_clock;

bool paused = true;
bool just_seeked = true;
float last_frame_secs = 0;
timer_clock::time_point last_frame_clock = timer_clock::now();
float last_seek_secs = 0;
float clips_bar_last_click_secs = -1;

struct nk_font_atlas *atlas;

AVFrame* rgb_frame;
SDL_AudioDeviceID audio_device = 1; // will never be 1 due to SDL docs
SDL_AudioSpec audio_spec;

Video video;

#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

void play()
{
	paused = false;
	SDL_PauseAudioDevice(audio_device, 0);
	last_frame_clock = timer_clock::now();
}

void pause()
{
	paused = true;
	SDL_PauseAudioDevice(audio_device, 1);
}

void play_pause()
{
	paused ? play() : pause();
}

void seek(float seek_secs) {
	just_seeked = video.seek(seek_secs);
	last_frame_secs = seek_secs;
}

void split_clip()
{
	if (clips_bar_last_click_secs == -1)
		return;
	video.main_track.split(clips_bar_last_click_secs, TransitionEffect::Fade);
}

void load_test_scenario()
{
	video.addToMainTrack("concert.mp4", TransitionEffect::None);
	video.addToOverlayTrack("overlay.mov", TransitionEffect::None);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	//seek(1.0f);
	//video.main_track.split(1, TransitionEffect::Fade);
	play();
}

void audio_callback(void*, Uint8 *stream, int len)
{
	// unfreed memory
	static uint8_t* audio_buf = new uint8_t[MAX_AUDIO_FRAME_SIZE * 2];
	static unsigned int audio_buf_size = 0;

	int copied = 0;

	Logger::get("audio") << "requested " << len << " bytes for audio stream\n";
	while (len > 0) {
		// check whether we have enough stored
		if (audio_buf_size >= len) {
			Logger::get("audio") << "copying " << len << " bytes to stream, have " << audio_buf_size - len << " left\n";
			// copy from buffer to stream
			memcpy(&stream[copied], audio_buf, len);
			// move remainder to beginning of stream
			memmove(audio_buf, &audio_buf[len], audio_buf_size - len);
			audio_buf_size -= len;
			break;
		}

		// copy whatever we have saved
		if (audio_buf_size > 0) {
			Logger::get("audio") << "copying the remaining " << audio_buf_size << " bytes to stream\n";
			// copy from buffer to stream
			memcpy(stream, audio_buf, audio_buf_size);
			len -= audio_buf_size;
			copied += audio_buf_size;
			audio_buf_size = 0;
		}

		// read another frame and store it in the buf
		int ret = video.get_next_audio_frame();
		if (ret < 0) {
			Logger::get("audio") << "unable to get audio frame: " << av_err2str(ret) << "\n";
			memset(&stream[copied], 0, len);
			break;
		}

		AVFrame* audio_frame = video.out_audio_frame;
		audio_buf_size = av_samples_get_buffer_size(NULL, audio_frame->channels, audio_frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
		memcpy(audio_buf, audio_frame->data[0], audio_buf_size);
	}

	Logger::get("audio") << "---\n";
}

void open_audio()
{
	SDL_AudioSpec desired;
	memset(&desired, 0, sizeof(desired));

	// audio
	desired.freq = 44100;
	//desired.freq = 48000;
	desired.format = AUDIO_S16SYS;
	desired.channels = 2;
	desired.silence = 0;
	desired.samples = 1024;
	desired.callback = audio_callback;
	audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &audio_spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
	if (audio_device == 0) {
		Logger::get("error") << "SDL_OpenAudio: " << SDL_GetError() << "\n";
		return;
	}
	Logger::get("audio") << "received sample rate " << audio_spec.freq << ", " << (char)(audio_spec.channels + '0') << " channels, format " << audio_spec.format << "\n";
}

void widget_clips_bar(struct nk_context *ctx)
{
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);

	struct nk_rect space;
	enum nk_widget_layout_states widget_state = nk_widget(&space, ctx);
	if (!widget_state)
		return;

	float video_duration = video.get_duration_secs();
	//float pixels_per_sec = space.w / video_duration;
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

	int max_height = 28;
	int color_num = 0;
	for (auto piece_it = video.main_track.pieces.begin(); piece_it != video.main_track.pieces.end(); ++piece_it) {
		float start = space.w * piece_it->file.video_start_secs / video_duration + 1; // add 1 for line thickness
		float width = space.w * piece_it->file.duration_secs / video_duration - 10; // dunno why need to subtract 9
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
	int current_x = space.x + space.w * video.get_last_video_frame_secs() / video_duration;
	nk_stroke_line(canvas, current_x, space.y, current_x, space.y + space.h, 1, nk_rgb(100,100,100));

	// handle seeks via click or drag
	if (widget_state != NK_WIDGET_ROM) {
		if (nk_input_has_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, space, nk_true)) {
			float mouse_x = ctx->input.mouse.pos.x;
			float mouse_x_pct = (mouse_x - space.x) / space.w;
			float seek_secs = mouse_x_pct * video_duration;

			if (just_seeked || last_seek_secs != seek_secs) {
				seek(seek_secs);
				last_seek_secs = seek_secs;
				clips_bar_last_click_secs = seek_secs;
			}
		}
	}

	// border
	//nk_stroke_rect(canvas, space, 0, 1, nk_rgb(200,200,200));
}

int main(int argc, char* argv[])
{
	Logger::addCategory("error");
	//Logger::addCategory("audio");
	//Logger::addCategory("clip_recalc");
	//Logger::addCategory("get_video_frame");
	//Logger::addCategory("get_audio_frame");
	//Logger::addCategory("realtime");
	//Logger::addCategory("filter");
	//Logger::addCategory("decoder");
	Logger::addCategory("overlay");

	// video decoder
	av_register_all();
	avfilter_register_all();

    // Platform
    SDL_Window *win;
    SDL_GLContext glContext;
    struct nk_color background;
    int win_width, win_height;

    // GUI
    struct nk_context *ctx;

    // SDL setup
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER|SDL_INIT_EVENTS);
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
		Logger::get("error") << "Failed to setup GLEW\n";
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

	// start with black texture
	uint8_t* black_frame = new uint8_t[video_w * video_h * 3];
	memset(black_frame, 0, video_w * video_h * 3);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_w, video_h, GL_RGB, GL_UNSIGNED_BYTE, black_frame);
	delete[] black_frame;

	load_test_scenario();

	// set up audio
	if (audio_device == 1) {
		open_audio();
		if (!paused)
			SDL_PauseAudioDevice(audio_device, 0);
	}

    background = nk_rgb(28,48,62);
    while (true)
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

		// show next frame if not paused and we've waited long enough or if just seeked
		if (just_seeked || !paused) {
			if (just_seeked)
				last_frame_clock = timer_clock::now();

			// advance frame if not paused unless just seeked
			if (!just_seeked && !paused) {
				auto now = timer_clock::now();
				Logger::get("realtime") << "adding " << std::chrono::duration_cast<std::chrono::duration<float>>(now - last_frame_clock).count() << "s\n";
				last_frame_secs += std::chrono::duration_cast<std::chrono::duration<float>>(now - last_frame_clock).count();
				last_frame_clock = now;
			}
			just_seeked = false;

			// realtime frame logging
			static auto start = std::chrono::high_resolution_clock::now();
			auto rt_now = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> duration = rt_now - start;
			Logger::get("realtime") << "asking for frame at " << std::setprecision(4) << last_frame_secs << "s at " << duration.count() << "s, diff " << duration.count() - last_frame_secs << "s\n";

			int ret = video.get_video_frame(last_frame_secs, video_w, video_h);
			if (ret == 0) {
				glBindTexture(GL_TEXTURE_2D, frame_texture);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_w, video_h, GL_RGB, GL_UNSIGNED_BYTE, video.out_video_frame->data[0]);
			}
		}

        // GUI
        if (nk_begin(ctx, "Demo", nk_rect(0, 0, win_width, win_height), 0)) {
            nk_menubar_begin(ctx);
            nk_layout_row_begin(ctx, NK_STATIC, 25, 2);
            nk_layout_row_push(ctx, 45);
            if (nk_menu_begin_label(ctx, "FILE", NK_TEXT_LEFT, nk_vec2(120, 200))) {
                nk_layout_row_dynamic(ctx, 30, 1);
                if (nk_menu_item_label(ctx, "ADD VIDEO", NK_TEXT_LEFT)) {
					std::string new_video = path();
					if (new_video.size() > 0) {
						video.addToMainTrack(new_video, TransitionEffect::Fade);
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
				}
                if (nk_menu_item_label(ctx, "ADD OVERLAY", NK_TEXT_LEFT)) {
					std::string new_video = path();
					if (new_video.size() > 0) {
						video.addToOverlayTrack(new_video, TransitionEffect::Fade);
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
				}
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
	av_frame_free(&rgb_frame);

    nk_sdl_shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(win);
    SDL_Quit();

    return 0;
}

