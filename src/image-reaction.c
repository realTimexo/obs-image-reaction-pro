//
// Created by scaled
//
// Based on image-source.c from OBS Studio: https://github.com/obsproject/obs-studio
// Also included some code from Spectralizer plugin: https://github.com/univrsal/spectralizer
//

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <media-io/audio-math.h>
#include <plugin-support.h>


struct image_reaction_source {
	obs_source_t *source;
	char source_name[255];

	char *file1;
	char *file2;
	bool persistent;
	bool linear_alpha;
	bool active;

	gs_image_file4_t if41;
	gs_image_file4_t if42;

	obs_weak_source_t *audio_source;

	bool loud;
	float threshold;
	float smoothness;
	float average;

	uint64_t last_time;
	uint64_t capture_check_time;

	bool animReset1;
	bool animReset2;
	bool loudOld;
	bool animResetTrigger;

	/* Blinking feature */
	bool blink_enabled;
	char *blink_silent_file;
	char *blink_speaking_file;
	bool blink_anim_reset;
	float blink_interval;   /* average time between blinks, seconds */
	float blink_variation;  /* random jitter around the interval, 0..0.95 */
	float blink_duration;   /* how long a blink stays fully visible, seconds */
	float blink_smoothness; /* crossfade time in/out of the blink, seconds */

	gs_image_file4_t if_blink_silent;
	gs_image_file4_t if_blink_speaking;

	bool blink_rng_seeded;
	bool blink_active;
	float blink_wait_timer;
	float blink_active_timer;
	float blink_next_wait;
	float blink_alpha;
	bool blink_reset_trigger;
};

/*int MAX(int a, int b) {
	return a > b ? a : b;
}*/

#define MIN(a,b) ((a)<(b) ? (a):(b))
#define MAX(a,b) ((a)>(b) ? (a):(b))

/* Custom effect used to draw the blink image on top of the base image with
 * an animatable opacity (see data/effects/image_opacity.effect). */
static gs_effect_t *blink_opacity_effect = NULL;

static const char *image_reaction_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ImageReactionSource");
}

/* Picks a randomized wait time around blink_interval, jittered by
 * +/- blink_variation (a fraction of the interval), so blinking doesn't
 * look mechanical. */
static float image_reaction_random_blink_wait(struct image_reaction_source *context)
{
	float base = MAX(context->blink_interval, 0.1f);

	if (!context->blink_rng_seeded) {
		srand((unsigned int)os_gettime_ns());
		context->blink_rng_seeded = true;
	}

	if (context->blink_variation <= 0.0f)
		return base;

	float span = base * context->blink_variation;
	float lo = MAX(0.1f, base - span);
	float hi = base + span;
	float r = (float)rand() / (float)RAND_MAX;

	return lo + r * (hi - lo);
}

static void image_reaction_source_load(struct image_reaction_source *context)
{
	char *files[4] = {
		context->file1,
		context->file2,
		context->blink_silent_file,
		context->blink_speaking_file,
	};
	gs_image_file4_t *if4s[4] = {
		&context->if41,
		&context->if42,
		&context->if_blink_silent,
		&context->if_blink_speaking,
	};

	for (int i = 0; i <= 3; i++) {
		char *file = files[i];
		gs_image_file4_t *if4 = if4s[i];

		obs_enter_graphics();
		gs_image_file4_free(if4);
		obs_leave_graphics();

		if (file && *file) {
			obs_log(LOG_DEBUG, "loading texture '%s'", file);
			gs_image_file4_init(if4, file,
					    context->linear_alpha
						    ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB
						    : GS_IMAGE_ALPHA_PREMULTIPLY);

			obs_enter_graphics();
			gs_image_file4_init_texture(if4);
			obs_leave_graphics();

			if (!if4->image3.image2.image.loaded)
				obs_log(LOG_WARNING, "failed to load texture '%s'", file);
		}
	}
}

static void image_reaction_source_unload(struct image_reaction_source *context)
{
	obs_enter_graphics();
	gs_image_file4_free(&context->if41);
	gs_image_file4_free(&context->if42);
	gs_image_file4_free(&context->if_blink_silent);
	gs_image_file4_free(&context->if_blink_speaking);
	obs_leave_graphics();
}

static void audio_capture(void *param, obs_source_t *src, const struct audio_data *data, bool muted)
{
    (void)src;
	struct image_reaction_source *context = param;
	
	if (muted) {
		context->average = 0;
	}
	else
	{
		uint32_t samplesCount = data->frames;
		float* samples = (float*)data->data[0];
		
		float averageLocal = 0.0f;
		
		for (uint32_t i = 0; i < samplesCount; i++) {
			averageLocal += fabsf(samples[i]) / samplesCount;
		}
		
		context->average += context->smoothness * (averageLocal - context->average);
	}
	
	context->loudOld = context->loud;
	context->loud = context->average > context->threshold;
	
	if (context->loud != context->loudOld)
		context->animResetTrigger = true;
}

static void image_reaction_source_update(void *data, obs_data_t *settings)
{
	struct image_reaction_source *context = data;
	const char *file1 = obs_data_get_string(settings, "file1");
	const char *file2 = obs_data_get_string(settings, "file2");
	const bool anim_reset_1 = obs_data_get_bool(settings, "anim_reset_1");
	const bool anim_reset_2 = obs_data_get_bool(settings, "anim_reset_2");
	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const float threshold = (float)obs_data_get_double(settings, "threshold");
	const float smoothness = (float)obs_data_get_double(settings, "smoothness");

	const bool blink_enabled = obs_data_get_bool(settings, "blink_enabled");
	const char *blink_silent = obs_data_get_string(settings, "blink_silent");
	const char *blink_speaking = obs_data_get_string(settings, "blink_speaking");
	const bool blink_anim_reset = obs_data_get_bool(settings, "blink_anim_reset");
	const float blink_interval = (float)obs_data_get_double(settings, "blink_interval");
	const float blink_variation = (float)obs_data_get_double(settings, "blink_variation") / 100.0f;
	const float blink_duration = (float)obs_data_get_double(settings, "blink_duration");
	const float blink_smoothness = (float)obs_data_get_double(settings, "blink_smoothness");

	if (context->file1)
		bfree(context->file1);
	context->file1 = bstrdup(file1);
	
	if (context->file2)
		bfree(context->file2);
	context->file2 = bstrdup(file2);
	
	context->animReset1 = anim_reset_1;
	context->animReset2 = anim_reset_2;
	
	context->persistent = !unload;
	context->linear_alpha = linear_alpha;
	context->threshold = db_to_mul(threshold);
	context->smoothness = powf(0.1f, smoothness);

	if (context->blink_silent_file)
		bfree(context->blink_silent_file);
	context->blink_silent_file = bstrdup(blink_silent);

	if (context->blink_speaking_file)
		bfree(context->blink_speaking_file);
	context->blink_speaking_file = bstrdup(blink_speaking);

	const bool blink_was_enabled = context->blink_enabled;

	context->blink_enabled = blink_enabled;
	context->blink_anim_reset = blink_anim_reset;
	context->blink_interval = MAX(blink_interval, 0.1f);
	context->blink_variation = MIN(MAX(blink_variation, 0.0f), 0.95f);
	context->blink_duration = MAX(blink_duration, 0.01f);
	context->blink_smoothness = MAX(blink_smoothness, 0.0f);

	if (blink_enabled && !blink_was_enabled) {
		/* Start from a clean, non-blinking state whenever the feature
		 * gets (re-)enabled, so behavior is predictable. */
		context->blink_active = false;
		context->blink_wait_timer = 0.0f;
		context->blink_active_timer = 0.0f;
		context->blink_alpha = 0.0f;
		context->blink_next_wait = image_reaction_random_blink_wait(context);
	}

	/* Load the image if the source is persistent or showing */
	if (context->persistent || obs_source_showing(context->source))
		image_reaction_source_load(data);
	else
		image_reaction_source_unload(data);
	
	const char* cfg_source_name = obs_data_get_string(settings, "audio_source");
	
	obs_weak_source_t *old = NULL;
	
	if (cfg_source_name[0] == '\0') {
		if (context->audio_source) {
			old = context->audio_source;
			context->audio_source = NULL;
		}
		context->source_name[0] = '\0';
	}
	else {
		if (context->source_name[0] == '\0' || strcmp(context->source_name, cfg_source_name) != 0) {
			if (context->audio_source) {
				old = context->audio_source;
				context->audio_source = NULL;
			}
			strcpy(context->source_name, cfg_source_name);
			context->capture_check_time = os_gettime_ns() - 3000000000;
		}
	}

	if (old) {
		obs_source_t *old_source = obs_weak_source_get_source(old);
		if (old_source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(old_source));
			obs_source_remove_audio_capture_callback(old_source, audio_capture, context);
			obs_source_release(old_source);
		}
		obs_weak_source_release(old);
	}
}

static void image_reaction_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
        obs_data_set_default_string(settings, "audio_source", "");
        obs_data_set_default_double(settings, "threshold", -40.0);
        obs_data_set_default_double(settings, "smoothness", 1.0);

        obs_data_set_default_bool(settings, "blink_enabled", false);
        obs_data_set_default_bool(settings, "blink_anim_reset", true);
        obs_data_set_default_double(settings, "blink_interval", 10.0);
        obs_data_set_default_double(settings, "blink_variation", 40.0);
        obs_data_set_default_double(settings, "blink_duration", 0.18);
        obs_data_set_default_double(settings, "blink_smoothness", 0.08);
}

static void image_reaction_source_show(void *data)
{
	struct image_reaction_source *context = data;

	if (!context->persistent)
		image_reaction_source_load(context);
}

static void image_reaction_source_hide(void *data)
{
	struct image_reaction_source *context = data;

	if (!context->persistent)
		image_reaction_source_unload(context);
}

static void *image_reaction_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct image_reaction_source *context = bzalloc(sizeof(struct image_reaction_source));
	context->source = source;
	
	context->source_name[0] = '\0';
	context->loud = false;

	image_reaction_source_update(context, settings);
	return context;
}

static void image_reaction_source_destroy(void *data)
{
	struct image_reaction_source *context = data;

	image_reaction_source_unload(context);

	if (context->file1)
		bfree(context->file1);

	if (context->file2)
		bfree(context->file2);

	if (context->blink_silent_file)
		bfree(context->blink_silent_file);

	if (context->blink_speaking_file)
		bfree(context->blink_speaking_file);
	
	/*if (context->audio_source) {
		//obs_source_t *source = obs_weak_source_get_source(context->audio_source);
		//if (source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(context->audio_source));
			obs_source_remove_audio_capture_callback(context->audio_source, audio_capture, context);
			//obs_source_release(source);
		//}
		//obs_weak_source_release(context->audio_source);
	}*/
	if (context->audio_source) {
		obs_source_t *source = obs_weak_source_get_source(context->audio_source);
		if (source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(source));
			obs_source_remove_audio_capture_callback(source, audio_capture, context);
			obs_source_release(source);
		}
		obs_weak_source_release(context->audio_source);
	}
	
	bfree(context);
}

static uint32_t image_reaction_source_getwidth(void *data)
{
	struct image_reaction_source *context = data;
	uint32_t w = MAX(context->if41.image3.image2.image.cx, context->if42.image3.image2.image.cx);
	w = MAX(w, context->if_blink_silent.image3.image2.image.cx);
	w = MAX(w, context->if_blink_speaking.image3.image2.image.cx);
	return w;
}

static uint32_t image_reaction_source_getheight(void *data)
{
	struct image_reaction_source *context = data;
	uint32_t h = MAX(context->if41.image3.image2.image.cy, context->if42.image3.image2.image.cy);
	h = MAX(h, context->if_blink_silent.image3.image2.image.cy);
	h = MAX(h, context->if_blink_speaking.image3.image2.image.cy);
	return h;
}

static void image_reaction_source_render(void *data, gs_effect_t *effect)
{
	struct image_reaction_source *context = data;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	
	gs_image_file4_t *if4 = context->loud ? &context->if42 : &context->if41;
	if (if4->image3.image2.image.texture)
	{
		gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture_srgb(param, if4->image3.image2.image.texture);

		gs_draw_sprite(if4->image3.image2.image.texture, 0,
			       if4->image3.image2.image.cx,
			       if4->image3.image2.image.cy);
	}

	/* Draw the blink image on top, faded in/out by blink_alpha. */
	if (context->blink_enabled && context->blink_alpha > 0.001f && blink_opacity_effect) {
		gs_image_file4_t *blink_if = context->loud ? &context->if_blink_speaking : &context->if_blink_silent;

		if (blink_if->image3.image2.image.texture) {
			gs_eparam_t *const img_param = gs_effect_get_param_by_name(blink_opacity_effect, "image");
			gs_eparam_t *const opacity_param = gs_effect_get_param_by_name(blink_opacity_effect, "opacity");

			gs_effect_set_texture_srgb(img_param, blink_if->image3.image2.image.texture);
			gs_effect_set_float(opacity_param, context->blink_alpha);

			while (gs_effect_loop(blink_opacity_effect, "Draw")) {
				gs_draw_sprite(blink_if->image3.image2.image.texture, 0,
					       blink_if->image3.image2.image.cx,
					       blink_if->image3.image2.image.cy);
			}
		}
	}

	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void image_reaction_tick(void *data, float seconds)
{
	struct image_reaction_source *context = data;
	

	// Update / refresh audio capturing
	char* new_name = NULL;
	if (context->source_name[0] != '\0' && !context->audio_source) {
		uint64_t t = os_gettime_ns();

		if (t - context->capture_check_time > 3000000000) {
			new_name = context->source_name;
			context->capture_check_time = t;
		}
	}

	if (new_name != NULL) {
		obs_source_t *capture = obs_get_source_by_name(new_name);
		obs_weak_source_t *weak_capture = capture ? obs_source_get_weak_source(capture) : NULL;

		if (context->source_name[0] != '\0' && new_name == context->source_name) {
			context->audio_source = weak_capture;
			weak_capture = NULL;
		}

		if (capture) {
			obs_log(LOG_INFO, "Added audio capture to '%s'", obs_source_get_name(capture));
			obs_source_add_audio_capture_callback(capture, audio_capture, context);
			obs_weak_source_release(weak_capture);
			obs_source_release(capture);
		}
	}

	// Advance the blink state machine (timing + smoothed crossfade alpha)
	if (context->blink_enabled) {
		if (!context->blink_active) {
			context->blink_wait_timer += seconds;

			if (context->blink_wait_timer >= context->blink_next_wait) {
				context->blink_active = true;
				context->blink_wait_timer = 0.0f;
				context->blink_active_timer = 0.0f;
				context->blink_reset_trigger = true;
			}
		} else {
			context->blink_active_timer += seconds;

			if (context->blink_active_timer >= context->blink_duration) {
				context->blink_active = false;
				context->blink_active_timer = 0.0f;
				context->blink_next_wait = image_reaction_random_blink_wait(context);
			}
		}

		const float target = context->blink_active ? 1.0f : 0.0f;

		if (context->blink_smoothness <= 0.0001f) {
			context->blink_alpha = target;
		} else {
			const float step = seconds / context->blink_smoothness;

			if (context->blink_alpha < target)
				context->blink_alpha = MIN(target, context->blink_alpha + step);
			else if (context->blink_alpha > target)
				context->blink_alpha = MAX(target, context->blink_alpha - step);
		}
	} else {
		context->blink_active = false;
		context->blink_wait_timer = 0.0f;
		context->blink_active_timer = 0.0f;
		context->blink_alpha = 0.0f;
	}
	
	// update GIF's
	uint64_t frame_time = obs_get_video_frame_time();
	if (obs_source_active(context->source)) {
		if (!context->active) {
			if (context->if41.image3.image2.image.is_animated_gif || context->if42.image3.image2.image.is_animated_gif ||
			    context->if_blink_silent.image3.image2.image.is_animated_gif || context->if_blink_speaking.image3.image2.image.is_animated_gif)
				context->last_time = frame_time;
			context->active = true;
		}

	} else {
		if (context->active) {
			for (int i = 0; i <=3; i++) {
				gs_image_file4_t *if4d = i == 0 ? &context->if41
							: i == 1 ? &context->if42
							: i == 2 ? &context->if_blink_silent
								 : &context->if_blink_speaking;
				if (if4d->image3.image2.image.is_animated_gif) {
					if4d->image3.image2.image.cur_frame = 0;
					if4d->image3.image2.image.cur_loop = 0;
					if4d->image3.image2.image.cur_time = 0;

					obs_enter_graphics();
					gs_image_file4_update_texture(if4d);
					obs_leave_graphics();
				}
			}

			context->active = false;
		}
	}

	for (int i = 0; i <=3; i++) {
		gs_image_file4_t *if4d = i == 0 ? &context->if41
					: i == 1 ? &context->if42
					: i == 2 ? &context->if_blink_silent
						 : &context->if_blink_speaking;
		bool animReset = i == 0 ? context->animReset1
				: i == 1 ? context->animReset2
					 : context->blink_anim_reset;
		bool resetTrigger = i <= 1 ? context->animResetTrigger : context->blink_reset_trigger;
		

		if (context->last_time && if4d->image3.image2.image.is_animated_gif) {
			if (animReset && resetTrigger) {
				if4d->image3.image2.image.cur_frame = 0;
				if4d->image3.image2.image.cur_loop = 0;
				if4d->image3.image2.image.cur_time = 0;

				obs_enter_graphics();
				gs_image_file4_update_texture(if4d);
				obs_leave_graphics();
			}
			else {
				uint64_t elapsed = frame_time - context->last_time;
				bool updated = gs_image_file4_tick(if4d, elapsed);

				if (updated) {
					obs_enter_graphics();
					gs_image_file4_update_texture(if4d);
					obs_leave_graphics();
				}
			}
		}
	}
	context->animResetTrigger = false;
	context->blink_reset_trigger = false;

	context->last_time = frame_time;
}

static const char *image_filter =
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
	"BMP Files (*.bmp);;"
	"Targa Files (*.tga);;"
	"PNG Files (*.png);;"
	"JPEG Files (*.jpeg *.jpg);;"
	"GIF Files (*.gif);;"
	"PSD Files (*.psd);;"
	"WebP Files (*.webp);;"
	"All Files (*.*)";

static bool add_source(void* param, obs_source_t* src)
{
    obs_property_t *list = param;
    
    uint32_t caps = obs_source_get_output_flags(src);

    if ((caps & OBS_SOURCE_AUDIO) == 0)
        return true;
    const char *name = obs_source_get_name(src);
    obs_property_list_add_string(list, name, name);
    return true;
}

static bool source_changed(obs_properties_t *props, obs_property_t * prop, obs_data_t *data)
{
    (void)props;
    (void)prop;
    obs_data_get_string(data, "audio_source");
    return true;
}

static obs_properties_t *image_reaction_source_properties(void *data)
{
	struct image_reaction_source *s = data;
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	if (s && s->file1 && *s->file1) {
		const char *slash;

		dstr_copy(&path, s->file1);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "file1", obs_module_text("Reaction1"),
				OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_1",
				obs_module_text("AnimReset1"));
	obs_properties_add_path(props, "file2", obs_module_text("Reaction2"),
				OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_2",
				obs_module_text("AnimReset2"));
	
	obs_properties_add_bool(props, "unload",
				obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha",
				obs_module_text("LinearAlpha"));
	obs_property_t* sources_list = obs_properties_add_list(props, "audio_source",
				obs_module_text("AudioSource"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources_list, "", "");
				
	obs_property_t *p = obs_properties_add_float_slider(props, "threshold",
		obs_module_text("Threshold"), -60.0, 0.0, 0.1);
		obs_property_float_set_suffix(p, " dB");
	
	obs_properties_add_float_slider(props, "smoothness",
		obs_module_text("Smoothness"), 0.0, 5.0, 0.1);

	/* --- Blinking (optional) --- */
	obs_properties_t *blink_group = obs_properties_create();

	obs_properties_add_path(blink_group, "blink_silent", obs_module_text("BlinkSilent"),
				OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_path(blink_group, "blink_speaking", obs_module_text("BlinkSpeaking"),
				OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(blink_group, "blink_anim_reset",
				obs_module_text("BlinkAnimReset"));

	obs_property_t *interval_p = obs_properties_add_float_slider(blink_group, "blink_interval",
		obs_module_text("BlinkInterval"), 1.0, 60.0, 0.5);
	obs_property_float_set_suffix(interval_p, " s");

	obs_property_t *variation_p = obs_properties_add_float_slider(blink_group, "blink_variation",
		obs_module_text("BlinkVariation"), 0.0, 90.0, 1.0);
	obs_property_float_set_suffix(variation_p, " %");

	obs_property_t *duration_p = obs_properties_add_float_slider(blink_group, "blink_duration",
		obs_module_text("BlinkDuration"), 0.05, 1.0, 0.01);
	obs_property_float_set_suffix(duration_p, " s");

	obs_property_t *smooth_p = obs_properties_add_float_slider(blink_group, "blink_smoothness",
		obs_module_text("BlinkSmoothness"), 0.0, 1.0, 0.01);
	obs_property_float_set_suffix(smooth_p, " s");

	obs_properties_add_group(props, "blink_enabled", obs_module_text("BlinkEnabled"),
				OBS_GROUP_CHECKABLE, blink_group);

	dstr_free(&path);
	
	//obs_property_set_modified_callback(src, source_changed);
	obs_enum_sources(add_source, sources_list);
	
	return props;
}

uint64_t image_reaction_source_get_memory_usage(void *data)
{
	struct image_reaction_source *s = data;
	return s->if41.image3.image2.mem_usage + s->if42.image3.image2.mem_usage +
	       s->if_blink_silent.image3.image2.mem_usage + s->if_blink_speaking.image3.image2.mem_usage;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct image_reaction_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static struct obs_source_info image_reaction_source_info = {
	.id = "image_reaction_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = image_reaction_source_get_name,
	.create = image_reaction_source_create,
	.destroy = image_reaction_source_destroy,
	.update = image_reaction_source_update,
	.get_defaults = image_reaction_source_defaults,
	.show = image_reaction_source_show,
	.hide = image_reaction_source_hide,
	.get_width = image_reaction_source_getwidth,
	.get_height = image_reaction_source_getheight,
	.video_render = image_reaction_source_render,
	.video_tick = image_reaction_tick,
	.get_properties = image_reaction_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("image-reaction", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Image reaction source";
}

extern struct obs_source_info slideshow_info;

bool obs_module_load(void)
{
	obs_enter_graphics();
	char *effect_path = obs_module_file("effects/image_opacity.effect");
	if (effect_path) {
		char *error_string = NULL;
		blink_opacity_effect = gs_effect_create_from_file(effect_path, &error_string);
		if (!blink_opacity_effect) {
			obs_log(LOG_WARNING, "Failed to load blink opacity effect from '%s': %s",
				effect_path, error_string ? error_string : "unknown error");
		}
		bfree(error_string);
		bfree(effect_path);
	} else {
		obs_log(LOG_WARNING, "Could not resolve path for blink opacity effect");
	}
	obs_leave_graphics();

	obs_register_source(&image_reaction_source_info);
	return true;
}

void obs_module_unload(void)
{
	obs_enter_graphics();
	if (blink_opacity_effect) {
		gs_effect_destroy(blink_opacity_effect);
		blink_opacity_effect = NULL;
	}
	obs_leave_graphics();
}
